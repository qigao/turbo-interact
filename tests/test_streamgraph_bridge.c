#include "turbo_interact/streamgraph_bridge.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* This runs inside the EXISTING ti_bridge_test native product binary,
 * linked to the actual installed TurboMedia::StreamGraph CPU FrameStore.
 * No fake Store, installer/consumer project, source clone or extra test target.
 */
typedef struct {
    unsigned calls;
    const uint8_t *source;
    size_t source_bytes;
    int reject;
} visit_result;

static ti_status inspect_real_frame(void *ctx,
                                    const ti_bridge_event_v1 *event,
                                    const tm_fs_frame_metadata_v1 *metadata,
                                    const tm_fs_cpu_view_v1 *view) {
    visit_result *r=(visit_result *)ctx;
    assert(event->header.kind==TI_EVENT_ANALYSIS_REQUEST);
    assert(event->header.producer_sequence==11u);
    assert(event->has_frame==1u && event->pin_frame==1u);
    assert(metadata->device_session_id==43u);
    assert(metadata->geometry_generation==5u);
    assert(metadata->capture_session_hi==88u);
    assert(metadata->packet_sequence==11u && metadata->pts==33000u);
    assert(view->data && view->size_bytes==r->source_bytes);
    assert(memcmp(view->data,r->source,r->source_bytes)==0);
    r->calls++;
    return r->reject ? TI_EAUTH : TI_OK;
}
static ti_sg_bridge_context_v1 context_for(
    tm_fs_store *store, const tm_fs_frame_ref_v1 *ref) {
    ti_sg_bridge_context_v1 ctx=TI_SG_BRIDGE_CONTEXT_V1_INIT;
    ctx.store=store;
    ctx.store_id=ref->store_id;
    ctx.store_generation=ref->store_generation;
    ctx.source_session_generation=ref->source_session_generation;
    ctx.display_generation=ref->display_generation;
    ctx.device_session_id=43u;
    ctx.device_session_generation=6u;
    ctx.display_id=7u;
    ctx.geometry_generation=5u;
    ctx.capture_session_hi=88u;
    ctx.capture_session_lo=99u;
    ctx.stream_generation=12u;
    return ctx;
}
static tm_fs_frame_metadata_v1 metadata_for(uint64_t sequence) {
    tm_fs_frame_metadata_v1 m=TM_FS_FRAME_METADATA_V1_INIT;
    m.width=2u;m.height=2u;m.format_id=1u;
    m.device_session_id=43u;m.session_generation=6u;
    m.display_id=7u;m.source_session_generation=77u;
    m.display_generation=4u;m.geometry_generation=5u;
    m.capture_session_hi=88u;m.capture_session_lo=99u;
    m.stream_generation=12u;m.packet_sequence=sequence;
    m.pts=33000u;m.dts=33000u;
    m.timebase_num=1u;m.timebase_den=1000000u;
    return m;
}
static tm_fs_store *create_store(void) {
    tm_fs_store *s=NULL;
    tm_fs_config_v1 c=TM_FS_CONFIG_V1_INIT;
    c.store_id=3001u;c.slot_capacity=2u;
    c.max_frame_bytes=8u;c.max_pins=8u;
    c.max_leases=4u;c.max_retained_bytes=16u;
    assert(tm_fs_create(&c,&s)==TM_FS_OK);
    return s;
}
static ti_bridge *make_bridge(ti_sg_bridge_context_v1 *ctx) {
    ti_bridge_config_v1 cfg=TI_BRIDGE_CONFIG_V1_INIT;
    ti_bridge_payload_limits_v1 lim=TI_BRIDGE_PAYLOAD_LIMITS_V1_INIT;
    ti_bridge *b=NULL;
    cfg.capacity=3u;cfg.incarnation=116u;
    lim.max_message_bytes=sizeof(ti_observation_v1);
    lim.max_inflight_bytes=sizeof(ti_observation_v1)*2u;
    assert(ti_sg_bridge_configure(ctx,&cfg)==TI_OK);
    assert(ti_bridge_create_with_payload_limits(&cfg,&lim,&b)==TI_OK);
    return b;
}
static ti_event_header_v1 header(uint32_t kind,uint64_t id,
                                 uint32_t payload_bytes) {
    ti_event_header_v1 e=TI_EVENT_HEADER_V1_INIT;
    e.kind=kind;e.event_id=id;e.plan_generation=3u;
    e.session_generation=77u;e.producer_sequence=11u;
    e.schema_id=42u;e.payload_bytes=payload_bytes;
    return e;
}

