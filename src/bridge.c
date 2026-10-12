#include "turbo_interact/bridge.h"
#include <stdlib.h>
#include <string.h>

#define TI_BRIDGE_MAX_CAPACITY 4096u

typedef enum {
    TI_ROW_FREE = 0,
    TI_ROW_PREPARED,
    TI_ROW_READY,
    TI_ROW_TAKEN
} ti_row_state;

typedef struct {
    ti_row_state state;
    uint32_t generation;
    uint64_t order;
    uint64_t pin;
    uint8_t *payload;
    size_t payload_size;
    ti_bridge_event_v1 event;
} ti_bridge_row;

struct ti_bridge {
    ti_bridge_config_v1 config;
    ti_bridge_row *rows;
    uint64_t next_order;
    size_t payload_inflight_bytes;
    size_t max_message_bytes;
    size_t max_inflight_bytes;
    int sealed;
};

static ti_status validate_event(const ti_bridge_event_v1 *event) {
    ti_status rc;
    if (!event) return TI_EINVAL;
    if (event->version != TI_ABI_VERSION) return TI_EVERSION;
    if (event->size != sizeof(*event) || event->has_frame > 1u ||
        event->pin_frame > 1u || (event->pin_frame && !event->has_frame)) return TI_EINVAL;
    rc = ti_event_header_validate(&event->header);
    if (rc != TI_OK) return rc;
    if (event->has_frame) {
        rc = ti_frame_ref_validate(&event->frame);
        if (rc != TI_OK) return rc;
        if (event->header.session_generation != event->frame.session_generation)
            return TI_ESTALE;
        if (event->header.kind != TI_EVENT_OBSERVATION &&
            event->header.kind != TI_EVENT_ANALYSIS_REQUEST) return TI_EINVAL;
    }
    return TI_OK;
}

static ti_bridge_row *resolve(ti_bridge *bridge, ti_bridge_ticket_v1 ticket) {
    ti_bridge_row *row;
    if (!bridge || ticket.incarnation != bridge->config.incarnation ||
        ticket.slot >= bridge->config.capacity || !ticket.generation) return NULL;
    row = &bridge->rows[ticket.slot];
    return row->generation == ticket.generation && row->state != TI_ROW_FREE ? row : NULL;
}

static ti_status retire(ti_bridge *bridge, ti_bridge_row *row) {
    if (row->pin) {
        ti_status rc = bridge->config.pin_release(bridge->config.pin_ctx, row->pin);
        if (rc != TI_OK) return rc; /* Failed release retains obligation for retry. */
    }
    row->pin = 0u;
    /* Failed pin release retains both the row and owned payload. */
    free(row->payload);
    bridge->payload_inflight_bytes -= row->payload_size;
    row->payload = NULL;
    row->payload_size = 0u;
    row->order = 0u;
    memset(&row->event, 0, sizeof(row->event));
    row->state = TI_ROW_FREE;
    return TI_OK;
}

static ti_status bridge_create_impl(const ti_bridge_config_v1 *config,
                                     const ti_bridge_payload_limits_v1 *limits, ti_bridge **out) {
    ti_bridge *bridge;
    if (!out || *out != NULL || !config) return TI_EINVAL;
    if (config->version != TI_ABI_VERSION) return TI_EVERSION;
    if (config->size != sizeof(*config) || config->reserved || !config->incarnation ||
        !config->capacity || config->capacity > TI_BRIDGE_MAX_CAPACITY ||
        (!!config->pin_reserve != !!config->pin_release)) return TI_EINVAL;
    if (limits) {
        if (limits->version != TI_ABI_VERSION) return TI_EVERSION;
        if (limits->size != sizeof(*limits) || !limits->max_message_bytes ||
            limits->max_message_bytes > TI_MAX_EVENT_PAYLOAD_BYTES ||
            limits->max_inflight_bytes < limits->max_message_bytes ||
            limits->max_inflight_bytes > TI_MAX_EVENT_PAYLOAD_BYTES * TI_BRIDGE_MAX_CAPACITY)
            return TI_EINVAL;
    }
    bridge = (ti_bridge *)calloc(1u, sizeof(*bridge));
    if (!bridge) return TI_ENOMEM;
    bridge->rows = (ti_bridge_row *)calloc(config->capacity, sizeof(*bridge->rows));
    if (!bridge->rows) { free(bridge); return TI_ENOMEM; }
    bridge->config = *config;
    if (limits) {
        bridge->max_message_bytes = limits->max_message_bytes;
        bridge->max_inflight_bytes = limits->max_inflight_bytes;
    }
    bridge->next_order = 1u;
    *out = bridge;
    return TI_OK;
}

