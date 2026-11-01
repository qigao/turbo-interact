#ifndef TURBO_INTERACT_STREAMGRAPH_BRIDGE_H
#define TURBO_INTERACT_STREAMGRAPH_BRIDGE_H

/* Optional installed TurboMedia::StreamGraph product integration.
 * All three owners (FrameStore, Bridge, callback context) are borrowed and
 * SERIALIZED ON ONE Media Owner lane. There is no second scheduler, registry,
 * decoder, cross-process FrameRef or automatic retry.
 */
#include "turbo_interact/bridge.h"
#include <turbo_frame_store.h>
#include <turbo_streamgraph_owner_dispatch.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ti_sg_bridge_context_v1 {
    uint32_t size, version;
    tm_fs_store *store;  /* borrowed; remains live through Bridge destroy */
    uint64_t store_id, store_generation;
    uint64_t source_session_generation, display_generation;
    uint64_t device_session_id, device_session_generation;
    uint32_t display_id, reserved;
    uint64_t geometry_generation;
    uint64_t capture_session_hi, capture_session_lo, stream_generation;
} ti_sg_bridge_context_v1;
#define TI_SG_BRIDGE_CONTEXT_V1_INIT {sizeof(ti_sg_bridge_context_v1), TI_ABI_VERSION, NULL, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}

/* Strictly binds existing ti_bridge_config_v1 reserve/release callbacks
 * to the ACTUAL TurboMedia FrameStore. Never overrides caller's owner quota,
 * slot count or bridge incarnation. Context MUST NOT change while Bridge
 * has prepared/ready/taken requests; it must outlive their settlement.
 */
TI_API ti_status ti_sg_bridge_configure(ti_sg_bridge_context_v1 *context,
                                        ti_bridge_config_v1 *bridge_config);

/* Convert upstream frame identity to TurboInteract's value-only FrameRef.
 * Store, session and capture lineage are separately checked at admission. */
TI_API ti_status ti_sg_bridge_frame_ref(const tm_fs_frame_ref_v1 *upstream,
                                        ti_frame_ref_v1 *out);

/* Queue an immutable typed Observation (no pin). It is weak and may become
 * stale before a later AnalysisRequest. This does NOT prove model accuracy,
 * compositor causality or input-task success. Caller commits/discards ticket
 * through the canonical ti_bridge API, never an alternate mailbox.
 * Payload is an in-process copy, NOT a wire serialization ABI. */
TI_API ti_status ti_sg_bridge_prepare_observation(
    ti_sg_bridge_context_v1 *context, ti_bridge *bridge,
    const tm_fs_frame_ref_v1 *frame, const ti_observation_v1 *observation,
    const ti_event_header_v1 *header, ti_bridge_ticket_v1 *out_ticket);

/* Reserve one independent FrameStore Pin per asynchronously retaining
 * AnalysisRequest, BEFORE committing an owned bounded request payload.
 * The Bridge owns the Pin until discard/settle; duplicates of the returned
 * numeric ticket do NOT clone its credit. Caller commits explicitly. */
TI_API ti_status ti_sg_bridge_prepare_analysis(
    ti_sg_bridge_context_v1 *context, ti_bridge *bridge,
    const tm_fs_frame_ref_v1 *frame, const ti_event_header_v1 *header,
    const void *payload, size_t payload_bytes,
    ti_bridge_ticket_v1 *out_ticket);

typedef ti_status (*ti_sg_bridge_frame_visit_fn)(
    void *user, const ti_bridge_event_v1 *actual_taken_event,
    const tm_fs_frame_metadata_v1 *provenance,
    const tm_fs_cpu_view_v1 *borrowed_pixels);

/* On the owning lane, take the Bridge's ACTUAL stored event and Pin token,
 * acquire a short-lived FrameLease, check the entire session/geometry/capture
 * incarnation, invoke callback and always release Lease before returning.
 * Does NOT settle or clone the Bridge Pin: caller explicitly settles the
 * original TAKEN ticket after any external delivery/reconciliation.
 * Callback may not retain the pixel pointer or recurse into Media Owner.
 */
