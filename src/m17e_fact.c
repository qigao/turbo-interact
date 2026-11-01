#include "turbo_interact/m17e_fact.h"
#include <string.h>

static void be16(uint8_t *p,uint16_t n) {
    p[0]=(uint8_t)(n>>8u);p[1]=(uint8_t)n;
}
static void be32(uint8_t *p,uint32_t n) {
    p[0]=(uint8_t)(n>>24u);p[1]=(uint8_t)(n>>16u);
    p[2]=(uint8_t)(n>>8u);p[3]=(uint8_t)n;
}
static void be64(uint8_t *p,uint64_t n) {
    be32(p,(uint32_t)(n>>32u));
    be32(p+4u,(uint32_t)n);
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0]<<8u)|p[1]);
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24u)|((uint32_t)p[1]<<16u)|
           ((uint32_t)p[2]<<8u)|(uint32_t)p[3];
}
static uint64_t get64(const uint8_t *p) {
    return ((uint64_t)get32(p)<<32u)|get32(p+4u);
}

ti_status ti_m17e_terminal_fact_validate(const ti_m17e_terminal_fact_v1 *f) {
    if (!f) return TI_EINVAL;
    if (f->version!=TI_M17E_FACT_ABI_V1) return TI_EVERSION;
    if (f->size!=sizeof(*f) || f->reserved ||
        !f->schema_id || !f->event_id || !f->correlation ||
        f->event_id==f->correlation || !f->plan_generation ||
        !f->source_session_generation || !f->device_session_id ||
        !f->device_session_generation || !f->display_id ||
        !f->geometry_generation ||
        (!f->capture_session_hi && !f->capture_session_lo) ||
        !f->stream_generation || !f->packet_sequence ||
        !f->provider_generation || f->timebase_num!=1u ||
        f->timebase_den!=1000000u || f->dts!=f->pts ||
        f->completion_observed>1u)
        return TI_EINVAL;
    if (f->outcome!=TM_PC_COMPLETED &&
        f->outcome!=TM_PC_CANCELLED &&
        f->outcome!=TM_PC_START_FAILED) return TI_EINVAL;
    if (f->outcome==TM_PC_COMPLETED && f->completion_observed!=1u)
        return TI_EINVAL;
    if (f->outcome==TM_PC_START_FAILED && f->completion_observed!=0u)
        return TI_EINVAL;
    return TI_OK;
}

ti_status ti_m17e_terminal_fact_match_scope(
    const ti_m17e_terminal_fact_v1 *f,
    const ti_m17e_fact_scope_v1 *scope) {
    ti_status rc;
    if (!scope) return TI_EINVAL;
    if (scope->version!=TI_M17E_FACT_ABI_V1) return TI_EVERSION;
    if (scope->size!=sizeof(*scope) || scope->reserved ||
        !scope->plan_generation || !scope->source_session_generation ||
        !scope->provider_generation || !scope->device_session_id ||
        !scope->device_session_generation || !scope->display_id ||
        !scope->geometry_generation ||
        (!scope->capture_session_hi && !scope->capture_session_lo) ||
        !scope->stream_generation) return TI_EINVAL;
    rc=ti_m17e_terminal_fact_validate(f);
    if (rc!=TI_OK) return rc;
    if (f->plan_generation!=scope->plan_generation ||
        f->source_session_generation!=scope->source_session_generation ||
        f->provider_generation!=scope->provider_generation ||
        f->device_session_id!=scope->device_session_id ||
        f->device_session_generation!=scope->device_session_generation ||
        f->display_id!=scope->display_id ||
        f->geometry_generation!=scope->geometry_generation ||
        f->capture_session_hi!=scope->capture_session_hi ||
        f->capture_session_lo!=scope->capture_session_lo ||
        f->stream_generation!=scope->stream_generation)
        return TI_ESTALE;
    return TI_OK;
}