ti_status ti_bridge_create(const ti_bridge_config_v1 *config, ti_bridge **out) {
    return bridge_create_impl(config, NULL, out);
}

ti_status ti_bridge_create_with_payload_limits(const ti_bridge_config_v1 *config,
                                                const ti_bridge_payload_limits_v1 *limits,
                                                ti_bridge **out) {
    if (!limits) return TI_EINVAL;
    return bridge_create_impl(config, limits, out);
}

static ti_status bridge_prepare_impl(ti_bridge *bridge, const ti_bridge_event_v1 *event,
                                     const void *payload, size_t payload_size,
                                     ti_bridge_ticket_v1 *out) {
    uint32_t i;
    ti_bridge_row *row = NULL;
    uint64_t pin = 0u;
    uint8_t *owned = NULL;
    ti_status rc;
    if (!bridge || !out) return TI_EINVAL;
    memset(out, 0, sizeof(*out));
    rc = validate_event(event);
    if (rc != TI_OK) return rc;
    if ((size_t)event->header.payload_bytes != payload_size ||
        (!payload && payload_size) || (payload && !payload_size)) return TI_EINVAL;
    if (payload_size && !bridge->max_message_bytes) return TI_EUNSUPPORTED;
    if (bridge->sealed) return TI_EBUSY;
    if (payload_size > bridge->max_message_bytes ||
        payload_size > bridge->max_inflight_bytes - bridge->payload_inflight_bytes)
        return TI_EFULL;
    for (i = 0; i < bridge->config.capacity; i++) {
        if (bridge->rows[i].state == TI_ROW_FREE &&
            bridge->rows[i].generation != UINT32_MAX) {
            row = &bridge->rows[i];
            break;
        }
    }
    if (!row) return TI_EFULL;
    if (payload_size) {
        owned = (uint8_t *)malloc(payload_size);
        if (!owned) return TI_ENOMEM;
        memcpy(owned, payload, payload_size);
    }
    if (event->pin_frame) {
        if (!bridge->config.pin_reserve) { free(owned); return TI_EUNSUPPORTED; }
        rc = bridge->config.pin_reserve(bridge->config.pin_ctx, &event->frame, &pin);
        if (rc != TI_OK) { free(owned); return rc; }
        if (!pin) { free(owned); return TI_ECONFLICT; }
    }
    row->generation++;
    row->event = *event;
    row->pin = pin;
    row->payload = owned;
    row->payload_size = payload_size;
    bridge->payload_inflight_bytes += payload_size;
    row->state = TI_ROW_PREPARED;
    out->incarnation = bridge->config.incarnation;
    out->slot = i;
    out->generation = row->generation;
    return TI_OK;
}

ti_status ti_bridge_prepare(ti_bridge *bridge, const ti_bridge_event_v1 *event,
                             ti_bridge_ticket_v1 *out) {
    if (event && event->header.payload_bytes) return TI_EUNSUPPORTED;
    return bridge_prepare_impl(bridge, event, NULL, 0u, out);
}

ti_status ti_bridge_prepare_owned(ti_bridge *bridge, const ti_bridge_event_v1 *event,
                                   const void *payload, size_t payload_size,
                                   ti_bridge_ticket_v1 *out) {
    return bridge_prepare_impl(bridge, event, payload, payload_size, out);
}

ti_status ti_bridge_commit(ti_bridge *bridge, ti_bridge_ticket_v1 ticket) {
    ti_bridge_row *row = resolve(bridge, ticket);
    if (!row) return TI_ESTALE;
    if (row->state != TI_ROW_PREPARED) return TI_ECONFLICT;
    if (bridge->sealed) return TI_EBUSY;
    if (bridge->next_order == UINT64_MAX) return TI_ERANGE;
    row->order = bridge->next_order++;
    row->state = TI_ROW_READY;
    return TI_OK;
}

ti_status ti_bridge_discard(ti_bridge *bridge, ti_bridge_ticket_v1 ticket) {
    ti_bridge_row *row = resolve(bridge, ticket);
    if (!row) return TI_ESTALE;
    if (row->state != TI_ROW_PREPARED && row->state != TI_ROW_READY) return TI_EBUSY;
    return retire(bridge, row);
}

