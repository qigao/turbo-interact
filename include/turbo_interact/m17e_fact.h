#ifndef TURBO_INTERACT_M17E_FACT_H
#define TURBO_INTERACT_M17E_FACT_H

/* M17e: pointer-free, versioned, canonical terminal-STATUS fact.
 * A Provider's COMPLETED state is NOT model recognition, SCXML transition,
 * Android gesture, or confirmation of an Agent objective.
 *
 * Process-local FrameRefs/FramePins/leases, host struct padding and raw pixel
 * pointers NEVER cross the Flow/SCXML or network boundary. The 160-byte wire
 * representation is fieldwise big-endian. Transport authentication and
 * trusted Flow/SCXML SDK binding are independent required gates.
 */
#include "turbo_interact/streamgraph_bridge.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TI_M17E_FACT_ABI_V1 1u
#define TI_M17E_TERMINAL_WIRE_BYTES 160u
#define TI_M17E_TERMINAL_WIRE_KIND 1u

typedef struct ti_m17e_terminal_fact_v1 {
    uint32_t size, version, outcome, completion_observed;
    int32_t provider_status;
    uint32_t schema_id;
    uint64_t event_id, correlation, plan_generation;
    uint64_t source_session_generation;
    uint64_t device_session_id, device_session_generation;
    uint32_t display_id, reserved;
    uint64_t geometry_generation, capture_session_hi, capture_session_lo;
    uint64_t stream_generation, packet_sequence, provider_generation;
    uint64_t pts, dts;
    uint32_t timebase_num, timebase_den;
} ti_m17e_terminal_fact_v1;
#define TI_M17E_TERMINAL_FACT_V1_INIT {sizeof(ti_m17e_terminal_fact_v1),TI_M17E_FACT_ABI_V1,0u,0u,0,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u}

TI_API ti_status ti_m17e_terminal_fact_validate(
    const ti_m17e_terminal_fact_v1 *fact);

/* Inspect the ACTUAL TAKEN Bridge result, never a supplied or guessed event.
 * This may succeed after FrameStore retirement, because a status fact is a
 * permanently owned value while FrameRef was only a weak pixel identity.
 * No Bridge settlement, Pin acquisition, Provider effect or retry occurs.
 * Expected source_context is the trusted stable Host session incarnation. */
TI_API ti_status ti_m17e_terminal_fact_from_taken(
    ti_sg_bridge_context_v1 *source_context,
    ti_bridge *bridge, ti_bridge_ticket_v1 taken_result_ticket,
    uint64_t expected_plan_generation,
    uint64_t expected_provider_generation,
    uint32_t expected_schema_id,
    ti_m17e_terminal_fact_v1 *out_fact);

/* Exact 160-byte transport-neutral encoding, NOT a raw native struct,
 * and never evidence of an authenticated sender. No extra trailing bytes. */
TI_API ti_status ti_m17e_terminal_fact_encode(
    const ti_m17e_terminal_fact_v1 *fact,
    void *destination, size_t capacity, size_t *out_bytes);
TI_API ti_status ti_m17e_terminal_fact_decode(
    const void *source, size_t source_bytes,
    ti_m17e_terminal_fact_v1 *out_fact);

#ifdef __cplusplus
}
#endif
#endif
