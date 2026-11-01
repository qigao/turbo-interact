#include "turbo_interact/streamgraph_bridge.h"
#include <string.h>

/* REAL TurboMedia C11 status mapping. A transient full Store is capacity,
 * not an authorization grant or automatic retry. A copied weak FrameRef
 * never creates a Pin. Bridge + Store are owner-serialized, not MPSC. */
static ti_status frame_status(tm_fs_status s) {
    switch(s) {
        case TM_FS_OK: return TI_OK;
        case TM_FS_EINVAL: return TI_EINVAL;
        case TM_FS_EVERSION: return TI_EVERSION;
        case TM_FS_ENOMEM: return TI_ENOMEM;
        case TM_FS_EFULL: return TI_EFULL;
        case TM_FS_EEXHAUSTED: return TI_ERANGE;
        case TM_FS_ESTALE:
        case TM_FS_ENOMETA: return TI_ESTALE;
        case TM_FS_EBUSY:
        case TM_FS_ESHUTDOWN: return TI_EBUSY;
        default: return TI_ECONFLICT;
    }
}

static int context_valid(const ti_sg_bridge_context_v1 *c) {
    return c && c->size==sizeof(*c) && c->version==TI_ABI_VERSION &&
        c->store && c->store_id && c->store_generation &&
        c->source_session_generation && c->display_generation &&
        c->device_session_id && c->device_session_generation &&
        c->display_id && !c->reserved && c->geometry_generation &&
        (c->capture_session_hi || c->capture_session_lo) &&
        c->stream_generation;
}

ti_status ti_sg_bridge_frame_ref(const tm_fs_frame_ref_v1 *up,
                                  ti_frame_ref_v1 *out) {
    if (!up || !out) return TI_EINVAL;
    if (up->version!=TM_FS_ABI_V1) return TI_EVERSION;
    if (up->size!=sizeof(*up) || !up->store_id ||
        !up->store_generation || !up->slot_generation ||
        !up->frame_id || !up->source_session_generation ||
        !up->display_generation) return TI_EINVAL;
    *out=(ti_frame_ref_v1)TI_FRAME_REF_V1_INIT;
    out->store_id=up->store_id;
    out->store_generation=up->store_generation;
    out->slot=up->slot;
    out->slot_generation=up->slot_generation;
    out->frame_id=up->frame_id;
    out->session_generation=up->source_session_generation;
    out->display_generation=up->display_generation;
    return TI_OK;
}

static ti_status to_upstream(const ti_frame_ref_v1 *ref,
                             tm_fs_frame_ref_v1 *out) {
    ti_status rc;
    if (!ref || !out) return TI_EINVAL;
    rc=ti_frame_ref_validate(ref);
    if (rc!=TI_OK) return rc;
    *out=(tm_fs_frame_ref_v1)TM_FS_FRAME_REF_V1_INIT;
    out->store_id=ref->store_id;
    out->store_generation=ref->store_generation;
    out->slot=ref->slot;
    out->slot_generation=ref->slot_generation;
    out->frame_id=ref->frame_id;
    out->source_session_generation=ref->session_generation;
    out->display_generation=ref->display_generation;
    return TI_OK;
}

static ti_status validate_source(ti_sg_bridge_context_v1 *c,
                                  const tm_fs_frame_ref_v1 *ref,
                                  tm_fs_frame_metadata_v1 *out_meta) {
    tm_fs_frame_metadata_v1 meta=TM_FS_FRAME_METADATA_V1_INIT;
    tm_fs_status result;
    if (!context_valid(c) || !ref) return TI_EINVAL;
    if (ref->store_id!=c->store_id ||
        ref->store_generation!=c->store_generation ||
        ref->source_session_generation!=c->source_session_generation ||
        ref->display_generation!=c->display_generation)
        return TI_ESTALE;
    /* Source metadata must exist, and it MUST be the exact current slot,
     * not another frame with a reused slot or the same byte count.
     * Same Media Owner lane serializes weak validation and the reserve. */
    result=tm_fs_get_metadata(c->store,ref,&meta);
    if (result!=TM_FS_OK) return frame_status(result);
    if (tm_fs_validate_metadata(&meta)!=TM_FS_OK ||
        meta.device_session_id!=c->device_session_id ||
        meta.session_generation!=c->device_session_generation ||
        meta.display_id!=c->display_id ||
        meta.source_session_generation!=c->source_session_generation ||
        meta.display_generation!=c->display_generation ||
        meta.geometry_generation!=c->geometry_generation ||
        meta.capture_session_hi!=c->capture_session_hi ||
        meta.capture_session_lo!=c->capture_session_lo ||
        meta.stream_generation!=c->stream_generation)
        return TI_ESTALE;
    if (out_meta) *out_meta=meta;
    return TI_OK;
}

