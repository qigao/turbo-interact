#include "turbo_interact/sandbox_bridge.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    ti_action_owner *owner;
    ti_sandbox_bridge *bridge;
    uint8_t token[TI_SANDBOX_TOKEN_BYTES];
    unsigned grant_count;
    unsigned device_count;
    unsigned allow_sandbox;
} fixture;

static ti_status grant(void *user, const ti_action_request_v1 *request) {
    fixture *f = (fixture *)user;
    f->grant_count++;
    if (request->origin != TI_ACTION_ORIGIN_MODEL && !f->allow_sandbox) return TI_EAUTH;
    return TI_OK;
}

static ti_status dispatch(void *user, const ti_action_request_v1 *request,
                          ti_action_receipt_v1 *receipt) {
    fixture *f = (fixture *)user;
    ti_action_receipt_v1 r = TI_ACTION_RECEIPT_V1_INIT;
    f->device_count++;
    r.action_id = request->intent.action_id;
    r.attempt_id = request->intent.attempt_id;
    r.plan_generation = request->intent.plan_generation;
    r.session_generation = request->intent.session_generation;
    r.outcome = TI_ACTION_COMPLETED;
    r.level = TI_RECEIPT_BACKEND_TERMINAL;
    *receipt = r;
    return TI_OK;
}

static void open_fixture(fixture *f) {
    ti_action_owner_config_v1 o = TI_ACTION_OWNER_CONFIG_V1_INIT;
    ti_sandbox_bridge_config_v1 b = TI_SANDBOX_BRIDGE_CONFIG_V1_INIT;
    unsigned k;
    memset(f, 0, sizeof(*f));
    for (k = 0u; k < TI_SANDBOX_TOKEN_BYTES; ++k) f->token[k] = (uint8_t)(k + 1u);
    o.provider_id = 17u;
    o.capabilities = TI_ACTION_CAP_GESTURE | TI_ACTION_CAP_SYSTEM;
    o.plan_generation = 3u;
    o.attempt_capacity = 12u;
    o.display.device_session_id = 5u;
    o.display.session_generation = 9u;
    o.display.display_id = 7u;
    o.display.display_generation = 2u;
    o.display.geometry_generation = 1u;
    o.grant = grant;
    o.dispatch = dispatch;
    o.user = f;
    assert(ti_action_owner_create(&o, &f->owner) == TI_OK);
    b.peer_uid = 10420u;
    b.allowed_tools = TI_SANDBOX_PHONE_TAP | TI_SANDBOX_PHONE_BACK;
    b.task_id = 100u;
    b.sandbox_run_id = 200u;
    memcpy(b.token, f->token, TI_SANDBOX_TOKEN_BYTES);
    b.action_owner = f->owner;
    assert(ti_sandbox_bridge_create(&b, &f->bridge) == TI_OK);
}

static ti_sandbox_phone_call_v1 phone(uint64_t call_id, uint64_t action_id) {
    ti_sandbox_phone_call_v1 call = TI_SANDBOX_PHONE_CALL_V1_INIT;
    call.tool = TI_SANDBOX_TOOL_TAP;
    call.task_id = 100u;
    call.sandbox_run_id = 200u;
    call.call_id = call_id;
    call.action.origin = TI_ACTION_ORIGIN_SANDBOX;
    call.action.display.device_session_id = 5u;
    call.action.display.session_generation = 9u;
    call.action.display.display_id = 7u;
    call.action.display.display_generation = 2u;
    call.action.display.geometry_generation = 1u;
    call.action.intent.kind = TI_ACTION_GESTURE_TAP;
    call.action.intent.provider_id = 17u;
    call.action.intent.action_id = action_id;
    call.action.intent.attempt_id = call_id;
    call.action.intent.evidence_id = 23u;
    call.action.intent.plan_generation = 3u;
    call.action.intent.session_generation = 9u;
    call.action.intent.display_generation = 2u;
    call.action.intent.deadline_ns = 1000u;
    call.action.intent.x_normalized = 100000u;
    call.action.intent.y_normalized = 900000u;
    call.action.source_observation_id = 23u;
    call.action.source_capture_sequence = 5u;
    return call;
}

