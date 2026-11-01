#include "turbo_interact/action_owner.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    ti_action_owner *owner;
    unsigned grants;
    unsigned dispatches;
    unsigned permit_sandbox;
    unsigned reject_grant;
    unsigned revoke_during_grant;
    unsigned revoke_during_dispatch;
    unsigned malformed_receipt;
    unsigned uncertain_dispatch;
    unsigned transport_only;
} fixture;

static ti_status grant(void *user, const ti_action_request_v1 *req) {
    fixture *f = (fixture *)user;
    f->grants++;
    assert(req->display.display_id == 7u);
    if (f->revoke_during_grant) {
        assert(ti_action_owner_revoke(f->owner) == TI_OK);
        return TI_OK;
    }
    if (f->reject_grant || (req->origin == TI_ACTION_ORIGIN_SANDBOX && !f->permit_sandbox))
        return TI_EAUTH;
    return TI_OK;
}

static ti_status dispatch(void *user, const ti_action_request_v1 *req,
                          ti_action_receipt_v1 *out) {
    fixture *f = (fixture *)user;
    ti_action_receipt_v1 r = TI_ACTION_RECEIPT_V1_INIT;
    ti_action_owner *borrowed;
    f->dispatches++;
    assert(req->display.display_id == 7u);
    if (f->revoke_during_dispatch) {
        borrowed = f->owner;
        assert(ti_action_owner_revoke(borrowed) == TI_OK);
        assert(ti_action_owner_destroy(&borrowed) == TI_ECONFLICT);
        assert(borrowed == f->owner);
    }
    if (f->uncertain_dispatch) return TI_EUNSUPPORTED;
    r.action_id = req->intent.action_id;
    r.attempt_id = req->intent.attempt_id;
    r.plan_generation = req->intent.plan_generation;
    r.session_generation = req->intent.session_generation;
    r.level = f->transport_only ? TI_RECEIPT_TRANSPORT : TI_RECEIPT_BACKEND_TERMINAL;
    r.outcome = TI_ACTION_COMPLETED;
    r.agent_sequence = 500u;
    if (f->malformed_receipt) r.action_id++;
    *out = r;
    return TI_OK;
}

static ti_action_owner_config_v1 make_config(fixture *f) {
    ti_action_owner_config_v1 c = TI_ACTION_OWNER_CONFIG_V1_INIT;
    c.provider_id = 17u;
    c.plan_generation = 11u;
    c.capabilities = TI_ACTION_CAP_GESTURE | TI_ACTION_CAP_SYSTEM;
    c.attempt_capacity = 4u;
    c.display.device_session_id = 92u;
    c.display.session_generation = 22u;
    c.display.display_id = 7u;
    c.display.display_generation = 5u;
    c.display.geometry_generation = 1u;
    c.grant = grant;
    c.dispatch = dispatch;
    c.user = f;
    return c;
}

static ti_action_request_v1 request(uint64_t action, uint64_t attempt, uint32_t origin) {
    ti_action_request_v1 r = TI_ACTION_REQUEST_V1_INIT;
    r.origin = origin;
    r.display.device_session_id = 92u;
    r.display.session_generation = 22u;
    r.display.display_id = 7u;
    r.display.display_generation = 5u;
    r.display.geometry_generation = 1u;
    r.intent.kind = TI_ACTION_GESTURE_TAP;
    r.intent.provider_id = 17u;
    r.intent.action_id = action;
    r.intent.attempt_id = attempt;
    r.intent.evidence_id = 33u;
    r.intent.plan_generation = 11u;
    r.intent.session_generation = 22u;
    r.intent.display_generation = 5u;
    r.intent.deadline_ns = 1500u;
    r.intent.x_normalized = 500000u;
    r.intent.y_normalized = 250000u;
    r.source_observation_id = 33u;
    r.source_capture_sequence = 10u;
    return r;
}

static void test_main_screen_and_stale_geometry(void) {
    fixture f = {0};
    ti_action_owner_config_v1 c = make_config(&f);
    ti_action_request_v1 req = request(5u, 1u, TI_ACTION_ORIGIN_MODEL);
    ti_action_receipt_v1 receipt = TI_ACTION_RECEIPT_V1_INIT;

    c.display.display_id = 0u;
    assert(ti_action_owner_create(&c, &f.owner) == TI_EINVAL && f.owner == NULL);
    c.display.display_id = 7u;
    assert(ti_action_owner_create(&c, &f.owner) == TI_OK);

    req.display.display_id = 0u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_EINVAL);
    req.display.display_id = 8u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_ESTALE);
    req.display.display_id = 7u;
    req.display.display_generation++;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_ESTALE);
    req.display.display_generation--;
    req.display.geometry_generation++;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_ESTALE);
    req.display.geometry_generation--;
    req.source_capture_sequence = 0u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_EINVAL);
    req.source_capture_sequence = 10u;
    req.source_observation_id = 34u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_EINVAL);
    req.source_observation_id = 33u;
    req.intent.version++;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_EVERSION);
    req.intent.version--;
    req.intent.deadline_ns = 100u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_EEXPIRED);
    req.intent.deadline_ns = 1500u;
    assert(f.grants == 0u && f.dispatches == 0u);

    assert(ti_action_owner_update_geometry(f.owner, 2u) == TI_OK);
    assert(ti_action_owner_update_geometry(f.owner, 2u) == TI_ESTALE);
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_ESTALE);
    req.display.geometry_generation = 2u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_OK);
    assert(receipt.level == TI_RECEIPT_BACKEND_TERMINAL);
    assert(receipt.outcome == TI_ACTION_COMPLETED);
    assert(f.grants == 1u && f.dispatches == 1u);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK && f.owner == NULL);
}

