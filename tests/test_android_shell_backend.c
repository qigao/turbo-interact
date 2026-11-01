#include "turbo_interact/android_shell_backend.h"
#include "turbo_interact/sandbox_bridge.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    /* Keep backend first: ActionOwner's config.user is the exact same
     * fixture pointer used by the trusted grant callback. */
    ti_android_shell_backend_v1 backend;
    ti_action_owner *owner;
    uint32_t width, height;
    uint64_t geometry;
    unsigned grants, taps, keys;
    int grant_sandbox;
    int32_t return_code;
    int32_t last_x, last_y;
    uint64_t last_action;
} fixture;

static ti_status grant(void *opaque, const ti_action_request_v1 *request) {
    fixture *f = (fixture *)opaque;
    f->grants++;
    if (request->origin == TI_ACTION_ORIGIN_SANDBOX && !f->grant_sandbox)
        return TI_EAUTH;
    return TI_OK;
}
static ti_status geometry(void *opaque, const ti_display_ref_v1 *ref,
                          uint32_t *width, uint32_t *height) {
    fixture *f = (fixture *)opaque;
    if (ref->display_id != 7u || ref->display_generation != 3u ||
        ref->geometry_generation != f->geometry) return TI_ESTALE;
    *width = f->width;
    *height = f->height;
    return TI_OK;
}
static int32_t tap(void *opaque, int32_t display_id, uint64_t display_generation,
                   uint64_t geometry_generation, uint64_t action_id,
                   uint64_t attempt_id, int32_t x_px, int32_t y_px) {
    fixture *f = (fixture *)opaque;
    assert(display_id == 7 && display_generation == 3u &&
           geometry_generation == f->geometry && attempt_id > 0u);
    f->taps++;
    f->last_x = x_px;
    f->last_y = y_px;
    f->last_action = action_id;
    return f->return_code;
}
static int32_t key(void *opaque, int32_t display_id, uint64_t display_generation,
                   uint64_t geometry_generation, uint64_t action_id,
                   uint64_t attempt_id, int32_t kind) {
    fixture *f = (fixture *)opaque;
    assert(display_id == 7 && display_generation == 3u &&
           geometry_generation == f->geometry && attempt_id > 0u &&
           kind == TI_ANDROID_SHELL_KEY_BACK);
    f->keys++;
    f->last_action = action_id;
    return f->return_code;
}

static void setup(fixture *f) {
    ti_action_owner_config_v1 c = TI_ACTION_OWNER_CONFIG_V1_INIT;
    ti_android_shell_backend_v1 b = TI_ANDROID_SHELL_BACKEND_V1_INIT;
    memset(f, 0, sizeof(*f));
    f->width = 1080u;
    f->height = 1920u;
    f->geometry = 1u;
    f->return_code = TI_ANDROID_SHELL_SENT;
    b.device_session_id = 2u;
    b.session_generation = 4u;
    b.display_id = 7u;
    b.display_generation = 3u;
    b.geometry = geometry;
    b.tap = tap;
    b.key = key;
    b.user = f;
    f->backend = b;
    assert(ti_android_shell_backend_validate(&f->backend) == TI_OK);
    c.provider_id = 17u;
    c.capabilities = TI_ACTION_CAP_GESTURE | TI_ACTION_CAP_SYSTEM;
    c.plan_generation = 11u;
    c.attempt_capacity = 12u;
    c.display.device_session_id = 2u;
    c.display.session_generation = 4u;
    c.display.display_id = 7u;
    c.display.display_generation = 3u;
    c.display.geometry_generation = 1u;
    c.grant = grant;
    c.dispatch = ti_android_shell_backend_dispatch;
    c.user = f;
    assert(ti_action_owner_create(&c, &f->owner) == TI_OK);
}

static ti_action_request_v1 make_tap(uint64_t action, uint64_t attempt) {
    ti_action_request_v1 r = TI_ACTION_REQUEST_V1_INIT;
    r.origin = TI_ACTION_ORIGIN_MODEL;
    r.display.device_session_id = 2u;
    r.display.session_generation = 4u;
    r.display.display_id = 7u;
    r.display.display_generation = 3u;
    r.display.geometry_generation = 1u;
    r.intent.kind = TI_ACTION_GESTURE_TAP;
    r.intent.provider_id = 17u;
    r.intent.action_id = action;
    r.intent.attempt_id = attempt;
    r.intent.evidence_id = 6u;
    r.intent.plan_generation = 11u;
    r.intent.session_generation = 4u;
    r.intent.display_generation = 3u;
    r.intent.deadline_ns = 10000u;
    r.intent.x_normalized = 500000u;
    r.intent.y_normalized = 250000u;
    r.source_observation_id = 6u;
    r.source_capture_sequence = 5u;
    return r;
}

