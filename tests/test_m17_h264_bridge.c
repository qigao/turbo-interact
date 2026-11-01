/*
 * Cross-project product C11 test IN THE EXISTING ti_bridge_test executable.
 * Actual TurboMedia H264/OpenH264 Source -> canonical StreamGraph Executor
 * -> genuine CPU FrameStore -> TurboInteract typed Observation/GraphBridge.
 * Codec/StreamGraph source is the exact upstream pinned checkout, not copied
 * into TurboInteract as a production implementation. No device or sockets.
 */
#include "turbo_interact/streamgraph_bridge.h"
#include "turbo_interact/m17e_fact.h"
#include <turbo_streamgraph_executor.h>
#include <turbo_streamgraph_h264.h>
#include <turbo_codec.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { TI_M17_W=64,TI_M17_H=64,TI_M17_I420=64*64*3/2,TI_M17_CAP=512*1024 };

typedef struct m17_bridge_fixture {
    tm_fs_store *store;
    ti_bridge *bridge;
    ti_sg_bridge_context_v1 ctx;
    tm_fs_frame_ref_v1 ref;
    ti_bridge_ticket_v1 weak, analysis_a, analysis_b;
    uint8_t pixels[TI_M17_I420];
    unsigned processor_visits, sink_visits, analysis_visits;
} m17_bridge_fixture;