ti_status ti_m17e_terminal_fact_from_taken(
    ti_sg_bridge_context_v1 *c,ti_bridge *bridge,
    ti_bridge_ticket_v1 taken,uint64_t plan,uint64_t provider,
    uint32_t schema,ti_m17e_terminal_fact_v1 *out) {
    ti_bridge_event_v1 event=TI_BRIDGE_EVENT_V1_INIT;
    ti_sg_analysis_terminal_v1 result={0};
    ti_m17e_terminal_fact_v1 fact=TI_M17E_TERMINAL_FACT_V1_INIT;
    ti_status rc;
    size_t bytes=0u;
    if (!c || !bridge || !out || !plan || !provider || !schema)
        return TI_EINVAL;
    if (out->version!=TI_M17E_FACT_ABI_V1) return TI_EVERSION;
    if (out->size!=sizeof(*out)) return TI_EINVAL;
    if (c->version!=TI_ABI_VERSION ||
        c->size!=sizeof(*c) || !c->store || !c->store_id ||
        !c->store_generation || !c->source_session_generation ||
        !c->display_generation || !c->device_session_id ||
        !c->device_session_generation || !c->display_id ||
        !c->geometry_generation ||
        (!c->capture_session_hi && !c->capture_session_lo) ||
        !c->stream_generation || c->reserved) return TI_EINVAL;
    rc=ti_bridge_taken_event(bridge,taken,&event);
    if (rc!=TI_OK) return rc;
    if (event.header.kind!=TI_EVENT_ANALYSIS_RESULT ||
        event.header.plan_generation!=plan ||
        event.header.session_generation!=c->source_session_generation ||
        event.header.schema_id!=schema ||
        event.header.payload_bytes!=sizeof(result) ||
        !event.header.correlation_id ||
        event.has_frame || event.pin_frame)
        return TI_ECONFLICT;
    rc=ti_bridge_read_payload(bridge,taken,&result,sizeof(result),&bytes);
    if (rc!=TI_OK) return rc;
    if (bytes!=sizeof(result) ||
        result.size!=sizeof(result) || result.version!=TI_ABI_VERSION ||
        result.reserved || result.reserved2 ||
        result.correlation!=event.header.correlation_id ||
        result.provider_generation!=provider ||
        result.source_packet_sequence!=event.header.producer_sequence ||
        result.frame.session_generation!=event.header.session_generation ||
        result.frame.display_generation!=c->display_generation ||
        result.frame.store_id!=c->store_id ||
        result.frame.store_generation!=c->store_generation ||
        result.device_session_id!=c->device_session_id ||
        result.device_session_generation!=c->device_session_generation ||
        result.display_id!=c->display_id ||
        result.geometry_generation!=c->geometry_generation ||
        result.capture_session_hi!=c->capture_session_hi ||
        result.capture_session_lo!=c->capture_session_lo ||
        result.stream_generation!=c->stream_generation ||
        ti_frame_ref_validate(&result.frame)!=TI_OK)
        return TI_ESTALE;
    fact.outcome=result.outcome;
    fact.completion_observed=result.completion_observed;
    fact.provider_status=result.provider_status;
    fact.schema_id=event.header.schema_id;
    fact.event_id=event.header.event_id;
    fact.correlation=event.header.correlation_id;
    fact.plan_generation=event.header.plan_generation;
    fact.source_session_generation=event.header.session_generation;
    fact.device_session_id=result.device_session_id;
    fact.device_session_generation=result.device_session_generation;
    fact.display_id=result.display_id;
    fact.geometry_generation=result.geometry_generation;
    fact.capture_session_hi=result.capture_session_hi;
    fact.capture_session_lo=result.capture_session_lo;
    fact.stream_generation=result.stream_generation;
    fact.packet_sequence=result.source_packet_sequence;
    fact.provider_generation=result.provider_generation;
    fact.pts=result.pts;fact.dts=result.dts;
    fact.timebase_num=result.timebase_num;
    fact.timebase_den=result.timebase_den;
    rc=ti_m17e_terminal_fact_validate(&fact);
    if (rc!=TI_OK) return rc;
    *out=fact;
    return TI_OK;
}