/* Bridge's u64 token is the ORIGINAL single FrameStore ticket ID.
 * Store/Generation are fixed in the borrowed context until Bridge drain.
 * We never mint additional rights from copying this numeric token. */
static ti_status reserve_pin(void *user,const ti_frame_ref_v1 *ref,
                              uint64_t *out_token) {
    ti_sg_bridge_context_v1 *c=(ti_sg_bridge_context_v1 *)user;
    tm_fs_frame_ref_v1 up=TM_FS_FRAME_REF_V1_INIT;
    tm_fs_pin_v1 pin={0};
    tm_fs_status fs;
    ti_status rc;
    if (!out_token) return TI_EINVAL;
    *out_token=0u;
    rc=to_upstream(ref,&up);
    if (rc!=TI_OK) return rc;
    rc=validate_source(c,&up,NULL);
    if (rc!=TI_OK) return rc;
    fs=tm_fs_reserve_pin(c->store,&up,&pin);
    if (fs!=TM_FS_OK) return frame_status(fs);
    if (!pin.ticket_id || pin.store_id!=c->store_id ||
        pin.store_generation!=c->store_generation) {
        if (pin.ticket_id) (void)tm_fs_release_pin(c->store,&pin);
        return TI_ECONFLICT;
    }
    *out_token=pin.ticket_id;
    return TI_OK;
}
static ti_status release_pin(void *user,uint64_t token) {
    const ti_sg_bridge_context_v1 *c=(const ti_sg_bridge_context_v1 *)user;
    tm_fs_pin_v1 pin;
    if (!context_valid(c) || !token) return TI_EINVAL;
    pin.store_id=c->store_id;
    pin.store_generation=c->store_generation;
    pin.ticket_id=token;
    return frame_status(tm_fs_release_pin(c->store,&pin));
}

ti_status ti_sg_bridge_configure(ti_sg_bridge_context_v1 *c,
                                 ti_bridge_config_v1 *cfg) {
    if (!context_valid(c) || !cfg) return TI_EINVAL;
    if (cfg->version!=TI_ABI_VERSION) return TI_EVERSION;
    if (cfg->size!=sizeof(*cfg) || cfg->reserved ||
        !cfg->capacity || !cfg->incarnation ||
        cfg->pin_reserve || cfg->pin_release || cfg->pin_ctx)
        return TI_EINVAL;
    cfg->pin_reserve=reserve_pin;
    cfg->pin_release=release_pin;
    cfg->pin_ctx=c;
    return TI_OK;
}

ti_status ti_sg_bridge_prepare_observation(
    ti_sg_bridge_context_v1 *c,ti_bridge *b,
    const tm_fs_frame_ref_v1 *frame,const ti_observation_v1 *obs,
    const ti_event_header_v1 *header,ti_bridge_ticket_v1 *out) {
    ti_frame_ref_v1 value=TI_FRAME_REF_V1_INIT;
    tm_fs_frame_metadata_v1 meta=TM_FS_FRAME_METADATA_V1_INIT;
    ti_bridge_event_v1 event=TI_BRIDGE_EVENT_V1_INIT;
    ti_status rc;
    if (!b || !obs || !header || !out) return TI_EINVAL;
    rc=ti_sg_bridge_frame_ref(frame,&value);
    if (rc!=TI_OK) return rc;
    rc=validate_source(c,frame,&meta);
    if (rc!=TI_OK) return rc;
    rc=ti_observation_validate(obs);
    if (rc!=TI_OK) return rc;
    rc=ti_event_header_validate(header);
    if (rc!=TI_OK) return rc;
    if (header->kind!=TI_EVENT_OBSERVATION ||
        header->session_generation!=c->source_session_generation ||
        header->producer_sequence!=meta.packet_sequence ||
        header->schema_id!=obs->output_schema_id ||
        header->payload_bytes!=sizeof(*obs) ||
        obs->capture_sequence!=meta.packet_sequence ||
        memcmp(&obs->frame,&value,sizeof(value))!=0)
        return TI_ESTALE;
    event.header=*header;
    event.frame=value;
    event.has_frame=1u; /* weak: no FrameStore pin */
    return ti_bridge_prepare_owned(b,&event,obs,sizeof(*obs),out);
}