static void cleanup(fixture *f) {
    assert(ti_sandbox_bridge_destroy(&f->bridge) == TI_OK);
    assert(f->bridge == NULL);
    assert(ti_action_owner_destroy(&f->owner) == TI_OK);
    assert(f->owner == NULL);
}

static void test_authentication_and_task_binding(void) {
    fixture f;
    ti_action_receipt_v1 r = TI_ACTION_RECEIPT_V1_INIT;
    ti_sandbox_phone_call_v1 q = phone(1u, 15u);
    uint8_t bad[TI_SANDBOX_TOKEN_BYTES];
    open_fixture(&f);
    memcpy(bad, f.token, sizeof(bad));
    bad[0] ^= 0x1u;
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, bad, 100u, &r) == TI_EAUTH);
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 999u, f.token, 100u, &r) == TI_EAUTH);
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, NULL, 100u, &r) == TI_EAUTH);
    q.task_id++;
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_EAUTH);
    q.task_id--;
    q.sandbox_run_id++;
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_EAUTH);
    q.sandbox_run_id--;
    assert(f.device_count == 0u && f.grant_count == 0u);

    /* Passing transport auth alone cannot bypass the independent host grant. */
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_EAUTH);
    assert(f.grant_count == 1u && f.device_count == 0u);
    f.allow_sandbox = 1u;
    q.call_id++; q.action.intent.attempt_id++;
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_OK);
    assert(r.outcome == TI_ACTION_COMPLETED);
    assert(f.device_count == 1u);
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_ECONFLICT);
    cleanup(&f);
}

static void test_tool_whitelist_and_main_screen(void) {
    fixture f;
    ti_action_receipt_v1 r = TI_ACTION_RECEIPT_V1_INIT;
    ti_sandbox_phone_call_v1 q = phone(1u, 42u);
    open_fixture(&f);
    f.allow_sandbox = 1u;
    q.tool = 99u;
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_EUNSUPPORTED);
    q.tool = TI_SANDBOX_TOOL_TAP;
    q.action.origin = TI_ACTION_ORIGIN_MODEL;
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_EINVAL);
    q.action.origin = TI_ACTION_ORIGIN_SCRIPT;
    q.action.display.display_id = 0u;
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_EINVAL);
    assert(f.device_count == 0u);
    q.action.display.display_id = 7u;
    q.call_id++; q.action.intent.attempt_id++;
    q.action.display.geometry_generation = 12u; /* old screenshot or resized geometry */
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_ESTALE);
    assert(f.device_count == 0u);
    q.call_id++; q.action.intent.attempt_id++;
    q.action.display.geometry_generation = 1u;
    q.tool = TI_SANDBOX_TOOL_BACK;
    q.action.intent.kind = TI_ACTION_SYSTEM_BACK;
    q.action.intent.evidence_id = 0u;
    q.action.source_observation_id = 0u;
    q.action.source_capture_sequence = 0u;
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_OK);
    assert(f.device_count == 1u);
    cleanup(&f);
}

static void test_bridge_lifetime_does_not_revoke_model_owner(void) {
    fixture f;
    ti_action_receipt_v1 r = TI_ACTION_RECEIPT_V1_INIT;
    ti_sandbox_phone_call_v1 q = phone(1u, 100u);
    open_fixture(&f);
    f.allow_sandbox = 1u;
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_OK);
    assert(ti_sandbox_bridge_close(f.bridge) == TI_OK);
    q.call_id++; q.action.intent.attempt_id++; q.action.intent.action_id++;
    assert(ti_sandbox_bridge_execute(f.bridge, &q, 10420u, f.token, 100u, &r) == TI_EAUTH);
    assert(f.device_count == 1u);
    assert(ti_sandbox_bridge_destroy(&f.bridge) == TI_OK);
    q.action.origin = TI_ACTION_ORIGIN_MODEL;
    assert(ti_action_owner_submit(f.owner, &q.action, 100u, &r) == TI_OK);
    assert(f.device_count == 2u); /* One ActionOwner survives closed sandbox. */
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
}

int main(void) {
    test_authentication_and_task_binding();
    test_tool_whitelist_and_main_screen();
    test_bridge_lifetime_does_not_revoke_model_owner();
    puts("sandbox phone bridge and single Action Owner policy contracts passed");
    return 0;
}
