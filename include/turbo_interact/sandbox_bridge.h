#ifndef TURBO_INTERACT_SANDBOX_BRIDGE_H
#define TURBO_INTERACT_SANDBOX_BRIDGE_H

#include "turbo_interact/action_owner.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TI_SANDBOX_TOKEN_BYTES 32u

enum {
    TI_SANDBOX_PHONE_TAP = 1u << 0,
    TI_SANDBOX_PHONE_BACK = 1u << 1
};

typedef enum ti_sandbox_phone_tool {
    TI_SANDBOX_TOOL_TAP = 1,
    TI_SANDBOX_TOOL_BACK = 2
} ti_sandbox_phone_tool;

/*
 * An authenticated command's sandbox tool invocation, NOT a raw JSON packet.
 * The socket/JSON adapter MUST parse and bound inputs before constructing it.
 * It must extract peer UID from OS socket credentials, never from JSON.
 */
typedef struct ti_sandbox_phone_call_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t tool;
    uint32_t reserved;
    uint64_t task_id;
    uint64_t sandbox_run_id;
    uint64_t call_id;
    ti_action_request_v1 action;
} ti_sandbox_phone_call_v1;

#define TI_SANDBOX_PHONE_CALL_V1_INIT {sizeof(ti_sandbox_phone_call_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, TI_ACTION_REQUEST_V1_INIT}

/* The owner is BORROWED. A bridge cannot create or grant Android permissions.
 * The token MUST be generated from a cryptographically secure random source
 * by the host. One bridge instance is scoped to one task and one sandbox run.
 */
typedef struct ti_sandbox_bridge_config_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t peer_uid;
    uint32_t allowed_tools;
    uint64_t task_id;
    uint64_t sandbox_run_id;
    uint8_t token[TI_SANDBOX_TOKEN_BYTES];
    ti_action_owner *action_owner;
} ti_sandbox_bridge_config_v1;

#define TI_SANDBOX_BRIDGE_CONFIG_V1_INIT {sizeof(ti_sandbox_bridge_config_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, {0}, NULL}

typedef struct ti_sandbox_bridge ti_sandbox_bridge;

TI_API ti_status ti_sandbox_bridge_create(const ti_sandbox_bridge_config_v1 *config,
                                           ti_sandbox_bridge **out);

/*
 * The trusted transport supplies peer_uid_from_os (SO_PEERCRED or equivalent),
 * not untrusted request data. This function checks a constant-time token,
 * tool allow-list and monotonic call IDs, then uses the SAME ActionOwner as
 * direct model calls. It never calls a device/input backend itself.
 *
 * TI_OK means the ActionOwner consumed the action attempt; UNKNOWN may be the
 * receipt. A failed authorization produces no receipt.
 */
TI_API ti_status ti_sandbox_bridge_execute(ti_sandbox_bridge *bridge,
                                            const ti_sandbox_phone_call_v1 *call,
                                            uint32_t peer_uid_from_os,
                                            const uint8_t token[TI_SANDBOX_TOKEN_BYTES],
                                            uint64_t now_ns,
                                            ti_action_receipt_v1 *receipt_out);

/* Close the sandbox's command session; does not revoke the shared ActionOwner.
 * In-flight external effects must settle or report UNKNOWN independently. */
TI_API ti_status ti_sandbox_bridge_close(ti_sandbox_bridge *bridge);
TI_API ti_status ti_sandbox_bridge_destroy(ti_sandbox_bridge **bridge);

#ifdef __cplusplus
}
#endif
#endif
