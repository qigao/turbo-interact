#ifndef TURBO_INTERACT_SANDBOX_IPC_H
#define TURBO_INTERACT_SANDBOX_IPC_H

#include "turbo_interact/sandbox_bridge.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TI_SANDBOX_IPC_MAX_LINE 65536u
#define TI_SANDBOX_IPC_PATH_CAP 104u
#define TI_SANDBOX_IPC_TOKEN_HEX_CAP 65u
#define TI_SANDBOX_IPC_MAX_DEADLINE_NS 30000000000ull

/*
 * Native host-side RPC (not an Android helper or shell command executor).
 * The host provides trusted device/display binding and a borrowed ActionOwner.
 * The sandbox process receives only a private Unix socket name and a random
 * per-command token; never the ActionOwner, Android Binder, or caller UID.
 */
typedef struct ti_sandbox_ipc_config_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t peer_uid;
    uint32_t allowed_tools;
    uint32_t provider_id;
    uint32_t reserved;
    uint64_t task_id;
    uint64_t sandbox_run_id;
    uint64_t plan_generation;
    ti_display_ref_v1 display;
    ti_action_owner *action_owner;
} ti_sandbox_ipc_config_v1;

#define TI_SANDBOX_IPC_CONFIG_V1_INIT {sizeof(ti_sandbox_ipc_config_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, 0u, 0u, TI_DISPLAY_REF_V1_INIT, NULL}

typedef struct ti_sandbox_ipc ti_sandbox_ipc;

/*
 * POSIX: private_directory must be an existing, real directory owned by the
 * current effective UID with no group/other access (e.g. Android app filesDir).
 * Creates a randomized, 0600 pathname Unix listener and a fresh CSPRNG token.
 * No directory/installer/consumer project is created. Windows: EUNSUPPORTED.
 */
TI_API ti_status ti_sandbox_ipc_open(const ti_sandbox_ipc_config_v1 *config,
                                     const char *private_directory,
                                     ti_sandbox_ipc **out);

/* Only the trusted host passes this data to the single sandbox process.
 * Required capacities are above. Does not reveal token over the RPC itself. */
TI_API ti_status ti_sandbox_ipc_credentials(ti_sandbox_ipc *session,
                                            char *path, size_t path_capacity,
                                            char *token_hex, size_t token_capacity);

/*
 * Serves ONE complete, newline-terminated, strict flat JSON request on a
 * real AF_UNIX connection. Extracts peer UID from OS socket credentials
 * (SO_PEERCRED on Android/Linux, getpeereid on macOS). Never accepts JSON UID.
 * 64KiB line bound, no duplicate/unknown keys, 30s max action deadline.
 *
 * Required keys for tap:
 * token, tool, callId, actionId, attemptId, observationId,
 * captureSequence, displayGeneration, geometryGeneration, deadlineNs, x, y
 * (x/y in normalized 0..1000000). For back, x/y are omitted, observation
 * and capture sequence may be zero. All other fields are forbidden.
 *
 * Sends {"ok":true|false,"status":integer,"outcome":integer,"level":integer}.
 * Return is the authorization/dispatch status, or transport error; TI_OK
 * does NOT mean the user's task was verified on screen.
 *
 * The host supplies a monotonic now_ns in the SAME clock domain as its
 * ActionOwner. timeout_ms in 1..10000 bounds accept, read and write.
 * The owner/bridge are borrowed during the call; do not concurrently destroy.
 */
TI_API ti_status ti_sandbox_ipc_serve_one(ti_sandbox_ipc *session,
                                          uint64_t now_ns, uint32_t timeout_ms);

/* Idempotent terminal close (not an ActionOwner revoke); busy returns ECONFLICT
 * instead of freeing a socket/bridge while an RPC is in progress. */
TI_API ti_status ti_sandbox_ipc_close(ti_sandbox_ipc *session);
TI_API ti_status ti_sandbox_ipc_destroy(ti_sandbox_ipc **session);

#ifdef __cplusplus
}
#endif
#endif