static tm_sg_port_v1 m17_port(const char *name,uint32_t dir) {
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=name;p.direction=dir;p.kind=TM_SG_VIDEO_FRAME;
    p.format_id=1u;p.memory_domain=TM_SG_MEMORY_CPU;
    p.timebase_num=1u;p.timebase_den=1000000u;
    return p;
}
static tm_sg_program *m17_program(void) {
    tm_sg_port_v1 source=m17_port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 process[2]={m17_port("in",TM_SG_INPUT),
                               m17_port("out",TM_SG_OUTPUT)};
    tm_sg_port_v1 sink=m17_port("in",TM_SG_INPUT);
    tm_sg_node_v1 nodes[3]={
        {sizeof(tm_sg_node_v1),TM_SG_ABI_VERSION,"source",TM_SG_SOURCE,1u,&source},
        {sizeof(tm_sg_node_v1),TM_SG_ABI_VERSION,"vision",TM_SG_PROCESSOR,2u,process},
        {sizeof(tm_sg_node_v1),TM_SG_ABI_VERSION,"preview",TM_SG_SINK,1u,&sink}};
    tm_sg_edge_v1 edges[2]={
        {sizeof(tm_sg_edge_v1),TM_SG_ABI_VERSION,"source","out","vision","in"},
        {sizeof(tm_sg_edge_v1),TM_SG_ABI_VERSION,"vision","out","preview","in"}};
    tm_sg_program *p=NULL;
    assert(tm_sg_compile(nodes,3u,edges,2u,NULL,&p,NULL)==TM_SG_OK);
    return p;
}
static ti_event_header_v1 m17_header(uint32_t kind,uint64_t event_id,
                                      uint64_t sequence,uint32_t payload_bytes) {
    ti_event_header_v1 h=TI_EVENT_HEADER_V1_INIT;
    h.kind=kind;h.event_id=event_id;h.plan_generation=3u;
    h.session_generation=77u;h.producer_sequence=sequence;
    h.schema_id=42u;h.payload_bytes=payload_bytes;
    return h;
}
static int m17_visit_processor(void *user,const tm_fs_frame_ref_v1 *frame,
                                const tm_fs_cpu_view_v1 *view) {
    m17_bridge_fixture *f=(m17_bridge_fixture *)user;
    tm_fs_frame_metadata_v1 meta=TM_FS_FRAME_METADATA_V1_INIT;
    ti_frame_ref_v1 ref=TI_FRAME_REF_V1_INIT;
    ti_observation_v1 obs=TI_OBSERVATION_V1_INIT;
    ti_event_header_v1 h;
    ti_bridge_config_v1 config=TI_BRIDGE_CONFIG_V1_INIT;
    ti_bridge_payload_limits_v1 limits=TI_BRIDGE_PAYLOAD_LIMITS_V1_INIT;
    assert(f->processor_visits==0u);
    assert(view->size_bytes==TI_M17_I420 && view->data);
    assert(tm_fs_get_metadata(f->store,frame,&meta)==TM_FS_OK);
    assert(meta.packet_sequence==1u && meta.pts==0u && meta.dts==0u);
    assert(meta.source_session_generation==77u &&
           meta.display_generation==4u && meta.geometry_generation==5u);
    memcpy(f->pixels,view->data,view->size_bytes);
    f->ref=*frame;
    f->ctx=(ti_sg_bridge_context_v1)TI_SG_BRIDGE_CONTEXT_V1_INIT;
    f->ctx.store=f->store;
    f->ctx.store_id=frame->store_id;
    f->ctx.store_generation=frame->store_generation;
    f->ctx.source_session_generation=77u;
    f->ctx.display_generation=4u;
    f->ctx.device_session_id=43u;
    f->ctx.device_session_generation=6u;
    f->ctx.display_id=7u;
    f->ctx.geometry_generation=5u;
    f->ctx.capture_session_hi=88u;
    f->ctx.capture_session_lo=99u;
    f->ctx.stream_generation=12u;
    config.capacity=3u;config.incarnation=99u;
    /* Result terminal has more provenance fields than the Observation.
     * The Bridge must pre-budget the LARGEST admitted typed message, not
     * silently grow its queue when a Provider Completion arrives. */
    limits.max_message_bytes=(uint32_t)(
        sizeof(ti_sg_analysis_terminal_v1)>sizeof(obs) ?
        sizeof(ti_sg_analysis_terminal_v1) : sizeof(obs));
    limits.max_inflight_bytes=2u*limits.max_message_bytes;
    assert(ti_sg_bridge_configure(&f->ctx,&config)==TI_OK);
    assert(ti_bridge_create_with_payload_limits(&config,&limits,&f->bridge)==TI_OK);

    assert(ti_sg_bridge_frame_ref(frame,&ref)==TI_OK);
    obs.frame=ref;obs.observation_id=40u;obs.capture_sequence=meta.packet_sequence;
    obs.output_schema_id=42u;obs.target_class_id=3u;
    obs.confidence_ppm=900000u;
    obs.width_normalized=1000000u;obs.height_normalized=1000000u;
    obs.model_sha256[0]=0xa3u;
    h=m17_header(TI_EVENT_OBSERVATION,10u,meta.packet_sequence,sizeof(obs));
    assert(ti_sg_bridge_prepare_observation(&f->ctx,f->bridge,frame,
                                             &obs,&h,&f->weak)==TI_OK);
    assert(ti_bridge_commit(f->bridge,f->weak)==TI_OK);
    h=m17_header(TI_EVENT_ANALYSIS_REQUEST,11u,meta.packet_sequence,0u);
    assert(ti_sg_bridge_prepare_analysis(&f->ctx,f->bridge,frame,&h,
                                          NULL,0u,&f->analysis_a)==TI_OK);
    assert(ti_bridge_commit(f->bridge,f->analysis_a)==TI_OK);
    h.event_id=12u;
    assert(ti_sg_bridge_prepare_analysis(&f->ctx,f->bridge,frame,&h,
                                          NULL,0u,&f->analysis_b)==TI_OK);
    assert(ti_bridge_commit(f->bridge,f->analysis_b)==TI_OK);
    f->processor_visits++;
    return 0;
}
static int m17_preview_sink(void *user,const tm_fs_frame_ref_v1 *frame,
                             const tm_fs_cpu_view_v1 *view) {
    m17_bridge_fixture *f=(m17_bridge_fixture *)user;
    assert(view->size_bytes==TI_M17_I420);
    assert(memcmp(view->data,f->pixels,TI_M17_I420)==0);
    assert(frame->frame_id==f->ref.frame_id);
    f->sink_visits++;
    return 0;
}
static ti_status m17_visit_analysis(void *user,
                                    const ti_bridge_event_v1 *event,
                                    const tm_fs_frame_metadata_v1 *meta,
                                    const tm_fs_cpu_view_v1 *pixels) {
    m17_bridge_fixture *f=(m17_bridge_fixture *)user;
    assert(event->header.kind==TI_EVENT_ANALYSIS_REQUEST);
    assert(event->frame.frame_id==f->ref.frame_id);
    assert(meta->packet_sequence==1u && meta->capture_session_hi==88u);
    assert(pixels->size_bytes==TI_M17_I420);
    assert(memcmp(pixels->data,f->pixels,TI_M17_I420)==0);
    f->analysis_visits++;
    return TI_OK;
}