ti_status ti_m17e_terminal_fact_encode(
    const ti_m17e_terminal_fact_v1 *fact,
    void *destination,size_t cap,size_t *out_bytes) {
    uint8_t *p=(uint8_t *)destination;
    ti_status rc;
    if (!out_bytes) return TI_EINVAL;
    *out_bytes=0u;
    rc=ti_m17e_terminal_fact_validate(fact);
    if (rc!=TI_OK) return rc;
    if (!p) return TI_EINVAL;
    if (cap<TI_M17E_TERMINAL_WIRE_BYTES) return TI_ERANGE;
    memset(p,0,TI_M17E_TERMINAL_WIRE_BYTES);
    memcpy(p,"TIFA",4u);
    be16(p+4u,1u);
    be16(p+6u,TI_M17E_TERMINAL_WIRE_BYTES);
    be32(p+8u,TI_M17E_TERMINAL_WIRE_KIND);
    be32(p+12u,fact->outcome);
    be32(p+16u,fact->completion_observed);
    be32(p+20u,(uint32_t)fact->provider_status);
    be64(p+24u,fact->event_id);
    be64(p+32u,fact->correlation);
    be64(p+40u,fact->plan_generation);
    be64(p+48u,fact->source_session_generation);
    be64(p+56u,fact->device_session_id);
    be64(p+64u,fact->device_session_generation);
    be32(p+72u,fact->display_id);
    be64(p+80u,fact->geometry_generation);
    be64(p+88u,fact->capture_session_hi);
    be64(p+96u,fact->capture_session_lo);
    be64(p+104u,fact->stream_generation);
    be64(p+112u,fact->packet_sequence);
    be64(p+120u,fact->provider_generation);
    be64(p+128u,fact->pts);
    be64(p+136u,fact->dts);
    be32(p+144u,fact->timebase_num);
    be32(p+148u,fact->timebase_den);
    be32(p+152u,fact->schema_id);
    *out_bytes=TI_M17E_TERMINAL_WIRE_BYTES;
    return TI_OK;
}

ti_status ti_m17e_terminal_fact_decode(
    const void *source,size_t size,ti_m17e_terminal_fact_v1 *out) {
    const uint8_t *p=(const uint8_t *)source;
    ti_m17e_terminal_fact_v1 f=TI_M17E_TERMINAL_FACT_V1_INIT;
    uint32_t raw_status;
    ti_status rc;
    if (!source || !out) return TI_EINVAL;
    if (out->version!=TI_M17E_FACT_ABI_V1) return TI_EVERSION;
    if (out->size!=sizeof(*out)) return TI_EINVAL;
    if (size!=TI_M17E_TERMINAL_WIRE_BYTES ||
        memcmp(p,"TIFA",4u)!=0 || get16(p+4u)!=1u ||
        get16(p+6u)!=TI_M17E_TERMINAL_WIRE_BYTES ||
        get32(p+8u)!=TI_M17E_TERMINAL_WIRE_KIND ||
        get32(p+76u)!=0u || get32(p+156u)!=0u)
        return TI_ECONFLICT;
    f.outcome=get32(p+12u);
    f.completion_observed=get32(p+16u);
    raw_status=get32(p+20u);
    memcpy(&f.provider_status,&raw_status,sizeof(raw_status));
    f.event_id=get64(p+24u);
    f.correlation=get64(p+32u);
    f.plan_generation=get64(p+40u);
    f.source_session_generation=get64(p+48u);
    f.device_session_id=get64(p+56u);
    f.device_session_generation=get64(p+64u);
    f.display_id=get32(p+72u);
    f.geometry_generation=get64(p+80u);
    f.capture_session_hi=get64(p+88u);
    f.capture_session_lo=get64(p+96u);
    f.stream_generation=get64(p+104u);
    f.packet_sequence=get64(p+112u);
    f.provider_generation=get64(p+120u);
    f.pts=get64(p+128u);f.dts=get64(p+136u);
    f.timebase_num=get32(p+144u);
    f.timebase_den=get32(p+148u);
    f.schema_id=get32(p+152u);
    rc=ti_m17e_terminal_fact_validate(&f);
    if (rc!=TI_OK) return rc;
    *out=f;
    return TI_OK;
}