static void test_exact_pixel_mapping_and_receipts(void) {
    fixture f;
    ti_action_request_v1 req = make_tap(8u, 1u);
    ti_action_receipt_v1 result = TI_ACTION_RECEIPT_V1_INIT;
    setup(&f);
    assert(ti_action_owner_submit(f.owner, &req, 100u, &result) == TI_OK);
    assert(f.grants == 1u && f.taps == 1u && f.keys == 0u);
    assert(f.last_x == 540 && f.last_y == 480 && f.last_action == 8u);
    assert(result.level == TI_RECEIPT_BACKEND_TERMINAL);
    assert(result.outcome == TI_ACTION_COMPLETED); /* Input dispatched; not App outcome. */
    assert(ti_action_owner_submit(f.owner, &req, 100u, &result) == TI_ECONFLICT);

    req.intent.action_id = 9u;
    f.return_code = TI_ANDROID_SHELL_DENIED;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &result) == TI_OK);
    assert(result.outcome == TI_ACTION_FAILED);
    req.intent.action_id = 10u;
    f.return_code = TI_ANDROID_SHELL_UNKNOWN;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &result) == TI_OK);
    assert(result.outcome == TI_ACTION_UNKNOWN);
    assert(result.level == TI_RECEIPT_AGENT_ACCEPTED);
    assert(ti_action_owner_submit(f.owner, &req, 100u, &result) == TI_ECONFLICT);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
}

static void test_geometry_and_back_mapping(void) {
    fixture f;
    ti_action_request_v1 req = make_tap(12u, 1u);
    ti_action_receipt_v1 result = TI_ACTION_RECEIPT_V1_INIT;
    setup(&f);
    req.display.geometry_generation = 2u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &result) == TI_ESTALE);
    assert(f.taps == 0u);
    assert(ti_action_owner_update_geometry(f.owner, 2u) == TI_OK);
    /* Device geometry must independently confirm the current version. */
    assert(ti_action_owner_submit(f.owner, &req, 100u, &result) == TI_OK);
    assert(result.outcome == TI_ACTION_UNKNOWN && f.taps == 0u);
    req.intent.action_id = 13u;
    f.geometry = 2u;
    f.width = 1600u;
    f.height = 720u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &result) == TI_OK);
    assert(f.last_x == 800 && f.last_y == 180);
    req.intent.kind = TI_ACTION_SYSTEM_BACK;
    req.intent.evidence_id = 0u;
    req.source_observation_id = 0u;
    req.source_capture_sequence = 0u;
    req.intent.action_id = 14u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &result) == TI_OK);
    assert(f.keys == 1u && result.level == TI_RECEIPT_BACKEND_TERMINAL);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
}

static void test_same_owner_for_model_and_sandbox(void) {
    fixture f;
    ti_sandbox_bridge_config_v1 b = TI_SANDBOX_BRIDGE_CONFIG_V1_INIT;
    ti_sandbox_phone_call_v1 call = TI_SANDBOX_PHONE_CALL_V1_INIT;
    ti_sandbox_bridge *bridge = NULL;
    ti_action_receipt_v1 out = TI_ACTION_RECEIPT_V1_INIT;
    unsigned k;
    setup(&f);
    b.peer_uid = 10001u;
    b.allowed_tools = TI_SANDBOX_PHONE_TAP;
    b.task_id = 16u;
    b.sandbox_run_id = 17u;
    b.action_owner = f.owner;
    for (k = 0u; k < TI_SANDBOX_TOKEN_BYTES; k++) b.token[k] = (uint8_t)(k + 1u);
    assert(ti_sandbox_bridge_create(&b, &bridge) == TI_OK);
    call.tool = TI_SANDBOX_TOOL_TAP;
    call.task_id = 16u;
    call.sandbox_run_id = 17u;
    call.call_id = 1u;
    call.action = make_tap(18u, 1u);
    call.action.origin = TI_ACTION_ORIGIN_SANDBOX;
    assert(ti_sandbox_bridge_execute(bridge, &call, 10001u, b.token, 100u, &out) == TI_EAUTH);
    assert(f.taps == 0u);
    f.grant_sandbox = 1;
    call.call_id = 2u;
    call.action.intent.attempt_id = 2u;
    assert(ti_sandbox_bridge_execute(bridge, &call, 10001u, b.token, 100u, &out) == TI_OK);
    assert(f.taps == 1u && out.outcome == TI_ACTION_COMPLETED);
    call.action.origin = TI_ACTION_ORIGIN_MODEL;
    assert(ti_action_owner_submit(f.owner, &call.action, 100u, &out) == TI_ECONFLICT);
    assert(f.taps == 1u);
    assert(ti_sandbox_bridge_destroy(&bridge) == TI_OK);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
}

int main(void) {
    test_exact_pixel_mapping_and_receipts();
    test_geometry_and_back_mapping();
    test_same_owner_for_model_and_sandbox();
    puts("Android AIDL input mapping via one ActionOwner verified (no device)");
    return 0;
}