static void test_real_store_observation_and_two_pinned_requests(void) {
    const uint8_t pixels[6]={1u,2u,3u,4u,5u,6u};
    tm_fs_store *s=create_store();
    tm_fs_frame_ref_v1 ref=TM_FS_FRAME_REF_V1_INIT;
    tm_fs_frame_metadata_v1 m=metadata_for(11u);
    tm_fs_frame_metadata_v1 read=TM_FS_FRAME_METADATA_V1_INIT;
    ti_sg_bridge_context_v1 ctx;
    ti_frame_ref_v1 ti_ref=TI_FRAME_REF_V1_INIT;
    ti_bridge *b;
    ti_bridge_event_v1 taken=TI_BRIDGE_EVENT_V1_INIT;
    ti_bridge_event_v1 current=TI_BRIDGE_EVENT_V1_INIT;
    ti_bridge_ticket_v1 weak={0},a={0},b_ticket={0},t={0},extra={0};
    ti_observation_v1 obs=TI_OBSERVATION_V1_INIT;
    ti_observation_v1 result=TI_OBSERVATION_V1_INIT;
    ti_event_header_v1 observe=header(TI_EVENT_OBSERVATION,2u,sizeof(obs));
    ti_event_header_v1 request=header(TI_EVENT_ANALYSIS_REQUEST,3u,0u);
    ti_bridge_snapshot_v1 snap=TI_BRIDGE_SNAPSHOT_V1_INIT;
    visit_result visitor={0u,pixels,sizeof(pixels),0};
    size_t got=0u;

    assert(tm_fs_publish_cpu_with_metadata(s,pixels,sizeof(pixels),
                  77u,4u,&m,&ref)==TM_FS_OK);
    ctx=context_for(s,&ref);
    b=make_bridge(&ctx);
    assert(ti_sg_bridge_frame_ref(&ref,&ti_ref)==TI_OK);
    obs.frame=ti_ref;obs.observation_id=31u;obs.capture_sequence=11u;
    obs.output_schema_id=42u;obs.target_class_id=5u;
    obs.confidence_ppm=900000u;obs.width_normalized=1000000u;
    obs.height_normalized=1000000u;obs.model_sha256[0]=0xa3u;
    assert(ti_observation_validate(&obs)==TI_OK);

    /* An Observation is a deep-copied weak fact, NOT a Pin or input grant. */
    assert(ti_sg_bridge_prepare_observation(&ctx,b,&ref,&obs,&observe,&weak)==TI_OK);
    assert(ti_bridge_commit(b,weak)==TI_OK);
    obs.model_sha256[0]=0xffu; /* The accepted bytes MUST be immutable. */
    assert(ti_sg_bridge_prepare_analysis(&ctx,b,&ref,&request,NULL,0u,&a)==TI_OK);
    request.event_id=4u;
    assert(ti_sg_bridge_prepare_analysis(&ctx,b,&ref,&request,NULL,0u,
                                         &b_ticket)==TI_OK);
    request.event_id=5u;
    assert(ti_sg_bridge_prepare_analysis(&ctx,b,&ref,&request,NULL,0u,
                                         &extra)==TI_EFULL);
    assert(extra.incarnation==0u);
    assert(ti_bridge_commit(b,a)==TI_OK);
    assert(ti_bridge_commit(b,b_ticket)==TI_OK);
    assert(tm_fs_retire(s,&ref)==TM_FS_EBUSY); /* Two actual Store Pins. */

    assert(ti_bridge_take(b,&t,&taken)==TI_OK);
    assert(t.incarnation==weak.incarnation && t.slot==weak.slot);
    assert(ti_bridge_read_payload(b,t,&result,sizeof(result),&got)==TI_OK);
    assert(got==sizeof(result) && result.model_sha256[0]==0xa3u);
    assert(result.capture_sequence==11u && result.frame.frame_id==ref.frame_id);
    assert(ti_bridge_settle(b,t)==TI_OK);
    assert(ti_bridge_take(b,&t,&taken)==TI_OK);
    assert(t.slot==a.slot);
    assert(ti_bridge_taken_event(b,t,&current)==TI_OK);
    assert(current.header.event_id==3u && current.pin_frame==1u);
    assert(ti_sg_bridge_visit_pinned(&ctx,b,t,inspect_real_frame,&visitor)==TI_OK);
    assert(ti_bridge_settle(b,t)==TI_OK);
    assert(ti_sg_bridge_visit_pinned(&ctx,b,t,inspect_real_frame,&visitor)==TI_ESTALE);

    /* B's Pin retains original bytes+metadata after A settles. Copying a
     * ticket cannot mint another Pin, and retiring FrameRef cannot re-pin. */
    assert(tm_fs_get_metadata(s,&ref,&read)==TM_FS_OK);
    assert(tm_fs_reserve_pin(s,&ref,(tm_fs_pin_v1[1]){{0}})==TM_FS_EBUSY);
    assert(ti_bridge_take(b,&t,&taken)==TI_OK && t.slot==b_ticket.slot);
    visitor.reject=1;
    assert(ti_sg_bridge_visit_pinned(&ctx,b,t,inspect_real_frame,&visitor)==TI_EAUTH);
    assert(tm_fs_get_metadata(s,&ref,&read)==TM_FS_OK);
    assert(ti_bridge_settle(b,t)==TI_OK); /* Host reconciled failed visit. */
    assert(ti_bridge_settle(b,t)==TI_ESTALE);
    assert(tm_fs_get_metadata(s,&ref,&read)==TM_FS_ESTALE);
    assert(visitor.calls==2u);

    assert(ti_bridge_seal(b)==TI_OK);
    assert(ti_bridge_snapshot(b,&snap)==TI_OK && snap.drained==1u);
    assert(ti_bridge_destroy(b)==TI_OK);
    assert(tm_fs_seal(s)==TM_FS_OK);
    assert(tm_fs_destroy(s)==TM_FS_OK);
}

