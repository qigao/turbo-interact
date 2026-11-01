#ifndef TURBO_INTERACT_ANDROID_SHELL_BACKEND_H
#define TURBO_INTERACT_ANDROID_SHELL_BACKEND_H

#include "turbo_interact/action_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ABI-facing callback shape for Android PR #17's ITurboDisplayShell AIDL.
 * This adapter is one dispatch callback for an EXISTING ti_action_owner.
 * It never creates an alternate authority, action ledger or privilege grant.
 *
 * All native callbacks are provided by the embedding Android host/JNI layer.
 * No JNI, Binder, Android SDK or real device access is implied by this API.
 */
enum {
    TI_ANDROID_SHELL_SENT = 1,
    TI_ANDROID_SHELL_DENIED = -1,
    TI_ANDROID_SHELL_UNKNOWN = -2,
    TI_ANDROID_SHELL_KEY_BACK = 1
};

/* Must return the dimensions of the EXACT currently active display/geometry
 * version. Stale, missing, or unowned refs must fail; no display-0 fallback.
 * The actual Android Kotlin registry and DisplayOwner remain authoritative
 * for release/rotation races at final input injection. */
typedef ti_status (*ti_android_shell_geometry_fn)(
    void *user, const ti_display_ref_v1 *display,
    uint32_t *width, uint32_t *height);

/* Return only SENT / DENIED / UNKNOWN as defined above; all other results
 * are treated as UNKNOWN rather than silently assuming success. */
typedef int32_t (*ti_android_shell_tap_fn)(
    void *user, int32_t display_id, uint64_t display_generation,
    uint64_t geometry_generation, uint64_t action_id, uint64_t attempt_id,
    int32_t x_px, int32_t y_px);

typedef int32_t (*ti_android_shell_key_fn)(
    void *user, int32_t display_id, uint64_t display_generation,
    uint64_t geometry_generation, uint64_t action_id, uint64_t attempt_id,
    int32_t kind);

typedef struct ti_android_shell_backend_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t device_session_id;
    uint64_t session_generation;
    uint32_t display_id;
    uint32_t reserved;
    uint64_t display_generation;
    ti_android_shell_geometry_fn geometry;
    ti_android_shell_tap_fn tap;
    ti_android_shell_key_fn key;
    void *user;
} ti_android_shell_backend_v1;

#define TI_ANDROID_SHELL_BACKEND_V1_INIT {sizeof(ti_android_shell_backend_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, NULL, NULL, NULL, NULL}

/* Validate endpoint binding before handing it to a live ActionOwner. */
TI_API ti_status ti_android_shell_backend_validate(
    const ti_android_shell_backend_v1 *backend);

/*
 * Pass this as the dispatch callback for ti_action_owner_create, with its
 * user pointer referring to a trusted host-owned binding/context. The host
 * still supplies its own live-grant callback to the SINGLE owner.
 *
 * Supports only GESTURE_TAP and SYSTEM_BACK in Android PR #17's narrow AIDL;
 * other action kinds fail closed, never fall back to generic shell input.
 *
 * An Android SDK SENT is a *backend-terminal input receipt* only. It never
 * proves the app accepted the tap or an end-user task succeeded.
 * Binder failure, an unrecognized status, or an unqualified geometry is
 * UNKNOWN to ActionOwner; that attempt is consumed and must not auto replay.
 */
TI_API ti_status ti_android_shell_backend_dispatch(
    void *backend, const ti_action_request_v1 *request,
    ti_action_receipt_v1 *receipt_out);

#ifdef __cplusplus
}
#endif
#endif