static void test_sandbox_cannot_bypass_host_grant(void) {
    fixture f = {0};
    ti_action_owner_config_v1 c = make_config(&f);
    ti_action_request_v1 req = request(6u, 1u, TI_ACTION_ORIGIN_SANDBOX);
    ti_action_receipt_v1 receipt = TI_ACTION_RECEIPT_V1_INIT;
    assert(ti_action_owner_create(&c, &f.owner) == TI_OK);

    receipt.action_id = 777u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_EAUTH);
    assert(receipt.action_id == 777u); /* Rejection must not fake a receipt. */
    assert(f.grants == 1u && f.dispatches == 0u);
    f.permit_sandbox = 1u; /* Trusted host explicitly enables sandbox input. */
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_OK);
    assert(f.dispatches == 1u);
    req.origin = TI_ACTION_ORIGIN_MODEL;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_ECONFLICT);
    assert(f.dispatches == 1u); /* Same attempt cannot replay via another origin. */
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
}

static void test_unknown_is_terminal_for_attempt(void) {
    fixture f = {0};
    ti_action_owner_config_v1 c = make_config(&f);
    ti_action_request_v1 req = request(8u, 2u, TI_ACTION_ORIGIN_SCRIPT);
    ti_action_receipt_v1 receipt = TI_ACTION_RECEIPT_V1_INIT;
    assert(ti_action_owner_create(&c, &f.owner) == TI_OK);
    f.uncertain_dispatch = 1u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_OK);
    assert(receipt.outcome == TI_ACTION_UNKNOWN);
    assert(receipt.level == TI_RECEIPT_AGENT_ACCEPTED);
    assert(receipt.agent_sequence == 0u);
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_ECONFLICT);
    assert(f.dispatches == 1u);

    f.uncertain_dispatch = 0u;
    req.intent.attempt_id = 3u;
    f.malformed_receipt = 1u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_OK);
    assert(receipt.outcome == TI_ACTION_UNKNOWN);
    req.intent.attempt_id = 4u;
    f.malformed_receipt = 0u;
    f.transport_only = 1u;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_OK);
    assert(receipt.outcome == TI_ACTION_UNKNOWN);
    assert(f.dispatches == 3u);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
}

static void test_capacity_and_no_eviction(void) {
    fixture f = {0};
    ti_action_owner_config_v1 c = make_config(&f);
    ti_action_request_v1 req = request(30u, 1u, TI_ACTION_ORIGIN_MODEL);
    ti_action_receipt_v1 receipt = TI_ACTION_RECEIPT_V1_INIT;
    c.attempt_capacity = 1u;
    assert(ti_action_owner_create(&c, &f.owner) == TI_OK);
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_OK);
    req.intent.action_id++;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_ERANGE);
    assert(f.dispatches == 1u);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
}

static void test_revoke_during_grant_and_dispatch(void) {
    fixture f = {0};
    ti_action_owner_config_v1 c = make_config(&f);
    ti_action_request_v1 req = request(60u, 1u, TI_ACTION_ORIGIN_MODEL);
    ti_action_receipt_v1 receipt = TI_ACTION_RECEIPT_V1_INIT;
    f.revoke_during_grant = 1u;
    assert(ti_action_owner_create(&c, &f.owner) == TI_OK);
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_EAUTH);
    assert(f.dispatches == 0u);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);

    memset(&f, 0, sizeof(f));
    c = make_config(&f);
    f.revoke_during_dispatch = 1u;
    assert(ti_action_owner_create(&c, &f.owner) == TI_OK);
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_OK);
    /* Already sent effects can finish even after concurrent revocation. */
    assert(receipt.level == TI_RECEIPT_BACKEND_TERMINAL);
    assert(receipt.outcome == TI_ACTION_COMPLETED);
    req.intent.action_id++;
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_EAUTH);
    assert(ti_action_owner_update_geometry(f.owner, 2u) == TI_EAUTH);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
}

static void test_system_back_without_frame(void) {
    fixture f = {0};
    ti_action_owner_config_v1 c = make_config(&f);
    ti_action_request_v1 req = request(90u, 1u, TI_ACTION_ORIGIN_MODEL);
    ti_action_receipt_v1 receipt = TI_ACTION_RECEIPT_V1_INIT;
    req.intent.kind = TI_ACTION_SYSTEM_BACK;
    req.intent.evidence_id = 0u;
    req.source_observation_id = 0u;
    req.source_capture_sequence = 0u;
    assert(ti_action_owner_create(&c, &f.owner) == TI_OK);
    assert(ti_action_owner_submit(f.owner, &req, 100u, &receipt) == TI_OK);
    assert(receipt.outcome == TI_ACTION_COMPLETED);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
}

int main(void) {
    test_main_screen_and_stale_geometry();
    test_sandbox_cannot_bypass_host_grant();
    test_unknown_is_terminal_for_attempt();
    test_capacity_and_no_eviction();
    test_revoke_during_grant_and_dispatch();
    test_system_back_without_frame();
    puts("Action Owner device/sandbox authorization contract passed");
    return 0;
}
