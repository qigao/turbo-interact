#ifndef TURBO_INTERACT_BRIDGE_H
#define TURBO_INTERACT_BRIDGE_H
#include "turbo_interact/domain.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One owner-lane bridge: caller serializes all API and provider callbacks.
 * A ticket is a value identity, not duplicated ownership when copied. */
typedef struct ti_bridge ti_bridge;

typedef struct ti_bridge_ticket_v1 {
    uint64_t incarnation;
    uint32_t slot;
    uint32_t generation;
} ti_bridge_ticket_v1;

typedef ti_status (*ti_frame_pin_reserve_fn)(void *ctx, const ti_frame_ref_v1 *frame,
                                             uint64_t *out_pin);
typedef ti_status (*ti_frame_pin_release_fn)(void *ctx, uint64_t pin);

typedef struct ti_bridge_config_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t capacity;
    uint32_t reserved;
    uint64_t incarnation;
    ti_frame_pin_reserve_fn pin_reserve;
    ti_frame_pin_release_fn pin_release;
    void *pin_ctx;
} ti_bridge_config_v1;
#define TI_BRIDGE_CONFIG_V1_INIT {sizeof(ti_bridge_config_v1), TI_ABI_VERSION, 0u, 0u, 0u, NULL, NULL, NULL}

/* Optional owned event payloads. Only one serialized owner lane is supported. */
typedef struct ti_bridge_payload_limits_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t max_message_bytes;
    uint32_t max_inflight_bytes;
} ti_bridge_payload_limits_v1;
#define TI_BRIDGE_PAYLOAD_LIMITS_V1_INIT {sizeof(ti_bridge_payload_limits_v1), TI_ABI_VERSION, 0u, 0u}

typedef struct ti_bridge_event_v1 {
    uint32_t size;
    uint32_t version;
    ti_event_header_v1 header;
    ti_frame_ref_v1 frame;
    uint32_t has_frame;
    uint32_t pin_frame;
} ti_bridge_event_v1;
#define TI_BRIDGE_EVENT_V1_INIT {sizeof(ti_bridge_event_v1), TI_ABI_VERSION, TI_EVENT_HEADER_V1_INIT, TI_FRAME_REF_V1_INIT, 0u, 0u}

typedef struct ti_bridge_snapshot_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t reserved;
    uint32_t ready;
    uint32_t inflight;
    uint32_t sealed;
    uint32_t drained;
} ti_bridge_snapshot_v1;
#define TI_BRIDGE_SNAPSHOT_V1_INIT {sizeof(ti_bridge_snapshot_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, 0u}

TI_API ti_status ti_bridge_create(const ti_bridge_config_v1 *config, ti_bridge **out);
/* Opt-in: the bridge owns bounded, deep-copied serialized bytes. */
TI_API ti_status ti_bridge_create_with_payload_limits(
    const ti_bridge_config_v1 *config, const ti_bridge_payload_limits_v1 *limits,
    ti_bridge **out);
TI_API ti_status ti_bridge_prepare(ti_bridge *bridge, const ti_bridge_event_v1 *event,
                                   ti_bridge_ticket_v1 *out);
/* Copies borrowed input before visible commit, quota held until successful
 * discard/settle. payload_size must equal event.header.payload_bytes. */
TI_API ti_status ti_bridge_prepare_owned(ti_bridge *bridge, const ti_bridge_event_v1 *event,
                                         const void *payload, size_t payload_size,
                                         ti_bridge_ticket_v1 *out);
TI_API ti_status ti_bridge_commit(ti_bridge *bridge, ti_bridge_ticket_v1 ticket);
TI_API ti_status ti_bridge_discard(ti_bridge *bridge, ti_bridge_ticket_v1 ticket);
TI_API ti_status ti_bridge_take(ti_bridge *bridge, ti_bridge_ticket_v1 *out_ticket,
                                ti_bridge_event_v1 *out_event);
/* Borrow a FrameStore lookup token after take. NOT an extra pin credit;
 * it expires at settlement and cannot be retained as ownership. */
/* Copies data from a TAKEN row; never returns a borrowed pointer. For
 * a size probe pass NULL/0, which returns TI_ERANGE if payload is nonempty. */
TI_API ti_status ti_bridge_read_payload(ti_bridge *bridge, ti_bridge_ticket_v1 ticket,
                                       void *destination, size_t capacity, size_t *out_size);
TI_API ti_status ti_bridge_borrow_pin_token(ti_bridge *bridge, ti_bridge_ticket_v1 ticket,
                                            uint64_t *out_token);
TI_API ti_status ti_bridge_settle(ti_bridge *bridge, ti_bridge_ticket_v1 ticket);
TI_API ti_status ti_bridge_seal(ti_bridge *bridge);
TI_API ti_status ti_bridge_snapshot(const ti_bridge *bridge, ti_bridge_snapshot_v1 *out);
/* Only succeeds after sealing and settlement of all outstanding credits. */
TI_API ti_status ti_bridge_destroy(ti_bridge *bridge);

#ifdef __cplusplus
}
#endif
#endif
