#include "turbo_interact/android_shell_backend.h"

#include <limits.h>
#include <stdint.h>
#include <stddef.h>

ti_status ti_android_shell_backend_validate(const ti_android_shell_backend_v1 *b) {
    if (!b || b->size != (uint32_t)sizeof(*b)) return TI_EINVAL;
    if (b->version != TI_ABI_VERSION) return TI_EVERSION;
    if (!b->device_session_id || !b->session_generation ||
        !b->display_id || b->display_id > INT32_MAX ||
        !b->display_generation || b->reserved ||
        !b->geometry || !b->tap || !b->key) return TI_EINVAL;
    return TI_OK;
}

static int32_t ti_android_px(uint32_t normalized, uint32_t pixels) {
    return (int32_t)(((uint64_t)normalized * (pixels - 1u) + 500000u) / 1000000u);
}

ti_status ti_android_shell_backend_dispatch(
    void *opaque, const ti_action_request_v1 *req,
    ti_action_receipt_v1 *out) {
    const ti_android_shell_backend_v1 *backend = (const ti_android_shell_backend_v1 *)opaque;
    ti_action_receipt_v1 result = TI_ACTION_RECEIPT_V1_INIT;
    uint32_t width = 0u, height = 0u;
    ti_status st;
    int32_t sent;
    if (!req || !out) return TI_EINVAL;
    st = ti_android_shell_backend_validate(backend);
    if (st != TI_OK) return st;
    st = ti_display_ref_validate(&req->display);
    if (st != TI_OK) return st;
    if (req->display.device_session_id != backend->device_session_id ||
        req->display.session_generation != backend->session_generation ||
        req->display.display_id != backend->display_id ||
        req->display.display_generation != backend->display_generation ||
        req->intent.session_generation != req->display.session_generation ||
        req->intent.display_generation != req->display.display_generation ||
        req->display.geometry_generation == 0u) return TI_ESTALE;
    if (req->intent.kind != TI_ACTION_GESTURE_TAP &&
        req->intent.kind != TI_ACTION_SYSTEM_BACK) return TI_EUNSUPPORTED;
    if (!req->intent.action_id || !req->intent.attempt_id ||
        !req->intent.plan_generation || !req->intent.deadline_ns)
        return TI_EINVAL;

    /* The device-side geometry callback must refuse stale or released refs.
     * Never assume that dimensions from a prior frame remain current. */
    st = backend->geometry(backend->user, &req->display, &width, &height);
    if (st != TI_OK) return st;
    if (!width || !height || width > 4096u || height > 4096u ||
        (uint64_t)width * height > 8388608u) return TI_ERANGE;
    if (req->intent.kind == TI_ACTION_GESTURE_TAP) {
        if (req->intent.x_normalized > 1000000u ||
            req->intent.y_normalized > 1000000u) return TI_ERANGE;
        sent = backend->tap(backend->user, (int32_t)backend->display_id,
            req->display.display_generation, req->display.geometry_generation,
            req->intent.action_id, req->intent.attempt_id,
            ti_android_px(req->intent.x_normalized, width),
            ti_android_px(req->intent.y_normalized, height));
    } else {
        sent = backend->key(backend->user, (int32_t)backend->display_id,
            req->display.display_generation, req->display.geometry_generation,
            req->intent.action_id, req->intent.attempt_id,
            TI_ANDROID_SHELL_KEY_BACK);
    }
    result.action_id = req->intent.action_id;
    result.attempt_id = req->intent.attempt_id;
    result.plan_generation = req->intent.plan_generation;
    result.session_generation = req->intent.session_generation;
    result.level = TI_RECEIPT_AGENT_ACCEPTED;
    result.outcome = TI_ACTION_UNKNOWN;
    if (sent == TI_ANDROID_SHELL_SENT || sent == TI_ANDROID_SHELL_DENIED) {
        result.level = TI_RECEIPT_BACKEND_TERMINAL;
        result.outcome = sent == TI_ANDROID_SHELL_SENT ?
            TI_ACTION_COMPLETED : TI_ACTION_FAILED;
    }
    *out = result;
    return TI_OK;
}
