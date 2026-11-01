#ifndef TURBO_INTERACT_STREAMGRAPH_BRIDGE_H
#define TURBO_INTERACT_STREAMGRAPH_BRIDGE_H

/* Optional installed TurboMedia::StreamGraph product integration.
 * All three owners (FrameStore, Bridge, callback context) are borrowed and
 * SERIALIZED ON ONE Media Owner lane. There is no second scheduler, registry,
 * decoder, cross-process FrameRef or automatic retry.
 */
#include "turbo_interact/bridge.h"
#include <turbo_frame_store.h>

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

#ifdef __cplusplus
}
#endif
#endif