ti_status ti_bridge_take(ti_bridge *bridge, ti_bridge_ticket_v1 *out_ticket,
                          ti_bridge_event_v1 *out_event) {
    uint32_t i, found = 0u;
    uint64_t order = UINT64_MAX;
    if (!bridge || !out_ticket || !out_event) return TI_EINVAL;
    if (out_event->version != TI_ABI_VERSION) return TI_EVERSION;
    if (out_event->size != sizeof(*out_event)) return TI_EINVAL;
    memset(out_ticket, 0, sizeof(*out_ticket));
    for (i = 0u; i < bridge->config.capacity; i++) {
        const ti_bridge_row *row = &bridge->rows[i];
        if (row->state == TI_ROW_READY && row->order < order) {
            found = i; order = row->order;
        }
    }
    if (order == UINT64_MAX) return TI_EBUSY;
    bridge->rows[found].state = TI_ROW_TAKEN;
    *out_event = bridge->rows[found].event;
    out_ticket->incarnation = bridge->config.incarnation;
    out_ticket->slot = found;
    out_ticket->generation = bridge->rows[found].generation;
    return TI_OK;
}

ti_status ti_bridge_read_payload(ti_bridge *bridge, ti_bridge_ticket_v1 ticket,
                                  void *destination, size_t capacity, size_t *out_size) {
    ti_bridge_row *row;
    if (!bridge || !out_size || (!destination && capacity)) return TI_EINVAL;
    *out_size = 0u;
    row = resolve(bridge, ticket);
    if (!row) return TI_ESTALE;
    if (row->state != TI_ROW_TAKEN) return TI_ECONFLICT;
    *out_size = row->payload_size;
    if (capacity < row->payload_size) return TI_ERANGE;
    if (row->payload_size && !destination) return TI_EINVAL;
    if (row->payload_size) memcpy(destination, row->payload, row->payload_size);
    return TI_OK;
}

ti_status ti_bridge_borrow_pin_token(ti_bridge *bridge, ti_bridge_ticket_v1 ticket,
                                     uint64_t *out_token) {
    ti_bridge_row *row;
    if (!bridge || !out_token) return TI_EINVAL;
    *out_token = 0u;
    row = resolve(bridge, ticket);
    if (!row) return TI_ESTALE;
    if (row->state != TI_ROW_TAKEN) return TI_ECONFLICT;
    if (!row->pin) return TI_EUNSUPPORTED;
    *out_token = row->pin;
    return TI_OK;
}

ti_status ti_bridge_settle(ti_bridge *bridge, ti_bridge_ticket_v1 ticket) {
    ti_bridge_row *row = resolve(bridge, ticket);
    if (!row) return TI_ESTALE;
    if (row->state != TI_ROW_TAKEN) return TI_ECONFLICT;
    return retire(bridge, row);
}

ti_status ti_bridge_seal(ti_bridge *bridge) {
    if (!bridge) return TI_EINVAL;
    bridge->sealed = 1;
    return TI_OK;
}

ti_status ti_bridge_snapshot(const ti_bridge *bridge, ti_bridge_snapshot_v1 *out) {
    uint32_t i;
    if (!bridge || !out) return TI_EINVAL;
    if (out->version != TI_ABI_VERSION) return TI_EVERSION;
    if (out->size != sizeof(*out)) return TI_EINVAL;
    out->reserved = out->ready = out->inflight = 0u;
    for (i = 0u; i < bridge->config.capacity; i++) {
        switch (bridge->rows[i].state) {
            case TI_ROW_PREPARED: out->reserved++; break;
            case TI_ROW_READY: out->ready++; break;
            case TI_ROW_TAKEN: out->inflight++; break;
            default: break;
        }
    }
    out->sealed = bridge->sealed ? 1u : 0u;
    out->drained = out->sealed && !out->reserved && !out->ready && !out->inflight;
    return TI_OK;
}

ti_status ti_bridge_destroy(ti_bridge *bridge) {
    ti_bridge_snapshot_v1 snap = TI_BRIDGE_SNAPSHOT_V1_INIT;
    if (!bridge) return TI_EINVAL;
    if (ti_bridge_snapshot(bridge, &snap) != TI_OK || !snap.drained) return TI_EBUSY;
    free(bridge->rows);
    free(bridge);
    return TI_OK;
}
