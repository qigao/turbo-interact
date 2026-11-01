#ifndef TURBO_INTERACT_ACTION_OWNER_H
#define TURBO_INTERACT_ACTION_OWNER_H

#include "turbo_interact/domain.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TI_ACTION_OWNER_MAX_ATTEMPTS 4096u

/*
 * Android display ID is not a global identity or an authorization token.
 * This pointer-free value is shared by model requests and sandbox script
 * callbacks; do not reuse it after a reconnect or display recreation.
 */
typedef struct ti_display_ref_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t device_session_id;
    uint64_t session_generation;
    uint32_t display_id;
    uint32_t reserved;
    uint64_t display_generation;
    uint64_t geometry_generation;
} ti_display_ref_v1;

#define TI_DISPLAY_REF_V1_INIT {sizeof(ti_display_ref_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, 0u}

TI_API ti_status ti_display_ref_validate(const ti_display_ref_v1 *ref);

typedef enum ti_action_origin {
    TI_ACTION_ORIGIN_MODEL = 1,
    TI_ACTION_ORIGIN_SANDBOX = 2,
    TI_ACTION_ORIGIN_SCRIPT = 3
} ti_action_origin;

/*
 * A validated/signed sandbox request MUST still be admitted through the same
 * Action Owner as direct model calls; origin cannot confer permissions.
 *
 * Evidence is a weak ID/sequence pair. A separate capture-compositor barrier
 * is needed to prove that a later frame occurred after an action.
 */
typedef struct ti_action_request_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t origin;
    uint32_t reserved;
    ti_display_ref_v1 display;
    ti_action_intent_v1 intent;
    uint64_t source_observation_id;
    uint64_t source_capture_sequence;
} ti_action_request_v1;

#define TI_ACTION_REQUEST_V1_INIT {sizeof(ti_action_request_v1), TI_ABI_VERSION, 0u, 0u, TI_DISPLAY_REF_V1_INIT, TI_ACTION_INTENT_V1_INIT, 0u, 0u}

/* Called for EVERY attempt, outside the owner's internal lock. The callback
 * must consult current host authority, including origin/tool-specific grants.
 * It must not accept untrusted caller-supplied 'authorized' booleans. */
typedef ti_status (*ti_action_live_grant_fn)(void *user,
                                              const ti_action_request_v1 *request);

/*
 * One external dispatch per admitted attempt. The callback may return a
 * BACKEND_TERMINAL receipt only when the device has reported a real terminal
 * result; a transport ACK is not completion. Otherwise report UNKNOWN.
 *
 * If the callback returns an error or an untrusted/mismatched receipt, the
 * owner records UNKNOWN and NEVER retries that (action_id, attempt_id).
 */
typedef ti_status (*ti_action_device_dispatch_fn)(void *user,
                                                    const ti_action_request_v1 *request,
                                                    ti_action_receipt_v1 *receipt);

typedef struct ti_action_owner_config_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t provider_id;
    uint32_t capabilities;
    uint32_t attempt_capacity;
    uint32_t reserved;
    uint64_t plan_generation;
    ti_display_ref_v1 display;
    ti_action_live_grant_fn grant;
    ti_action_device_dispatch_fn dispatch;
    void *user;
} ti_action_owner_config_v1;

#define TI_ACTION_OWNER_CONFIG_V1_INIT {sizeof(ti_action_owner_config_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, TI_DISPLAY_REF_V1_INIT, NULL, NULL, NULL}

typedef struct ti_action_owner ti_action_owner;

/* Allocates only bounded in-process attempt history. No device or graph starts. */
TI_API ti_status ti_action_owner_create(const ti_action_owner_config_v1 *config,
                                         ti_action_owner **out);

/*
 * On rejection, *receipt_out remains unchanged and no device effect begins.
 * On TI_OK, an attempt has been consumed, possibly with UNKNOWN outcome.
 * A revoked/in-flight attempt is not implicitly replayed.
 *
 * Per-owner dispatch is serialized. Reentrant/concurrent submissions return
 * TI_ECONFLICT rather than racing physical touch ownership.
 */
TI_API ti_status ti_action_owner_submit(ti_action_owner *owner,
                                         const ti_action_request_v1 *request,
                                         uint64_t now_ns,
                                         ti_action_receipt_v1 *receipt_out);

/* Bumps only the geometry generation, preserving the display incarnation.
 * Existing screenshots/targets then become stale. Fails while busy/revoked. */
TI_API ti_status ti_action_owner_update_geometry(ti_action_owner *owner,
                                                   uint64_t next_geometry_generation);

/*
 * Revoke makes future attempts fail immediately; a previously admitted
 * in-flight device effect cannot be un-sent. Its backend receipt or UNKNOWN
 * must still be recorded. No assumption of instantaneous hardware cancel.
 */
TI_API ti_status ti_action_owner_revoke(ti_action_owner *owner);

/* Returns TI_ECONFLICT if a dispatch/grant callback is still in flight.
 * On success sets *owner to NULL; never releases external Android resources. */
TI_API ti_status ti_action_owner_destroy(ti_action_owner **owner);

#ifdef __cplusplus
}
#endif
#endif