TI_API ti_status ti_sg_bridge_visit_pinned(
    ti_sg_bridge_context_v1 *context, ti_bridge *bridge,
    ti_bridge_ticket_v1 taken_ticket,
    ti_sg_bridge_frame_visit_fn callback, void *user);


/* M17d asynchronous PROVIDER STATUS → typed Bridge terminal contract.
 * This is not a model inference result or a new executor. A Host-owned
 * Provider callback thread calls tm_pc_complete(); the existing Media Owner
 * alone invokes tm_od_step_completion(), which calls this callback.
 *
 * Only the actual TAKEN, Pin-retaining AnalysisRequest with exact
 * correlation/provider/frame lineage may authorize a terminal event.
 * The result is a bounded owned VALUE payload; FrameRef stays weak and
 * an ActionIntent is never created or executed here.
 */
#define TI_SG_TERMINAL_MAX_ENTRIES 32u
typedef struct ti_sg_analysis_terminal_v1 {
    uint32_t size, version;
    uint32_t outcome, completion_observed; /* original TM_PC_* semantics */
    int32_t provider_status;
    uint32_t reserved;
    uint64_t correlation, provider_generation, source_packet_sequence;
    ti_frame_ref_v1 frame; /* weak, not pixels or an extra Pin */
    uint64_t device_session_id, device_session_generation;
    uint32_t display_id, reserved2;
    uint64_t geometry_generation, capture_session_hi, capture_session_lo;
    uint64_t stream_generation, pts, dts;
    uint32_t timebase_num, timebase_den;
} ti_sg_analysis_terminal_v1;

typedef struct ti_sg_terminal_entry_v1 {
    uint32_t size, version;
    uint64_t correlation; /* MUST equal the existing AnalysisRequest event_id */
    uint64_t result_event_id; /* unique nonzero downstream event ID */
    ti_bridge_ticket_v1 request_ticket; /* still TAKEN, owns the FramePin */
    uint32_t attempted; /* owner-local one-shot terminal delivery gate */
    uint32_t reserved;
} ti_sg_terminal_entry_v1;
#define TI_SG_TERMINAL_ENTRY_V1_INIT {sizeof(ti_sg_terminal_entry_v1),TI_ABI_VERSION,0u,0u,{0},0u,0u}

typedef struct ti_sg_terminal_delivery_v1 {
    uint32_t size, version;
    ti_sg_bridge_context_v1 *source_context; /* borrowed, stable until join */
    ti_bridge *bridge; /* borrowed: must outlive all portal terminal callbacks */
    uint64_t plan_generation, provider_generation;
    uint32_t result_schema_id, entry_count;
    ti_sg_terminal_entry_v1 *entries; /* caller-owned, bounded, stable */
    uint32_t reserved;
} ti_sg_terminal_delivery_v1;
#define TI_SG_TERMINAL_DELIVERY_V1_INIT {sizeof(ti_sg_terminal_delivery_v1),TI_ABI_VERSION,NULL,NULL,0u,0u,0u,0u,NULL,0u}

/* Compatible with tm_od_terminal_emit_fn. Return 0 ONLY after Bridge commit
 * accepted the immutable terminal. On capacity/authority/lineage rejection
 * return nonzero: tm_od_step_completion leaves its original Provider ticket
 * TAKEN/pending; no replay/reemit/retry. Host must reconcile and explicitly
 * call tm_od_settle_pending_completion() before teardown.
 * The matching pinned AnalysisRequest remains owned until its own settle.
 * Every entry is single-use: attempted is set ON THE OWNER lane for the
 * first matching Provider terminal, even when Bridge delivery is rejected.
 * A repeated correlation/new Provider ticket cannot re-emit an old result.
 * Entries and all borrowed owners must outlive Provider callback JOIN,
 * pending terminal reconciliation and Bridge request settlement.
 */
TI_API int ti_sg_bridge_emit_terminal(void *delivery_context,
                                      const tm_pc_result_v1 *terminal);

#ifdef __cplusplus
}
#endif
#endif