/* Host-owned Provider worker: only the existing TurboMedia
 * ProviderCompletion portal is touched off-owner. There is NO inference
 * implementation here; the test qualifies the terminal/owner protocol. */
#include <pthread.h>
/* A real asynchronous CPU read-only Provider test: the Host lends
 * a pinned FrameLease pointer to a worker. The worker computes an actual
 * checksum over the original immutable TurboMedia pixels, without another
 * pixel copy and without any off-owner FrameStore or Bridge operation.
 * This is NOT a trained ML recognizer or Android task-success predicate. */
static uint64_t m17_checksum(const uint8_t *bytes,size_t n) {
    uint64_t sum=UINT64_C(1469598103934665603);
    size_t i;
    for(i=0u;i<n;i++) {
        sum^=bytes[i];
        sum*=UINT64_C(1099511628211);
    }
    return sum;
}
typedef struct m17_completion_worker {
    tm_pc_portal *portal;
    tm_pc_ticket_v1 ticket;
    int32_t provider_status;
    const uint8_t *borrowed_pixels;
    size_t pixel_count;
    uint64_t expected_digest;
    uint64_t observed_digest;
    tm_pc_status result;
} m17_completion_worker;
static void *m17_worker_complete(void *arg) {
    m17_completion_worker *w=(m17_completion_worker *)arg;
    if (w->borrowed_pixels) {
        w->observed_digest=m17_checksum(w->borrowed_pixels,w->pixel_count);
        assert(w->observed_digest==w->expected_digest);
    }
    /* Producer threads touch the canonical Portal only. No FrameStore
     * method, Bridge commit or ActionOwner call occurs off owner. */
    w->result=tm_pc_complete(w->portal,w->ticket,w->provider_status);
    return NULL;
}
static void m17_complete_from_real_worker(tm_pc_portal *portal,
                                          tm_pc_ticket_v1 ticket,
                                          int32_t status,
                                          const tm_fs_cpu_view_v1 *pixels,
                                          uint64_t expected) {
    m17_completion_worker worker={0};
    pthread_t thread;
    worker.portal=portal;
    worker.ticket=ticket;
    worker.provider_status=status;
    worker.borrowed_pixels=pixels?pixels->data:NULL;
    worker.pixel_count=pixels?pixels->size_bytes:0u;
    worker.expected_digest=expected;
    assert(pthread_create(&thread,NULL,m17_worker_complete,&worker)==0);
    assert(pthread_join(thread,NULL)==0);
    assert(worker.result==TM_PC_OK);
    if (pixels) assert(worker.observed_digest==expected);
}
static ti_sg_analysis_terminal_v1 m17_take_terminal(
    ti_sg_bridge_context_v1 *ctx,ti_bridge *bridge,
    uint64_t event_id,uint32_t outcome) {
    ti_bridge_ticket_v1 taken={0};
    ti_bridge_event_v1 event=TI_BRIDGE_EVENT_V1_INIT;
    ti_sg_analysis_terminal_v1 value={0};
    ti_m17e_terminal_fact_v1 fact=TI_M17E_TERMINAL_FACT_V1_INIT;
    ti_m17e_terminal_fact_v1 recovered=TI_M17E_TERMINAL_FACT_V1_INIT;
    uint8_t wire[TI_M17E_TERMINAL_WIRE_BYTES]={0};
    size_t bytes=0u,wire_bytes=0u;
    assert(ti_bridge_take(bridge,&taken,&event)==TI_OK);
    assert(event.header.kind==TI_EVENT_ANALYSIS_RESULT &&
           event.header.event_id==event_id &&
           event.header.correlation_id==(event_id-10u) &&
           event.header.schema_id==67u &&
           event.header.payload_bytes==sizeof(value) &&
           event.pin_frame==0u && event.has_frame==0u);
    assert(ti_bridge_read_payload(bridge,taken,&value,sizeof(value),&bytes)==TI_OK);
    assert(bytes==sizeof(value) && value.size==sizeof(value) &&
           value.version==TI_ABI_VERSION &&
           value.outcome==outcome && value.completion_observed==1u);
    assert(value.correlation==event_id-10u &&
           value.provider_generation==55u &&
           value.source_packet_sequence==1u &&
           value.geometry_generation==5u &&
           value.capture_session_hi==88u &&
           value.capture_session_lo==99u &&
           value.pts==0u && value.dts==0u &&
           value.timebase_num==1u && value.timebase_den==1000000u);
    /* Converting an ACTUAL TAKEN Provider terminal to a stable 160-byte
     * Flow/SCXML fact must not serialize raw FrameRef/pointer/padding. */
    assert(ti_m17e_terminal_fact_from_taken(ctx,bridge,taken,3u,55u,67u,
                                             &fact)==TI_OK);
    assert(fact.correlation==event_id-10u && fact.event_id==event_id);
    assert(fact.outcome==outcome && fact.provider_status==value.provider_status);
    assert(fact.source_session_generation==77u &&
           fact.device_session_id==43u && fact.geometry_generation==5u);
    assert(ti_m17e_terminal_fact_encode(&fact,wire,sizeof(wire),
                                          &wire_bytes)==TI_OK);
    assert(wire_bytes==160u && memcmp(wire,"TIFA",4u)==0 &&
           wire[6]==0u && wire[7]==160u);
    assert(ti_m17e_terminal_fact_decode(wire,wire_bytes,&recovered)==TI_OK);
    assert(recovered.event_id==fact.event_id &&
           recovered.correlation==fact.correlation &&
           recovered.packet_sequence==fact.packet_sequence &&
           recovered.provider_status==fact.provider_status &&
           recovered.outcome==fact.outcome);
    wire[156]=1u; /* reserved bits MUST NOT be silently carried to Flow */
    assert(ti_m17e_terminal_fact_decode(wire,sizeof(wire),&recovered)==TI_ECONFLICT);
    wire[156]=0u;
    assert(ti_m17e_terminal_fact_decode(wire,sizeof(wire)-1u,&recovered)==TI_ECONFLICT);
    assert(ti_bridge_settle(bridge,taken)==TI_OK);
    assert(ti_m17e_terminal_fact_from_taken(ctx,bridge,taken,3u,55u,67u,
                                             &recovered)==TI_ESTALE);
    assert(ti_bridge_settle(bridge,taken)==TI_ESTALE);
    return value;
}

