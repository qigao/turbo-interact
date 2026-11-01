#include "turbo_interact/domain.h"

static ti_status ti_validate_abi(uint32_t size, uint32_t version, uint32_t expected_size) {
    if (version != TI_ABI_VERSION) return TI_EVERSION;
    return size == expected_size ? TI_OK : TI_EINVAL;
}

ti_status ti_frame_ref_validate(const ti_frame_ref_v1 *ref) {
    ti_status status;
    if (!ref) return TI_EINVAL;
    status = ti_validate_abi(ref->size, ref->version, (uint32_t)sizeof(*ref));
    if (status != TI_OK) return status;
    if (!ref->store_id || !ref->store_generation || !ref->slot_generation ||
        !ref->frame_id || !ref->session_generation || !ref->display_generation) return TI_EINVAL;
    return TI_OK; /* Slot zero is valid. Ownership must be checked by FrameStore. */
}

static int ti_digest_present(const uint8_t digest[TI_DIGEST_SHA256_BYTES]) {
    uint32_t i;
    for (i = 0u; i < TI_DIGEST_SHA256_BYTES; ++i)
        if (digest[i]) return 1;
    return 0;
}

ti_status ti_observation_validate(const ti_observation_v1 *observation) {
    ti_status status;
    const uint32_t scale = 1000000u;
    if (!observation) return TI_EINVAL;
    status = ti_validate_abi(observation->size, observation->version,
                             (uint32_t)sizeof(*observation));
    if (status != TI_OK) return status;
    status = ti_frame_ref_validate(&observation->frame);
    if (status != TI_OK) return status;
    if (!observation->observation_id || !observation->capture_sequence ||
        !observation->output_schema_id || !observation->target_class_id ||
        !ti_digest_present(observation->model_sha256)) return TI_EINVAL;
    if (observation->confidence_ppm > scale ||
        observation->x_normalized > scale || observation->y_normalized > scale ||
        observation->width_normalized == 0u || observation->height_normalized == 0u ||
        observation->width_normalized > scale - observation->x_normalized ||
        observation->height_normalized > scale - observation->y_normalized) return TI_ERANGE;
    return TI_OK;
}

ti_status ti_event_header_validate(const ti_event_header_v1 *event) {
    ti_status status;
    if (!event) return TI_EINVAL;
    status = ti_validate_abi(event->size, event->version, (uint32_t)sizeof(*event));
    if (status != TI_OK) return status;
    if (event->kind < TI_EVENT_OBSERVATION || event->kind > TI_EVENT_OWNER_STATUS ||
        event->flags != 0u || !event->event_id || !event->plan_generation ||
        !event->session_generation || !event->schema_id) return TI_EINVAL;
    if (event->payload_bytes > TI_MAX_EVENT_PAYLOAD_BYTES) return TI_ERANGE;
    if ((event->kind == TI_EVENT_ANALYSIS_RESULT || event->kind == TI_EVENT_ACTION_RECEIPT) &&
        !event->correlation_id) return TI_EINVAL;
    return TI_OK;
}

static uint32_t ti_required_capability(uint32_t kind) {
    switch (kind) {
        case TI_ACTION_POINTER_DOWN:
        case TI_ACTION_POINTER_MOVE:
        case TI_ACTION_POINTER_UP: return TI_ACTION_CAP_POINTER;
        case TI_ACTION_GESTURE_TAP: return TI_ACTION_CAP_GESTURE;
        case TI_ACTION_UI_NODE_CLICK: return TI_ACTION_CAP_UI_NODE;
        case TI_ACTION_SYSTEM_BACK: return TI_ACTION_CAP_SYSTEM;
        default: return 0u;
    }
}

ti_status ti_action_intent_preflight(const ti_action_intent_v1 *intent,
                                     const ti_action_context_v1 *context) {
    ti_status status;
    uint32_t required;
    if (!intent || !context) return TI_EINVAL;
    status = ti_validate_abi(intent->size, intent->version, (uint32_t)sizeof(*intent));
    if (status != TI_OK) return status;
    status = ti_validate_abi(context->size, context->version, (uint32_t)sizeof(*context));
    if (status != TI_OK) return status;
    required = ti_required_capability(intent->kind);
    if (!required) return TI_EUNSUPPORTED;
    if (!intent->provider_id || !intent->action_id || !intent->attempt_id ||
        !intent->plan_generation || !intent->session_generation ||
        !intent->display_generation || !intent->deadline_ns ||
        !context->provider_id || !context->plan_generation ||
        !context->session_generation || !context->display_generation ||
        context->authorized > 1u || context->reserved) return TI_EINVAL;
    if (intent->provider_id != context->provider_id) return TI_EUNSUPPORTED;
    if (intent->plan_generation != context->plan_generation ||
        intent->session_generation != context->session_generation ||
        intent->display_generation != context->display_generation) return TI_ESTALE;
    if (!context->authorized) return TI_EAUTH;
    if ((context->capabilities & required) == 0u) return TI_EUNSUPPORTED;
    if (intent->deadline_ns <= context->now_ns) return TI_EEXPIRED;
    if (intent->kind != TI_ACTION_SYSTEM_BACK && !intent->evidence_id) return TI_EINVAL;
    if (intent->kind == TI_ACTION_UI_NODE_CLICK && !intent->target_id) return TI_EINVAL;
    if ((required == TI_ACTION_CAP_POINTER || required == TI_ACTION_CAP_GESTURE) &&
        (intent->x_normalized > 1000000u || intent->y_normalized > 1000000u)) return TI_ERANGE;
    if (required == TI_ACTION_CAP_POINTER && intent->pointer_id == 0u) return TI_EINVAL;
    return TI_OK;
}

ti_status ti_action_receipt_validate(const ti_action_receipt_v1 *receipt) {
    ti_status status;
    if (!receipt) return TI_EINVAL;
    status = ti_validate_abi(receipt->size, receipt->version, (uint32_t)sizeof(*receipt));
    if (status != TI_OK) return status;
    if (receipt->outcome < TI_ACTION_COMPLETED || receipt->outcome > TI_ACTION_UNKNOWN ||
        receipt->level < TI_RECEIPT_TRANSPORT || receipt->level > TI_RECEIPT_BACKEND_TERMINAL ||
        !receipt->action_id || !receipt->attempt_id ||
        !receipt->plan_generation || !receipt->session_generation) return TI_EINVAL;
    /* Only a backend terminal receipt may assert an attempt's final outcome. */
    if (receipt->outcome == TI_ACTION_COMPLETED &&
        receipt->level != TI_RECEIPT_BACKEND_TERMINAL) return TI_EINVAL;
    return TI_OK;
}
