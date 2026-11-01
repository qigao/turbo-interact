#include "turbo_interact/action_owner.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct ti_attempt_key {
    uint64_t action_id;
    uint64_t attempt_id;
} ti_attempt_key;

struct ti_action_owner {
    atomic_flag guard;
    ti_action_owner_config_v1 config;
    uint32_t count;
    uint32_t busy;
    uint32_t revoked;
    ti_attempt_key keys[];
};

static void ti_lock(ti_action_owner *owner) {
    while (atomic_flag_test_and_set_explicit(&owner->guard, memory_order_acquire)) {
        /* Short state-only critical section. No host callbacks under the lock. */
    }
}

static void ti_unlock(ti_action_owner *owner) {
    atomic_flag_clear_explicit(&owner->guard, memory_order_release);
}

ti_status ti_display_ref_validate(const ti_display_ref_v1 *ref) {
    if (!ref) return TI_EINVAL;
    if (ref->size != (uint32_t)sizeof(*ref)) return TI_EINVAL;
    if (ref->version != TI_ABI_VERSION) return TI_EVERSION;
    if (!ref->device_session_id || !ref->session_generation ||
        !ref->display_id || ref->reserved || !ref->display_generation ||
        !ref->geometry_generation) return TI_EINVAL;
    return TI_OK;
}

ti_status ti_action_owner_create(const ti_action_owner_config_v1 *config,
                                 ti_action_owner **out) {
    ti_action_owner *owner;
    ti_status status;
    size_t alloc_size;

    if (!out) return TI_EINVAL;
    *out = NULL;
    if (!config || config->size != (uint32_t)sizeof(*config)) return TI_EINVAL;
    if (config->version != TI_ABI_VERSION) return TI_EVERSION;
    status = ti_display_ref_validate(&config->display);
    if (status != TI_OK) return status;
    if (!config->provider_id || !config->plan_generation ||
        !config->capabilities ||
        (config->capabilities & ~(TI_ACTION_CAP_POINTER | TI_ACTION_CAP_GESTURE |
                                   TI_ACTION_CAP_UI_NODE | TI_ACTION_CAP_SYSTEM)) ||
        !config->attempt_capacity ||
        config->attempt_capacity > TI_ACTION_OWNER_MAX_ATTEMPTS ||
        config->reserved || !config->grant || !config->dispatch) return TI_EINVAL;
    alloc_size = sizeof(*owner) + (size_t)config->attempt_capacity * sizeof(ti_attempt_key);
    owner = (ti_action_owner *)calloc(1u, alloc_size);
    if (!owner) return TI_ENOMEM;
    atomic_flag_clear(&owner->guard);
    owner->config = *config;
    *out = owner;
    return TI_OK;
}

static ti_status ti_request_preflight_locked(ti_action_owner *owner,
                                             const ti_action_request_v1 *request,
                                             uint64_t now_ns) {
    ti_action_context_v1 context = TI_ACTION_CONTEXT_V1_INIT;
    ti_status status;
    ti_display_ref_v1 current = owner->config.display;
    if (!request || request->size != (uint32_t)sizeof(*request)) return TI_EINVAL;
    if (request->version != TI_ABI_VERSION) return TI_EVERSION;
    if (request->reserved || request->origin < TI_ACTION_ORIGIN_MODEL ||
        request->origin > TI_ACTION_ORIGIN_SCRIPT) return TI_EINVAL;
    status = ti_display_ref_validate(&request->display);
    if (status != TI_OK) return status;
    if (request->display.device_session_id != current.device_session_id ||
        request->display.session_generation != current.session_generation ||
        request->display.display_id != current.display_id ||
        request->display.display_generation != current.display_generation ||
        request->display.geometry_generation != current.geometry_generation) return TI_ESTALE;

    context.provider_id = owner->config.provider_id;
    context.capabilities = owner->config.capabilities;
    context.plan_generation = owner->config.plan_generation;
    context.session_generation = current.session_generation;
    context.display_generation = current.display_generation;
    context.now_ns = now_ns;
    context.authorized = 1u; /* Trusted preflight; live grant is checked later. */
    status = ti_action_intent_preflight(&request->intent, &context);
    if (status != TI_OK) return status;
    if (request->intent.kind != TI_ACTION_SYSTEM_BACK) {
        if (!request->source_observation_id || !request->source_capture_sequence ||
            request->source_observation_id != request->intent.evidence_id) return TI_EINVAL;
    } else if (request->source_observation_id &&
               request->intent.evidence_id != request->source_observation_id) return TI_EINVAL;
    return TI_OK;
}

static int ti_seen_locked(const ti_action_owner *owner, const ti_action_request_v1 *request) {
    uint32_t i;
    for (i = 0u; i < owner->count; ++i) {
        if (owner->keys[i].action_id == request->intent.action_id &&
            owner->keys[i].attempt_id == request->intent.attempt_id) return 1;
    }
    return 0;
}