ti_status ti_sg_bridge_prepare_analysis(
    ti_sg_bridge_context_v1 *c,ti_bridge *b,
    const tm_fs_frame_ref_v1 *frame,const ti_event_header_v1 *header,
    const void *payload,size_t payload_size,ti_bridge_ticket_v1 *out) {
    ti_frame_ref_v1 value=TI_FRAME_REF_V1_INIT;
    tm_fs_frame_metadata_v1 meta=TM_FS_FRAME_METADATA_V1_INIT;
    ti_bridge_event_v1 event=TI_BRIDGE_EVENT_V1_INIT;
    ti_status rc;
    if (!b || !header || !out) return TI_EINVAL;
    rc=ti_sg_bridge_frame_ref(frame,&value);
    if (rc!=TI_OK) return rc;
    rc=validate_source(c,frame,&meta);
    if (rc!=TI_OK) return rc;
    rc=ti_event_header_validate(header);
    if (rc!=TI_OK) return rc;
    if (header->kind!=TI_EVENT_ANALYSIS_REQUEST ||
        header->session_generation!=c->source_session_generation ||
        header->producer_sequence!=meta.packet_sequence ||
        header->payload_bytes!=payload_size)
        return TI_ESTALE;
    event.header=*header;event.frame=value;
    event.has_frame=1u;event.pin_frame=1u;
    /* prepare reserves an independent FrameStore Pin before commit. */
    return ti_bridge_prepare_owned(b,&event,payload,payload_size,out);
}

ti_status ti_sg_bridge_visit_pinned(
    ti_sg_bridge_context_v1 *c,ti_bridge *b,
    ti_bridge_ticket_v1 ticket,ti_sg_bridge_frame_visit_fn visit,void *user) {
    ti_bridge_event_v1 event=TI_BRIDGE_EVENT_V1_INIT;
    tm_fs_frame_ref_v1 frame=TM_FS_FRAME_REF_V1_INIT;
    tm_fs_frame_metadata_v1 meta=TM_FS_FRAME_METADATA_V1_INIT;
    tm_fs_lease_v1 lease={0};
    tm_fs_cpu_view_v1 view=TM_FS_CPU_VIEW_V1_INIT;
    tm_fs_pin_v1 pin;
    uint64_t token=0u;
    ti_status rc;
    tm_fs_status fs;
    if (!context_valid(c) || !b || !visit) return TI_EINVAL;
    /* Fetch the ACTUAL event stored in the taken row, not a caller-provided
     * FrameRef that could mismatch the reserved Pin's actual pixels. */
    rc=ti_bridge_taken_event(b,ticket,&event);
    if (rc!=TI_OK) return rc;
    if (!event.has_frame || !event.pin_frame ||
        event.header.kind!=TI_EVENT_ANALYSIS_REQUEST)
        return TI_ECONFLICT;
    rc=to_upstream(&event.frame,&frame);
    if (rc!=TI_OK) return rc;
    rc=validate_source(c,&frame,&meta);
    if (rc!=TI_OK) return rc;
    if (event.header.producer_sequence!=meta.packet_sequence ||
        event.header.session_generation!=meta.source_session_generation)
        return TI_ESTALE;
    rc=ti_bridge_borrow_pin_token(b,ticket,&token);
    if (rc!=TI_OK) return rc;
    pin.store_id=c->store_id;pin.store_generation=c->store_generation;
    pin.ticket_id=token;
    fs=tm_fs_acquire_pinned(c->store,&pin,&lease);
    if (fs!=TM_FS_OK) return frame_status(fs);
    fs=tm_fs_map_cpu(c->store,&lease,&view);
    if (fs==TM_FS_OK)
        rc=visit(user,&event,&meta,&view);
    else rc=frame_status(fs);
    /* Lease always released here; original Bridge Pin remains until settle.
     * No raw pixel view is published outside the bounded visit. */
    fs=tm_fs_release_lease(c->store,&lease);
    if (fs!=TM_FS_OK) return frame_status(fs);
    return rc;
}


/* M17d: map the canonical TurboMedia Provider Completion Portal's terminal
 * result through the EXISTING Media Owner's tm_od_step_completion callback.
 * No Provider thread calls Bridge, FrameStore, SCXML or ActionOwner. No
 * retries and no new queue: terminal acceptance is ti_bridge_commit().
 */
