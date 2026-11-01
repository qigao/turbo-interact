/*
 * Cross-project product C11 test IN THE EXISTING ti_bridge_test executable.
 * Actual TurboMedia H264/OpenH264 Source -> canonical StreamGraph Executor
 * -> genuine CPU FrameStore -> TurboInteract typed Observation/GraphBridge.
 * Codec/StreamGraph source is the exact upstream pinned checkout, not copied
 * into TurboInteract as a production implementation. No device or sockets.
 */
#include "turbo_interact/streamgraph_bridge.h"
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
    limits.max_message_bytes=sizeof(obs);
    limits.max_inflight_bytes=2u*sizeof(obs);
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
    assert(tm_ex_destroy(executor)==TM_EX_OK);

    assert(ti_bridge_take(f.bridge,&ticket,&event)==TI_OK);
    assert(event.header.kind==TI_EVENT_OBSERVATION && event.pin_frame==0u);
    assert(ti_bridge_read_payload(f.bridge,ticket,&received,sizeof(received),&read)
           ==TI_OK);
    assert(read==sizeof(received) && received.capture_sequence==1u);
    assert(received.frame.frame_id==f.ref.frame_id);
    assert(ti_bridge_settle(f.bridge,ticket)==TI_OK);
    assert(ti_bridge_take(f.bridge,&ticket,&event)==TI_OK);
    assert(ti_sg_bridge_visit_pinned(&f.ctx,f.bridge,ticket,
                                     m17_visit_analysis,&f)==TI_OK);
    assert(ti_bridge_settle(f.bridge,ticket)==TI_OK);
    assert(tm_fs_get_metadata(store,&f.ref,&check)==TM_FS_OK);
    assert(ti_bridge_take(f.bridge,&ticket,&event)==TI_OK);
    assert(ti_sg_bridge_visit_pinned(&f.ctx,f.bridge,ticket,
                                     m17_visit_analysis,&f)==TI_OK);
    assert(ti_bridge_settle(f.bridge,ticket)==TI_OK);
    assert(f.analysis_visits==2u);
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