static ti_action_receipt_v1 ti_unknown(const ti_action_request_v1 *req) {
    ti_action_receipt_v1 receipt = TI_ACTION_RECEIPT_V1_INIT;
    receipt.outcome = TI_ACTION_UNKNOWN;
    receipt.level = TI_RECEIPT_AGENT_ACCEPTED;
    receipt.action_id = req->intent.action_id;
    receipt.attempt_id = req->intent.attempt_id;
    receipt.plan_generation = req->intent.plan_generation;
    receipt.session_generation = req->intent.session_generation;
    return receipt;
}

static int ti_receipt_matches(const ti_action_receipt_v1 *receipt,
                              const ti_action_request_v1 *req) {
    if (ti_action_receipt_validate(receipt) != TI_OK ||
        receipt->action_id != req->intent.action_id ||
        receipt->attempt_id != req->intent.attempt_id ||
        receipt->plan_generation != req->intent.plan_generation ||
        receipt->session_generation != req->intent.session_generation) return 0;
    /* Transport/agent-level success or failure is not a terminal result. */
    if (receipt->level != TI_RECEIPT_BACKEND_TERMINAL &&
        receipt->outcome != TI_ACTION_UNKNOWN) return 0;
    return 1;
}

ti_status ti_action_owner_submit(ti_action_owner *owner,
                                 const ti_action_request_v1 *request,
                                 uint64_t now_ns,
                                 ti_action_receipt_v1 *receipt_out) {
    ti_action_receipt_v1 from_backend = TI_ACTION_RECEIPT_V1_INIT;
    ti_action_receipt_v1 result;
    ti_status status;
    ti_status callback_result;
    if (!owner || !request || !receipt_out) return TI_EINVAL;

    ti_lock(owner);
    if (owner->revoked) {
        ti_unlock(owner);
        return TI_EAUTH;
    }
    if (owner->busy) {
        ti_unlock(owner);
        return TI_ECONFLICT;
    }
    status = ti_request_preflight_locked(owner, request, now_ns);
    if (status == TI_OK && ti_seen_locked(owner, request)) status = TI_ECONFLICT;
    if (status == TI_OK && owner->count == owner->config.attempt_capacity) status = TI_ERANGE;
    if (status != TI_OK) {
        ti_unlock(owner);
        return status;
    }
    owner->busy = 1u;
    ti_unlock(owner);

    /* No callback under an internal mutex; callbacks may revoke this owner. */
    callback_result = owner->config.grant(owner->config.user, request);

    ti_lock(owner);
    if (owner->revoked) status = TI_EAUTH;
    else if (callback_result != TI_OK) status = TI_EAUTH;
    else {
        /* An attempt is consumed BEFORE dispatch; even UNKNOWN is never replayed. */
        owner->keys[owner->count].action_id = request->intent.action_id;
        owner->keys[owner->count].attempt_id = request->intent.attempt_id;
        ++owner->count;
        status = TI_OK;
    }
    if (status != TI_OK) owner->busy = 0u;
    ti_unlock(owner);
    if (status != TI_OK) return status;

    callback_result = owner->config.dispatch(owner->config.user, request, &from_backend);
    result = (callback_result == TI_OK && ti_receipt_matches(&from_backend, request))
        ? from_backend : ti_unknown(request);

    ti_lock(owner);
    owner->busy = 0u;
    ti_unlock(owner);
    *receipt_out = result;
    return TI_OK;
}

ti_status ti_action_owner_update_geometry(ti_action_owner *owner,
                                          uint64_t next_geometry_generation) {
    ti_status status;
    if (!owner) return TI_EINVAL;
    ti_lock(owner);
    if (owner->revoked) status = TI_EAUTH;
    else if (owner->busy) status = TI_ECONFLICT;
    else if (next_geometry_generation <= owner->config.display.geometry_generation) status = TI_ESTALE;
    else {
        owner->config.display.geometry_generation = next_geometry_generation;
        status = TI_OK;
    }
    ti_unlock(owner);
    return status;
}

ti_status ti_action_owner_revoke(ti_action_owner *owner) {
    if (!owner) return TI_EINVAL;
    ti_lock(owner);
    owner->revoked = 1u;
    ti_unlock(owner);
    return TI_OK;
}

ti_status ti_action_owner_destroy(ti_action_owner **owner) {
    ti_action_owner *value;
    if (!owner || !*owner) return TI_EINVAL;
    value = *owner;
    ti_lock(value);
    if (value->busy) {
        ti_unlock(value);
        return TI_ECONFLICT;
    }
    /* Only owner-dispatched callbacks may borrow this pointer. After the last
     * submission returns, callers must synchronize any external API users. */
    *owner = NULL;
    ti_unlock(value);
    free(value);
    return TI_OK;
}