int ti_sg_bridge_emit_terminal(void *delivery_context,
                               const tm_pc_result_v1 *terminal) {
    const ti_sg_terminal_delivery_v1 *c=
        (const ti_sg_terminal_delivery_v1 *)delivery_context;
    const ti_sg_terminal_entry_v1 *binding=NULL;
    ti_bridge_event_v1 request=TI_BRIDGE_EVENT_V1_INIT;
    ti_bridge_event_v1 emitted=TI_BRIDGE_EVENT_V1_INIT;
    tm_fs_frame_ref_v1 upstream=TM_FS_FRAME_REF_V1_INIT;
    tm_fs_frame_metadata_v1 meta=TM_FS_FRAME_METADATA_V1_INIT;
    ti_sg_analysis_terminal_v1 result={0};
    ti_bridge_ticket_v1 ticket={0};
    ti_status rc;
    uint64_t token=0u;
    uint32_t i, matches=0u;

    if (!c || c->version!=TI_ABI_VERSION || c->size!=sizeof(*c) ||
        c->reserved || !context_valid(c->source_context) || !c->bridge ||
        !c->plan_generation || !c->provider_generation ||
        !c->result_schema_id || !c->entries ||
        !c->entry_count || c->entry_count>TI_SG_TERMINAL_MAX_ENTRIES ||
        !terminal || terminal->size!=sizeof(*terminal) ||
        terminal->version!=TM_PC_ABI_V1 || terminal->reserved ||
        !terminal->correlation ||
        terminal->provider_generation!=c->provider_generation ||
        (terminal->outcome!=TM_PC_COMPLETED &&
         terminal->outcome!=TM_PC_CANCELLED &&
         terminal->outcome!=TM_PC_START_FAILED) ||
        terminal->completion_observed>1u ||
        (terminal->outcome==TM_PC_COMPLETED &&
         terminal->completion_observed!=1u) ||
        (terminal->outcome==TM_PC_START_FAILED &&
         terminal->completion_observed!=0u))
        return -1;

    /* Caller freezes a bounded correlation ledger before starting Provider.
     * Duplicate identities must fail, not accidentally deliver two results. */
    for (i=0u;i<c->entry_count;i++) {
        const ti_sg_terminal_entry_v1 *entry=&c->entries[i];
        if (entry->version!=TI_ABI_VERSION ||
            entry->size!=sizeof(*entry) ||
            !entry->correlation || !entry->result_event_id ||
            entry->correlation==entry->result_event_id ||
            !entry->request_ticket.incarnation ||
            !entry->request_ticket.generation)
            return -1;
        if (entry->correlation==terminal->correlation) {
            binding=entry;matches++;
        }
    }
    if (matches!=1u) return -1;

    /* Do not trust a host-supplied FrameRef: always fetch the ACTUAL
     * pinned TAKEN Bridge row and validate original source generations. */
    rc=ti_bridge_taken_event(c->bridge,binding->request_ticket,&request);
    if (rc!=TI_OK || !request.has_frame || !request.pin_frame ||
        request.header.kind!=TI_EVENT_ANALYSIS_REQUEST ||
        request.header.event_id!=binding->correlation ||
        request.header.plan_generation!=c->plan_generation ||
        request.header.session_generation!=c->source_context->source_session_generation)
        return -1;
    rc=ti_bridge_borrow_pin_token(c->bridge,binding->request_ticket,&token);
    if (rc!=TI_OK || !token) return -1;
    rc=to_upstream(&request.frame,&upstream);
    if (rc!=TI_OK) return -1;
    rc=validate_source(c->source_context,&upstream,&meta);
    if (rc!=TI_OK || request.header.producer_sequence!=meta.packet_sequence)
        return -1;

    result.size=sizeof(result);result.version=TI_ABI_VERSION;
    result.outcome=terminal->outcome;
    result.completion_observed=terminal->completion_observed;
    result.provider_status=terminal->provider_status;
    result.correlation=terminal->correlation;
    result.provider_generation=terminal->provider_generation;
    result.source_packet_sequence=meta.packet_sequence;
    result.frame=request.frame; /* weak value; NO NEW PIN */
    result.device_session_id=meta.device_session_id;
    result.device_session_generation=meta.session_generation;
    result.display_id=meta.display_id;
    result.geometry_generation=meta.geometry_generation;
    result.capture_session_hi=meta.capture_session_hi;
    result.capture_session_lo=meta.capture_session_lo;
    result.stream_generation=meta.stream_generation;
    result.pts=meta.pts;result.dts=meta.dts;
    result.timebase_num=meta.timebase_num;
    result.timebase_den=meta.timebase_den;

    emitted.header=(ti_event_header_v1)TI_EVENT_HEADER_V1_INIT;
    emitted.header.kind=TI_EVENT_ANALYSIS_RESULT;
    emitted.header.event_id=binding->result_event_id;
    emitted.header.correlation_id=terminal->correlation;
    emitted.header.plan_generation=c->plan_generation;
    emitted.header.session_generation=meta.source_session_generation;
    emitted.header.producer_sequence=meta.packet_sequence;
    emitted.header.schema_id=c->result_schema_id;
    emitted.header.payload_bytes=(uint32_t)sizeof(result);
    /* This event holds an OWNED value-only payload; the original request
     * ticket owns the sole Pin until the host explicitly settles it. */
    rc=ti_bridge_prepare_owned(c->bridge,&emitted,&result,sizeof(result),
                               &ticket);
    if (rc!=TI_OK) return -1;
    rc=ti_bridge_commit(c->bridge,ticket);
    if (rc!=TI_OK) {
        /* No terminal was visible. Do not leave an extra prepared row.
         * If discard fails the host must reconcile before destruction. */
        (void)ti_bridge_discard(c->bridge,ticket);
        return -1;
    }
    return 0;
}
