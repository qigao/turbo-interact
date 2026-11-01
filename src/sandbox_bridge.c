#include "turbo_interact/sandbox_bridge.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

struct ti_sandbox_bridge {
    atomic_flag guard;
    ti_sandbox_bridge_config_v1 config;
    uint64_t last_call_id;
    uint32_t busy;
    uint32_t closed;
};

static void ti_lock(ti_sandbox_bridge *bridge) {
    while (atomic_flag_test_and_set_explicit(&bridge->guard, memory_order_acquire)) {
        /* No external callback may execute with this lock held. */
    }
}
static void ti_unlock(ti_sandbox_bridge *bridge) {
    atomic_flag_clear_explicit(&bridge->guard, memory_order_release);
}

static int ti_token_equal(const uint8_t *a, const uint8_t *b) {
    unsigned i;
    uint32_t diff = 0u;
    for (i = 0u; i < TI_SANDBOX_TOKEN_BYTES; i++) diff |= (uint32_t)(a[i] ^ b[i]);
    return diff == 0u;
}
static int ti_nonzero_token(const uint8_t *a) {
    unsigned i;
    uint32_t any = 0u;
    for (i = 0u; i < TI_SANDBOX_TOKEN_BYTES; i++) any |= a[i];
    return any != 0u;
}

ti_status ti_sandbox_bridge_create(const ti_sandbox_bridge_config_v1 *config,
                                   ti_sandbox_bridge **out) {
    ti_sandbox_bridge *bridge;
    if (!out) return TI_EINVAL;
    *out = NULL;
    if (!config || config->size != (uint32_t)sizeof(*config)) return TI_EINVAL;
    if (config->version != TI_ABI_VERSION) return TI_EVERSION;
    if (!config->peer_uid || !config->task_id || !config->sandbox_run_id ||
        !config->action_owner || !config->allowed_tools ||
        (config->allowed_tools & ~(TI_SANDBOX_PHONE_TAP | TI_SANDBOX_PHONE_BACK)) ||
        !ti_nonzero_token(config->token)) return TI_EINVAL;
    bridge = (ti_sandbox_bridge *)calloc(1u, sizeof(*bridge));
    if (!bridge) return TI_ENOMEM;
    atomic_flag_clear(&bridge->guard);
    bridge->config = *config;
    *out = bridge;
    return TI_OK;
}

ti_status ti_sandbox_bridge_execute(ti_sandbox_bridge *bridge,
                                    const ti_sandbox_phone_call_v1 *call,
                                    uint32_t peer_uid_from_os,
                                    const uint8_t token[TI_SANDBOX_TOKEN_BYTES],
                                    uint64_t now_ns,
                                    ti_action_receipt_v1 *receipt_out) {
    uint32_t required;
    ti_status status;
    if (!bridge || !call || !receipt_out) return TI_EINVAL;
    if (call->size != (uint32_t)sizeof(*call)) return TI_EINVAL;
    if (call->version != TI_ABI_VERSION) return TI_EVERSION;
    if (call->reserved || !call->call_id) return TI_EINVAL;
    ti_lock(bridge);
    if (bridge->closed) {
        ti_unlock(bridge);
        return TI_EAUTH;
    }
    if (bridge->busy) {
        ti_unlock(bridge);
        return TI_ECONFLICT;
    }
    if (!token || !peer_uid_from_os || peer_uid_from_os != bridge->config.peer_uid ||
        !ti_token_equal(token, bridge->config.token) ||
        call->task_id != bridge->config.task_id ||
        call->sandbox_run_id != bridge->config.sandbox_run_id) {
        ti_unlock(bridge);
        return TI_EAUTH;
    }
    if (call->call_id <= bridge->last_call_id) {
        ti_unlock(bridge);
        return TI_ECONFLICT;
    }
    required = call->tool == TI_SANDBOX_TOOL_TAP ? TI_SANDBOX_PHONE_TAP :
               call->tool == TI_SANDBOX_TOOL_BACK ? TI_SANDBOX_PHONE_BACK : 0u;
    if (!required || !(bridge->config.allowed_tools & required)) {
        ti_unlock(bridge);
        return TI_EUNSUPPORTED;
    }
    if ((call->action.origin != TI_ACTION_ORIGIN_SANDBOX &&
         call->action.origin != TI_ACTION_ORIGIN_SCRIPT) ||
        (call->tool == TI_SANDBOX_TOOL_TAP &&
         call->action.intent.kind != TI_ACTION_GESTURE_TAP) ||
        (call->tool == TI_SANDBOX_TOOL_BACK &&
         call->action.intent.kind != TI_ACTION_SYSTEM_BACK)) {
        ti_unlock(bridge);
        return TI_EINVAL;
    }
    /* Per command-session call IDs are consumed before effect admission.
     * No duplicate script invocation can replay a prior completed call. */
    bridge->last_call_id = call->call_id;
    bridge->busy = 1u;
    ti_unlock(bridge);

    status = ti_action_owner_submit(bridge->config.action_owner, &call->action,
                                    now_ns, receipt_out);
    ti_lock(bridge);
    bridge->busy = 0u;
    ti_unlock(bridge);
    return status;
}

ti_status ti_sandbox_bridge_close(ti_sandbox_bridge *bridge) {
    if (!bridge) return TI_EINVAL;
    ti_lock(bridge);
    bridge->closed = 1u;
    ti_unlock(bridge);
    return TI_OK;
}

ti_status ti_sandbox_bridge_destroy(ti_sandbox_bridge **bridge) {
    ti_sandbox_bridge *b;
    volatile uint8_t *secret;
    unsigned i;
    if (!bridge || !*bridge) return TI_EINVAL;
    b = *bridge;
    ti_lock(b);
    if (b->busy) {
        ti_unlock(b);
        return TI_ECONFLICT;
    }
    *bridge = NULL;
    ti_unlock(b);
    /* Explicitly clear the borrowed command secret when the session ends. */
    secret = b->config.token;
    for (i = 0u; i < TI_SANDBOX_TOKEN_BYTES; i++) secret[i] = 0u;
    free(b);
    return TI_OK;
}
