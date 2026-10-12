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
    ti_bridge_event_v1 event;
} ti_bridge_row;

struct ti_bridge {
    ti_bridge_config_v1 config;
    ti_bridge_row *rows;
    uint64_t next_order;
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
    row->order = 0u;
    memset(&row->event, 0, sizeof(row->event));
    row->state = TI_ROW_FREE;
    return TI_OK;
}

ti_status ti_bridge_create(const ti_bridge_config_v1 *config, ti_bridge **out) {
    ti_bridge *bridge;
    if (!out || *out != NULL || !config) return TI_EINVAL;
    if (config->version != TI_ABI_VERSION) return TI_EVERSION;
    if (config->size != sizeof(*config) || config->reserved || !config->incarnation ||
        !config->capacity || config->capacity > TI_BRIDGE_MAX_CAPACITY ||
        (!!config->pin_reserve != !!config->pin_release)) return TI_EINVAL;
    bridge = (ti_bridge *)calloc(1u, sizeof(*bridge));
    if (!bridge) return TI_ENOMEM;
    bridge->rows = (ti_bridge_row *)calloc(config->capacity, sizeof(*bridge->rows));
    if (!bridge->rows) { free(bridge); return TI_ENOMEM; }
    bridge->config = *config;
    bridge->next_order = 1u;
    *out = bridge;
    return TI_OK;
}

ti_status ti_bridge_prepare(ti_bridge *bridge, const ti_bridge_event_v1 *event,
                             ti_bridge_ticket_v1 *out) {
    uint32_t i;
    ti_bridge_row *row = NULL;
    uint64_t pin = 0u;
    ti_status rc;
    if (!bridge || !out) return TI_EINVAL;
    memset(out, 0, sizeof(*out));
    rc = validate_event(event);
    if (rc != TI_OK) return rc;
    if (bridge->sealed) return TI_EBUSY;
    for (i = 0u; i < bridge->config.capacity; i++) {
        if (bridge->rows[i].state == TI_ROW_FREE &&
            bridge->rows[i].generation != UINT32_MAX) {
            row = &bridge->rows[i];
            break;
        }
    }
    if (!row) return TI_EFULL;
    if (event->pin_frame) {
        if (!bridge->config.pin_reserve) return TI_EUNSUPPORTED;
        rc = bridge->config.pin_reserve(bridge->config.pin_ctx, &event->frame, &pin);
        if (rc != TI_OK) return rc;
        if (!pin) return TI_ECONFLICT;
    }
    row->generation++;
    row->event = *event;
    row->pin = pin;
    row->state = TI_ROW_PREPARED;
    out->incarnation = bridge->config.incarnation;
    out->slot = i;
    out->generation = row->generation;
    return TI_OK;
}

ti_status ti_bridge_commit(ti_bridge *bridge, ti_bridge_ticket_v1 ticket) {
    ti_bridge_row *row = resolve(bridge, ticket);
    if (!row) return TI_ESTALE;
    if (row->state != TI_ROW_PREPARED) return TI_ECONFLICT;
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