void test_m17_real_codec_frame_to_turbointeract_bridge(void) {
    uint8_t *raw=(uint8_t *)malloc(TI_M17_I420);
    uint8_t *encoded=(uint8_t *)malloc(TI_M17_CAP);
    turbo_video_codec_config_t encode_cfg={0};
    turbo_codec_t *encoder=NULL;
    tm_sgh264_config_v1 source_cfg=TM_SGH264_CONFIG_V1_INIT;
    tm_sgh264_packet_v1 packet=TM_SGH264_PACKET_V1_INIT;
    turbo_encoded_frame_t packet_info={0};
    tm_sgh264_source *source=NULL;
    tm_fs_store *store=NULL;
    tm_fs_config_v1 fs=TM_FS_CONFIG_V1_INIT;
    tm_ex_config_v1 ex=TM_EX_CONFIG_V1_INIT;
    tm_sg_program *program=NULL;
    tm_ex_owner *executor=NULL;
    tm_ex_binding_v1 bindings[3]={
        TM_EX_BINDING_V1_INIT,TM_EX_BINDING_V1_INIT,TM_EX_BINDING_V1_INIT};
    size_t encoded_bytes=TI_M17_CAP,read=0u;
    tm_fs_frame_metadata_v1 check=TM_FS_FRAME_METADATA_V1_INIT;
    ti_bridge_ticket_v1 ticket={0};
    ti_bridge_event_v1 event=TI_BRIDGE_EVENT_V1_INIT;
    ti_observation_v1 received=TI_OBSERVATION_V1_INIT;
    m17_bridge_fixture f={0};
    tm_ho_inbox *inbox=NULL;
    tm_ho_config_v1 inbox_cfg=TM_HO_CONFIG_V1_INIT;
    tm_od_owner *media_owner=NULL;
    tm_od_config_v1 owner_cfg=TM_OD_CONFIG_V1_INIT;
    tm_pc_portal *portal=NULL;
    tm_pc_config_v1 portal_cfg=TM_PC_CONFIG_V1_INIT;
    tm_pc_ticket_v1 provider_a={0},provider_b={0},duplicate={0};
    tm_pc_result_v1 terminal=TM_PC_RESULT_V1_INIT;
    ti_bridge_ticket_v1 taken_a={0},taken_b={0};
    ti_sg_terminal_entry_v1 entries[2]={
        TI_SG_TERMINAL_ENTRY_V1_INIT,
        TI_SG_TERMINAL_ENTRY_V1_INIT};
    ti_sg_terminal_delivery_v1 delivery=TI_SG_TERMINAL_DELIVERY_V1_INIT;
    ti_sg_analysis_terminal_v1 outcome={0};
    ti_sg_async_borrow_v1 async_borrow=TI_SG_ASYNC_BORROW_V1_INIT;
    tm_fs_cpu_view_v1 borrowed_view=TM_FS_CPU_VIEW_V1_INIT;
    uint64_t expected_digest=0u;
    assert(raw && encoded);
    turbo_codec_registry_init();
    encode_cfg.width=TI_M17_W;encode_cfg.height=TI_M17_H;
    encode_cfg.framerate=30;encode_cfg.bitrate=400000;
    encode_cfg.keyframe_interval=1;encode_cfg.threads=1;
    encoder=turbo_codec_create_encoder("h264",&encode_cfg);
    assert(encoder);
    memset(raw,60,TI_M17_W*TI_M17_H);
    memset(raw+TI_M17_W*TI_M17_H,128,TI_M17_I420-TI_M17_W*TI_M17_H);
    turbo_codec_request_keyframe(encoder);
    assert(turbo_codec_encode(encoder,raw,TI_M17_I420,encoded,
                             &encoded_bytes,&packet_info)==TURBO_CODEC_OK);
    assert(packet_info.is_keyframe && encoded_bytes>0u);
    source_cfg.width=TI_M17_W;source_cfg.height=TI_M17_H;
    source_cfg.max_packet_bytes=TI_M17_CAP;source_cfg.decoded_format_id=1u;
    source_cfg.device_session_id=43u;source_cfg.session_generation=6u;
    source_cfg.display_id=7u;source_cfg.source_session_generation=77u;
    source_cfg.display_generation=4u;source_cfg.geometry_generation=5u;
    source_cfg.capture_session_hi=88u;source_cfg.capture_session_lo=99u;
    source_cfg.stream_generation=12u;
    assert(tm_sgh264_create(&source_cfg,&source)==TM_SGH264_OK);
    packet.framing=TM_SGH264_FRAMING_ANNEX_B;
    packet.flags=TM_SGH264_PACKET_KEY_FRAME;
    packet.bytes=encoded;packet.bytes_length=encoded_bytes;
    packet.metadata=(tm_fs_frame_metadata_v1)TM_FS_FRAME_METADATA_V1_INIT;
    packet.metadata.width=TI_M17_W;packet.metadata.height=TI_M17_H;
    packet.metadata.format_id=1u;packet.metadata.flags=TM_FS_META_FLAG_KEY_FRAME;
    packet.metadata.device_session_id=43u;packet.metadata.session_generation=6u;
    packet.metadata.display_id=7u;packet.metadata.source_session_generation=77u;
    packet.metadata.display_generation=4u;packet.metadata.geometry_generation=5u;
    packet.metadata.capture_session_hi=88u;packet.metadata.capture_session_lo=99u;
    packet.metadata.stream_generation=12u;packet.metadata.packet_sequence=1u;
    packet.metadata.pts=0u;packet.metadata.dts=0u;
    packet.metadata.timebase_num=1u;packet.metadata.timebase_den=1000000u;
    assert(tm_sgh264_submit(source,&packet)==TM_SGH264_FRAME_READY);
    fs.store_id=3001u;fs.slot_capacity=2u;fs.max_frame_bytes=TI_M17_I420;
    fs.max_pins=8u;fs.max_leases=8u;
    fs.max_retained_bytes=2u*TI_M17_I420;
    assert(tm_fs_create(&fs,&store)==TM_FS_OK);
    f.store=store;
    program=m17_program();
    ex.max_frame_bytes=TI_M17_I420;
    ex.source_session_generation=77u;ex.display_generation=4u;
    ex.fanout.branch_count=1u;
    ex.fanout.branches[0]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
    ex.fanout.branches[0].policy=TM_FO_ORDERED_LOSSLESS;
    ex.fanout.branches[0].queue_depth=1u;
    bindings[0].node_id="source";
    bindings[0].source_next=tm_sgh264_next_cpu_i420;
    bindings[0].ctx=source;
    bindings[1].node_id="vision";
    bindings[1].visit=m17_visit_processor;
    bindings[1].ctx=&f;
    bindings[2].node_id="preview";
    bindings[2].visit=m17_preview_sink;
    bindings[2].ctx=&f;
    assert(tm_ex_create(program,store,&ex,bindings,3u,&executor)==TM_EX_OK);
    assert(tm_ex_bind_source_metadata(executor,tm_sgh264_source_metadata,source)
           ==TM_EX_OK);
    assert(tm_ex_pump_source(executor)==TM_EX_OK);
    assert(f.processor_visits==1u && f.bridge!=NULL);
    assert(tm_ex_pump_sink(executor,0u)==TM_EX_OK);
    assert(f.sink_visits==1u);
    assert(tm_fs_get_metadata(store,&f.ref,&check)==TM_FS_OK);
    assert(tm_ex_stop(executor)==TM_EX_OK);

    assert(ti_bridge_take(f.bridge,&ticket,&event)==TI_OK);
    assert(event.header.kind==TI_EVENT_OBSERVATION && event.pin_frame==0u);
    assert(ti_bridge_read_payload(f.bridge,ticket,&received,sizeof(received),&read)
           ==TI_OK);
    assert(read==sizeof(received) && received.capture_sequence==1u);
    assert(received.frame.frame_id==f.ref.frame_id);
    assert(ti_bridge_settle(f.bridge,ticket)==TI_OK);

    /* Both exact request rows retain independent FramePins after the media
     * preview sink and Source have stopped. No scheduling on worker thread. */
    assert(ti_bridge_take(f.bridge,&taken_a,&event)==TI_OK);
    assert(event.header.kind==TI_EVENT_ANALYSIS_REQUEST &&
           event.header.event_id==11u && event.pin_frame==1u);
    assert(ti_sg_bridge_visit_pinned(&f.ctx,f.bridge,taken_a,
                                     m17_visit_analysis,&f)==TI_OK);
    assert(ti_bridge_take(f.bridge,&taken_b,&event)==TI_OK);
    assert(event.header.kind==TI_EVENT_ANALYSIS_REQUEST &&
           event.header.event_id==12u && event.pin_frame==1u);
    assert(ti_sg_bridge_visit_pinned(&f.ctx,f.bridge,taken_b,
                                     m17_visit_analysis,&f)==TI_OK);
    assert(f.analysis_visits==2u);
    expected_digest=m17_checksum(f.pixels,sizeof(f.pixels));
    /* A wrong/tampered TAKEN ticket cannot lend pixel memory. */
    {
        ti_bridge_ticket_v1 tampered=taken_a;
        tampered.generation++;
        assert(ti_sg_bridge_begin_async_borrow(&f.ctx,f.bridge,tampered,
                  &async_borrow,&borrowed_view)==TI_ESTALE);
    }

    /* Reuse existing TurboMedia ProviderCompletion + OwnerDispatch, never
     * create a new executor/portal scheduler inside TurboInteract. */
    inbox_cfg.capacity=2u;
    assert(tm_ho_create(&inbox_cfg,&inbox)==TM_HO_OK);
    assert(tm_od_create(inbox,executor,store,&owner_cfg,&media_owner)==TM_OD_OK);
    portal_cfg.capacity=2u;portal_cfg.provider_generation=55u;
    assert(tm_pc_create(&portal_cfg,&portal)==TM_PC_OK);
    entries[0].correlation=11u;entries[0].result_event_id=21u;
    entries[0].request_ticket=taken_a;
    entries[1].correlation=12u;entries[1].result_event_id=22u;
    entries[1].request_ticket=taken_b;
    delivery.source_context=&f.ctx;
    delivery.bridge=f.bridge;
    delivery.plan_generation=3u;delivery.provider_generation=55u;
    delivery.result_schema_id=67u;
    delivery.entries=entries;delivery.entry_count=2u;
    assert(tm_od_bind_completion(media_owner,portal,ti_sg_bridge_emit_terminal,
                                 &delivery)==TM_OD_OK);

    /* Actual callback thread completes the originally admitted Portal
     * ticket. Only the Media Owner may deliver terminal values to Bridge. */
    assert(tm_pc_begin(portal,11u,&provider_a)==TM_PC_OK);
    assert(ti_sg_bridge_begin_async_borrow(&f.ctx,f.bridge,taken_a,
                   &async_borrow,&borrowed_view)==TI_OK);
    assert(borrowed_view.size_bytes==TI_M17_I420);
    assert(ti_sg_bridge_begin_async_borrow(&f.ctx,f.bridge,taken_a,
                   &async_borrow,&borrowed_view)==TI_EBUSY);
    m17_complete_from_real_worker(portal,provider_a,0,
                                  &borrowed_view,expected_digest);
    /* Worker JOIN must precede owner release; its callback tail is quiesced. */
    assert(ti_sg_bridge_end_async_borrow(&f.ctx,&async_borrow,
                                         &borrowed_view)==TI_OK);
    assert(borrowed_view.data==NULL && borrowed_view.size_bytes==0u);
    assert(ti_sg_bridge_end_async_borrow(&f.ctx,&async_borrow,
                                         &borrowed_view)==TI_ESTALE);
    assert(tm_od_step_completion(media_owner,&terminal)==TM_OD_OK);
    assert(terminal.correlation==11u &&
           terminal.outcome==TM_PC_COMPLETED);
    outcome=m17_take_terminal(&f.ctx,f.bridge,21u,TM_PC_COMPLETED);
    assert(outcome.provider_status==0 && entries[0].attempted==1u);

    /* Cancellation wins before callback completion: terminal is CANCELLED,
     * never silently upgraded to model/Agent success. */
    assert(tm_pc_begin(portal,12u,&provider_b)==TM_PC_OK);
    assert(ti_sg_bridge_begin_async_borrow(&f.ctx,f.bridge,taken_b,
                   &async_borrow,&borrowed_view)==TI_OK);
    assert(tm_pc_request_cancel(portal,provider_b)==TM_PC_OK);
    /* Cancellation never revokes in-use borrowed pixels before worker JOIN.
     * A real worker reads them, but Portal terminal remains CANCELLED. */
    m17_complete_from_real_worker(portal,provider_b,99,
                                  &borrowed_view,expected_digest);
    assert(ti_sg_bridge_end_async_borrow(&f.ctx,&async_borrow,
                                         &borrowed_view)==TI_OK);
    terminal=(tm_pc_result_v1)TM_PC_RESULT_V1_INIT;
    assert(tm_od_step_completion(media_owner,&terminal)==TM_OD_OK);
    assert(terminal.correlation==12u &&
           terminal.outcome==TM_PC_CANCELLED &&
           terminal.completion_observed==1u);
    outcome=m17_take_terminal(&f.ctx,f.bridge,22u,TM_PC_CANCELLED);
    assert(outcome.provider_status==99 && entries[1].attempted==1u);

    /* A fresh Portal ticket with an OLD correlation MUST NOT emit another
     * fact for the same pinned AnalysisRequest. The existing Owner retains
     * the failed terminal as TAKEN until explicit reconciliation. */
    assert(tm_pc_begin(portal,11u,&duplicate)==TM_PC_OK);
    m17_complete_from_real_worker(portal,duplicate,0,NULL,0u);
    terminal=(tm_pc_result_v1)TM_PC_RESULT_V1_INIT;
    assert(tm_od_step_completion(media_owner,&terminal)==TM_OD_EDELIVERY);
    assert(tm_od_pending_completion(media_owner,&terminal)==TM_OD_OK &&
           terminal.correlation==11u);
    assert(tm_od_step_completion(media_owner,&terminal)==TM_OD_EBUSY);
    assert(tm_od_settle_pending_completion(media_owner)==TM_OD_OK);
    assert(tm_od_pending_completion(media_owner,&terminal)==TM_OD_EMPTY);

    assert(tm_pc_seal(portal)==TM_PC_OK);
    assert(tm_ho_seal(inbox)==TM_HO_OK);
    assert(tm_od_destroy(media_owner)==TM_OD_OK);
    assert(tm_pc_destroy(portal)==TM_PC_OK);
    assert(tm_ho_destroy(inbox)==TM_HO_OK);
    assert(tm_ex_destroy(executor)==TM_EX_OK);

    /* Terminal values never mint new Pins. The original two TAKEN analysis
     * tickets remain independently settleable and retain the retired slot. */
    assert(ti_bridge_settle(f.bridge,taken_a)==TI_OK);
    assert(tm_fs_get_metadata(store,&f.ref,&check)==TM_FS_OK);
    assert(ti_bridge_settle(f.bridge,taken_b)==TI_OK);
    assert(ti_bridge_settle(f.bridge,taken_b)==TI_ESTALE);
    assert(tm_fs_get_metadata(store,&f.ref,&check)==TM_FS_ESTALE);
    assert(ti_bridge_seal(f.bridge)==TI_OK);
    assert(ti_bridge_destroy(f.bridge)==TI_OK);
    assert(tm_fs_seal(store)==TM_FS_OK);
    assert(tm_fs_destroy(store)==TM_FS_OK);
    tm_sg_program_destroy(program);
    assert(tm_sgh264_destroy(source)==TM_SGH264_OK);
    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
    free(raw);free(encoded);
    puts("M17c→M17d real H264 Codec→Executor→FrameStore→Observation: PASS");
}