static void test_generation_and_unannotated_reject_before_pin(void) {
    const uint8_t pixels[6]={11u,12u,13u,14u,15u,16u};
    tm_fs_store *s=create_store();
    tm_fs_frame_ref_v1 ref=TM_FS_FRAME_REF_V1_INIT;
    tm_fs_frame_ref_v1 old=TM_FS_FRAME_REF_V1_INIT;
    tm_fs_frame_metadata_v1 m=metadata_for(11u);
    ti_sg_bridge_context_v1 ctx;
    ti_bridge_ticket_v1 ticket={0};
    ti_event_header_v1 request=header(TI_EVENT_ANALYSIS_REQUEST,18u,0u);
    ti_bridge *b;
    assert(tm_fs_publish_cpu_with_metadata(s,pixels,sizeof(pixels),
                                          77u,4u,&m,&ref)==TM_FS_OK);
    ctx=context_for(s,&ref);
    b=make_bridge(&ctx);
    ctx.geometry_generation++;  /* Stale host bind, not a new authority. */
    assert(ti_sg_bridge_prepare_analysis(&ctx,b,&ref,&request,NULL,0u,
                                          &ticket)==TI_ESTALE);
    assert(ticket.incarnation==0u);
    ctx.geometry_generation--;
    ctx.capture_session_hi++;
    assert(ti_sg_bridge_prepare_analysis(&ctx,b,&ref,&request,NULL,0u,
                                          &ticket)==TI_ESTALE);
    ctx.capture_session_hi--;
    request.producer_sequence++;
    assert(ti_sg_bridge_prepare_analysis(&ctx,b,&ref,&request,NULL,0u,
                                          &ticket)==TI_ESTALE);
    request.producer_sequence--;
    assert(tm_fs_retire(s,&ref)==TM_FS_OK);
    assert(ti_sg_bridge_prepare_analysis(&ctx,b,&ref,&request,NULL,0u,
                                          &ticket)==TI_ESTALE);
    old=ref;
    assert(tm_fs_publish_cpu(s,pixels,sizeof(pixels),77u,4u,&ref)==TM_FS_OK);
    assert(ti_sg_bridge_prepare_analysis(&ctx,b,&ref,&request,NULL,0u,
                                          &ticket)==TI_ESTALE); /* ENOMETA */
    assert(ti_sg_bridge_prepare_analysis(&ctx,b,&old,&request,NULL,0u,
                                          &ticket)==TI_ESTALE); /* ABA */
    assert(tm_fs_retire(s,&ref)==TM_FS_OK);
    assert(ti_bridge_seal(b)==TI_OK);
    assert(ti_bridge_destroy(b)==TI_OK);
    assert(tm_fs_seal(s)==TM_FS_OK);
    assert(tm_fs_destroy(s)==TM_FS_OK);
}

void test_real_turbomedia_frame_store_bridge(void) {
    test_real_store_observation_and_two_pinned_requests();
    test_generation_and_unannotated_reject_before_pin();
    puts("real TurboMedia FrameStore / TurboInteract GraphBridge: PASS");
}
