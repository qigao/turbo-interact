#include "turbo_interact/streamgraph_bridge.h"
#include "turbo_interact/m17e_fact.h"
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


/* A full actual Bridge MUST cause the canonical Media Owner Dispatcher
 * to retain its TAKEN Portal credit for explicit reconciliation. Neither
 * the Provider result nor the pinned AnalysisRequest is silently retried,
 * settled or re-emitted; store/queue/codec owners remain distinct.
 *
 * Existing ti_bridge_test on ALL native platforms, no extra fixture target.
 */
static tm_ex_source_result terminal_test_source(void *ctx, uint8_t *bytes,
                                               size_t cap, size_t *written) {
    (void)ctx;(void)bytes;(void)cap;
    *written=0u;
    return TM_EX_SOURCE_EOS;
}
static int terminal_test_visitor(void *ctx,const tm_fs_frame_ref_v1 *ref,
                                  const tm_fs_cpu_view_v1 *pixels) {
    (void)ctx;(void)ref;(void)pixels;
    return 0;
}
static tm_sg_port_v1 terminal_test_port(const char *name,uint32_t direction) {
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=name;p.direction=direction;
    p.kind=TM_SG_VIDEO_FRAME;p.format_id=1u;
    p.memory_domain=TM_SG_MEMORY_CPU;
    p.timebase_num=1u;p.timebase_den=1000000u;
    return p;
}
static void test_terminal_queue_full_remains_pending(void) {
    const uint8_t bytes[6]={3u,2u,1u,4u,5u,6u};
    tm_fs_store *store=create_store();
    tm_fs_frame_ref_v1 ref=TM_FS_FRAME_REF_V1_INIT;
    tm_fs_frame_metadata_v1 meta=metadata_for(11u);
    ti_sg_bridge_context_v1 ctx;
    ti_bridge_config_v1 bcfg=TI_BRIDGE_CONFIG_V1_INIT;
    ti_bridge_payload_limits_v1 budget=TI_BRIDGE_PAYLOAD_LIMITS_V1_INIT;
    ti_bridge *bridge=NULL;
    ti_event_header_v1 request=header(TI_EVENT_ANALYSIS_REQUEST,123u,0u);
    ti_bridge_ticket_v1 prepared={0},taken={0};
    ti_bridge_event_v1 stored=TI_BRIDGE_EVENT_V1_INIT;
    ti_sg_terminal_entry_v1 entry=TI_SG_TERMINAL_ENTRY_V1_INIT;
    ti_sg_terminal_delivery_v1 terminal_ctx=TI_SG_TERMINAL_DELIVERY_V1_INIT;
    tm_sg_port_v1 source_port=terminal_test_port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 intermediate[2]={
        TM_SG_PORT_V1_INIT,TM_SG_PORT_V1_INIT};
    tm_sg_port_v1 sink_port=terminal_test_port("in",TM_SG_INPUT);
    tm_sg_node_v1 nodes[3]={
        TM_SG_NODE_V1_INIT,TM_SG_NODE_V1_INIT,TM_SG_NODE_V1_INIT};
    tm_sg_edge_v1 edges[2]={
        TM_SG_EDGE_V1_INIT,TM_SG_EDGE_V1_INIT};
    tm_sg_program *program=NULL;
    tm_ex_config_v1 ex_cfg=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 binds[3]={
        TM_EX_BINDING_V1_INIT,TM_EX_BINDING_V1_INIT,TM_EX_BINDING_V1_INIT};
    tm_ex_owner *executor=NULL;
    tm_ho_inbox *inbox=NULL;
    tm_ho_config_v1 hcfg=TM_HO_CONFIG_V1_INIT;
    tm_od_owner *owner=NULL;
    tm_od_config_v1 ocfg=TM_OD_CONFIG_V1_INIT;
    tm_pc_portal *portal=NULL;
    tm_pc_config_v1 pcfg=TM_PC_CONFIG_V1_INIT;
    tm_pc_ticket_v1 ticket={0};
    tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
    tm_pc_result_v1 invalid=TM_PC_RESULT_V1_INIT;
    tm_fs_frame_metadata_v1 still_pinned=TM_FS_FRAME_METADATA_V1_INIT;

    assert(tm_fs_publish_cpu_with_metadata(store,bytes,sizeof(bytes),
                                            77u,4u,&meta,&ref)==TM_FS_OK);
    ctx=context_for(store,&ref);
    bcfg.capacity=1u;bcfg.incarnation=430u;
    budget.max_message_bytes=sizeof(ti_sg_analysis_terminal_v1);
    budget.max_inflight_bytes=budget.max_message_bytes;
    assert(ti_sg_bridge_configure(&ctx,&bcfg)==TI_OK);
    assert(ti_bridge_create_with_payload_limits(&bcfg,&budget,&bridge)==TI_OK);
    assert(ti_sg_bridge_prepare_analysis(&ctx,bridge,&ref,&request,NULL,0u,
                                          &prepared)==TI_OK);
    assert(ti_bridge_commit(bridge,prepared)==TI_OK);
    assert(ti_bridge_take(bridge,&taken,&stored)==TI_OK);
    assert(taken.slot==prepared.slot && stored.header.event_id==123u);
    assert(tm_fs_retire(store,&ref)==TM_FS_EBUSY);

    intermediate[0]=terminal_test_port("in",TM_SG_INPUT);
    intermediate[1]=terminal_test_port("out",TM_SG_OUTPUT);
    nodes[0].id="source";nodes[0].kind=TM_SG_SOURCE;
    nodes[0].ports=&source_port;nodes[0].port_count=1u;
    nodes[1].id="process";nodes[1].kind=TM_SG_PROCESSOR;
    nodes[1].ports=intermediate;nodes[1].port_count=2u;
    nodes[2].id="sink";nodes[2].kind=TM_SG_SINK;
    nodes[2].ports=&sink_port;nodes[2].port_count=1u;
    edges[0].source_node="source";edges[0].source_port="out";
    edges[0].target_node="process";edges[0].target_port="in";
    edges[1].source_node="process";edges[1].source_port="out";
    edges[1].target_node="sink";edges[1].target_port="in";
    assert(tm_sg_compile(nodes,3u,edges,2u,NULL,&program,NULL)==TM_SG_OK);
    ex_cfg.max_frame_bytes=8u;
    ex_cfg.source_session_generation=77u;ex_cfg.display_generation=4u;
    ex_cfg.fanout.branch_count=1u;
    ex_cfg.fanout.branches[0]=
        (tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
    ex_cfg.fanout.branches[0].policy=TM_FO_ORDERED_LOSSLESS;
    ex_cfg.fanout.branches[0].queue_depth=1u;
    binds[0].node_id="source";binds[0].source_next=terminal_test_source;
    binds[1].node_id="process";binds[1].visit=terminal_test_visitor;
    binds[2].node_id="sink";binds[2].visit=terminal_test_visitor;
    assert(tm_ex_create(program,store,&ex_cfg,binds,3u,&executor)==TM_EX_OK);
    hcfg.capacity=1u;
    assert(tm_ho_create(&hcfg,&inbox)==TM_HO_OK);
    assert(tm_od_create(inbox,executor,store,&ocfg,&owner)==TM_OD_OK);
    pcfg.capacity=1u;pcfg.provider_generation=66u;
    assert(tm_pc_create(&pcfg,&portal)==TM_PC_OK);
    entry.correlation=123u;entry.result_event_id=124u;
    entry.request_ticket=taken;
    terminal_ctx.source_context=&ctx;
    terminal_ctx.bridge=bridge;
    terminal_ctx.plan_generation=3u;terminal_ctx.provider_generation=66u;
    terminal_ctx.result_schema_id=67u;
    terminal_ctx.entries=&entry;terminal_ctx.entry_count=1u;
    assert(tm_od_bind_completion(owner,portal,ti_sg_bridge_emit_terminal,
                                 &terminal_ctx)==TM_OD_OK);
    invalid.outcome=TM_PC_COMPLETED;
    invalid.completion_observed=1u;
    invalid.correlation=123u;
    invalid.provider_generation=67u; /* Wrong provider generation rejected. */
    assert(ti_sg_bridge_emit_terminal(&terminal_ctx,&invalid)!=0);
    assert(entry.attempted==0u);

    assert(tm_pc_begin(portal,123u,&ticket)==TM_PC_OK);
    assert(tm_pc_complete(portal,ticket,0)==TM_PC_OK);
    assert(tm_od_step_completion(owner,&result)==TM_OD_EDELIVERY);
    assert(result.outcome==TM_PC_COMPLETED && result.correlation==123u);
    assert(entry.attempted==1u);
    assert(tm_od_pending_completion(owner,&result)==TM_OD_OK);
    assert(tm_od_step_completion(owner,&result)==TM_OD_EBUSY);
    assert(tm_od_destroy(owner)==TM_OD_EBUSY);
    assert(ti_bridge_take(bridge,&prepared,&stored)==TI_EBUSY);
    assert(tm_fs_get_metadata(store,&ref,&still_pinned)==TM_FS_OK);

    /* Host now explicitly acknowledges that no downstream effect committed.
     * No replay is permitted on the old Portal ticket or old request. */
    assert(tm_od_settle_pending_completion(owner)==TM_OD_OK);
    assert(tm_od_pending_completion(owner,&result)==TM_OD_EMPTY);
    assert(tm_pc_complete(portal,ticket,0)==TM_PC_ESTALE);
    assert(tm_pc_seal(portal)==TM_PC_OK);
    assert(tm_ho_seal(inbox)==TM_HO_OK);
    assert(tm_od_destroy(owner)==TM_OD_OK);
    assert(tm_pc_destroy(portal)==TM_PC_OK);
    assert(tm_ho_destroy(inbox)==TM_HO_OK);
    assert(tm_ex_stop(executor)==TM_EX_OK);
    assert(tm_ex_destroy(executor)==TM_EX_OK);
    assert(ti_bridge_settle(bridge,taken)==TI_OK);
    assert(ti_bridge_settle(bridge,taken)==TI_ESTALE);
    assert(tm_fs_get_metadata(store,&ref,&still_pinned)==TM_FS_ESTALE);
    assert(ti_bridge_seal(bridge)==TI_OK);
    assert(ti_bridge_destroy(bridge)==TI_OK);
    assert(tm_fs_seal(store)==TM_FS_OK);
    assert(tm_fs_destroy(store)==TM_FS_OK);
    tm_sg_program_destroy(program);
}


/* The canonical 160-byte TIFA fact is independent of host C11 packing,
 * raw FrameRef and FramePin. This test runs on *all* existing native
 * Linux/macOS/Windows/ASan product CI builds with the real Store SDK. */
static void test_m17e_canonical_terminal_fact_codec(void) {
    ti_m17e_terminal_fact_v1 fact=TI_M17E_TERMINAL_FACT_V1_INIT;
    ti_m17e_terminal_fact_v1 restored=TI_M17E_TERMINAL_FACT_V1_INIT;
    uint8_t wire[TI_M17E_TERMINAL_WIRE_BYTES]={0};
    size_t used=0u;
    fact.outcome=TM_PC_CANCELLED;
    fact.completion_observed=1u;
    fact.provider_status=-99;
    fact.schema_id=67u;
    fact.event_id=221u;fact.correlation=211u;
    fact.plan_generation=19u;
    fact.source_session_generation=77u;
    fact.device_session_id=43u;
    fact.device_session_generation=6u;
    fact.display_id=7u;
    fact.geometry_generation=5u;
    fact.capture_session_hi=88u;fact.capture_session_lo=99u;
    fact.stream_generation=12u;
    fact.packet_sequence=11u;fact.provider_generation=55u;
    fact.pts=33000u;fact.dts=33000u;
    fact.timebase_num=1u;fact.timebase_den=1000000u;
    assert(ti_m17e_terminal_fact_validate(&fact)==TI_OK);
    assert(ti_m17e_terminal_fact_encode(&fact,wire,sizeof(wire)-1u,
                                         &used)==TI_ERANGE);
    assert(used==0u);
    assert(ti_m17e_terminal_fact_encode(&fact,wire,sizeof(wire),
                                         &used)==TI_OK);
    assert(used==160u && memcmp(wire,"TIFA",4u)==0);
    assert(wire[4]==0u && wire[5]==1u &&
           wire[6]==0u && wire[7]==160u);
    /* The last bytes are a schema and explicit zero reserved word. */
    assert(wire[152]==0u && wire[155]==67u &&
           wire[156]==0u && wire[159]==0u);
    assert(ti_m17e_terminal_fact_decode(wire,used,&restored)==TI_OK);
    assert(restored.outcome==TM_PC_CANCELLED &&
           restored.provider_status==-99 &&
           restored.correlation==211u && restored.event_id==221u &&
           restored.device_session_id==43u &&
           restored.capture_session_hi==88u &&
           restored.packet_sequence==11u && restored.pts==33000u);
    {
        ti_m17e_fact_scope_v1 scope=TI_M17E_FACT_SCOPE_V1_INIT;
        scope.plan_generation=19u;
        scope.source_session_generation=77u;
        scope.provider_generation=55u;
        scope.device_session_id=43u;
        scope.device_session_generation=6u;
        scope.display_id=7u;
        scope.geometry_generation=5u;
        scope.capture_session_hi=88u;
        scope.capture_session_lo=99u;
        scope.stream_generation=12u;
        assert(ti_m17e_terminal_fact_match_scope(&restored,&scope)==TI_OK);
        scope.geometry_generation++;
        assert(ti_m17e_terminal_fact_match_scope(&restored,&scope)==TI_ESTALE);
        scope.geometry_generation--;
        scope.provider_generation++;
        assert(ti_m17e_terminal_fact_match_scope(&restored,&scope)==TI_ESTALE);
        scope.provider_generation--;
        scope.capture_session_lo++;
        assert(ti_m17e_terminal_fact_match_scope(&restored,&scope)==TI_ESTALE);
        scope.capture_session_lo--;
        scope.stream_generation++;
        assert(ti_m17e_terminal_fact_match_scope(&restored,&scope)==TI_ESTALE);
        scope.stream_generation--;
        scope.source_session_generation++;
        assert(ti_m17e_terminal_fact_match_scope(&restored,&scope)==TI_ESTALE);
        scope.source_session_generation--;
        scope.plan_generation++;
        assert(ti_m17e_terminal_fact_match_scope(&restored,&scope)==TI_ESTALE);
        scope.plan_generation--;
        assert(ti_m17e_terminal_fact_match_scope(&restored,&scope)==TI_OK);
    }
    wire[76]=1u; /* no unknown future ABI bits */
    assert(ti_m17e_terminal_fact_decode(wire,used,&restored)==TI_ECONFLICT);
    wire[76]=0u;
    wire[12]=0xffu; /* unknown outcome */
    assert(ti_m17e_terminal_fact_decode(wire,used,&restored)==TI_EINVAL);
    wire[12]=0u;
    wire[156]=1u;
    assert(ti_m17e_terminal_fact_decode(wire,used,&restored)==TI_ECONFLICT);
    wire[156]=0u;
    fact.outcome=TM_PC_COMPLETED;fact.completion_observed=0u;
    assert(ti_m17e_terminal_fact_validate(&fact)==TI_EINVAL);
    fact.outcome=TM_PC_START_FAILED;fact.completion_observed=1u;
    assert(ti_m17e_terminal_fact_validate(&fact)==TI_EINVAL);
    fact.outcome=TM_PC_CANCELLED;fact.completion_observed=1u;
    fact.dts=33001u;
    assert(ti_m17e_terminal_fact_validate(&fact)==TI_EINVAL);
}
void test_real_turbomedia_frame_store_bridge(void) {
    test_real_store_observation_and_two_pinned_requests();
    test_generation_and_unannotated_reject_before_pin();
    test_terminal_queue_full_remains_pending();
    test_m17e_canonical_terminal_fact_codec();
    puts("real TurboMedia FrameStore / TurboInteract GraphBridge: PASS");
}
