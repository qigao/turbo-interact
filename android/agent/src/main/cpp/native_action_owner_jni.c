/*
 * TurboInteract Android JNI: a bridge to the SAME production C11 ActionOwner.
 * No alternate action engine, privilege elevation, shell launch, or device
 * fallback. Java host callbacks own the already-authenticated Binder.
 */
#include "turbo_interact/action_owner.h"
#include "turbo_interact/android_shell_backend.h"

#include <jni.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/*
 * Private versioned Java/JNI fields, also defined by NativeInputAction.values:
 * [version, origin, kind, actionId, attemptId, evidenceId, captureSequence,
 *  deviceSessionId, sessionGeneration, displayId, displayGeneration,
 *  geometryGeneration, planGeneration, deadlineNs, xNorm, yNorm, nowNs].
 */
enum { TI_JNI_FIELDS = 17, TI_JNI_BACKEND_TAP_FIELDS = 7,
       TI_JNI_BACKEND_KEY_FIELDS = 6 };
enum {
    TI_JNI_VERSION = 0,
    TI_JNI_ORIGIN = 1, TI_JNI_KIND = 2,
    TI_JNI_ACTION = 3, TI_JNI_ATTEMPT = 4,
    TI_JNI_EVIDENCE = 5, TI_JNI_CAPTURE = 6,
    TI_JNI_DEVICE = 7, TI_JNI_SESSION = 8, TI_JNI_DISPLAY = 9,
    TI_JNI_DISPLAY_GEN = 10, TI_JNI_GEOM_GEN = 11,
    TI_JNI_PLAN = 12, TI_JNI_DEADLINE = 13,
    TI_JNI_X = 14, TI_JNI_Y = 15, TI_JNI_NOW = 16
};

typedef struct ti_native_owner {
    /* MUST be first: ti_android_shell_backend_dispatch receives config.user. */
    ti_android_shell_backend_v1 backend;
    ti_action_owner *owner;
    JavaVM *vm;
    jobject peer; /* GlobalRef, kept until native owner is no longer in use. */
    jmethodID grant_method;
    jmethodID geometry_method;
    jmethodID tap_method;
    jmethodID key_method;
    atomic_flag guard;
    uint32_t provider_id;
    int revoked;
    int proposed;
    int pending;
    int consumed;
    ti_action_request_v1 proposal;
} ti_native_owner;

static ti_native_owner *ti_owner(jlong handle) {
    return (ti_native_owner *)(uintptr_t)(uint64_t)handle;
}
static void ti_guard(ti_native_owner *owner) {
    while (atomic_flag_test_and_set_explicit(&owner->guard, memory_order_acquire)) {}
}
static void ti_unguard(ti_native_owner *owner) {
    atomic_flag_clear_explicit(&owner->guard, memory_order_release);
}
static JNIEnv *ti_current_env(ti_native_owner *owner) {
    JNIEnv *env = NULL;
    if (!owner->vm || (*owner->vm)->GetEnv(owner->vm, (void **)&env,
                                          JNI_VERSION_1_6) != JNI_OK) return NULL;
    return env;
}
static int ti_clear_java_exception(JNIEnv *env) {
    if (!(*env)->ExceptionCheck(env)) return 0;
    (*env)->ExceptionClear(env);
    return 1;
}
static jlongArray ti_java_longs(JNIEnv *env, const jlong *cells, jsize n) {
    jlongArray result = (*env)->NewLongArray(env, n);
    if (!result || ti_clear_java_exception(env)) return NULL;
    (*env)->SetLongArrayRegion(env, result, 0, n, cells);
    if (ti_clear_java_exception(env)) {
        (*env)->DeleteLocalRef(env, result);
        return NULL;
    }
    return result;
}
static void ti_request_cells(const ti_action_request_v1 *r, jlong values[TI_JNI_FIELDS]) {
    memset(values, 0, TI_JNI_FIELDS * sizeof(*values));
    values[TI_JNI_VERSION] = 1;
    values[TI_JNI_ORIGIN] = (jlong)r->origin;
    values[TI_JNI_KIND] = (jlong)r->intent.kind;
    values[TI_JNI_ACTION] = (jlong)r->intent.action_id;
    values[TI_JNI_ATTEMPT] = (jlong)r->intent.attempt_id;
    values[TI_JNI_EVIDENCE] = (jlong)r->intent.evidence_id;
    values[TI_JNI_CAPTURE] = (jlong)r->source_capture_sequence;
    values[TI_JNI_DEVICE] = (jlong)r->display.device_session_id;
    values[TI_JNI_SESSION] = (jlong)r->display.session_generation;
    values[TI_JNI_DISPLAY] = (jlong)r->display.display_id;
    values[TI_JNI_DISPLAY_GEN] = (jlong)r->display.display_generation;
    values[TI_JNI_GEOM_GEN] = (jlong)r->display.geometry_generation;
    values[TI_JNI_PLAN] = (jlong)r->intent.plan_generation;
    values[TI_JNI_DEADLINE] = (jlong)r->intent.deadline_ns;
    values[TI_JNI_X] = (jlong)r->intent.x_normalized;
    values[TI_JNI_Y] = (jlong)r->intent.y_normalized;
}

static ti_status ti_grant(void *user, const ti_action_request_v1 *request) {
    ti_native_owner *owner = (ti_native_owner *)user;
    JNIEnv *env = ti_current_env(owner);
    jlong values[TI_JNI_FIELDS];
    jlongArray input;
    jboolean accepted;
    int revoked;
    if (!env) return TI_EAUTH;
    ti_guard(owner);
    revoked = owner->revoked;
    ti_unguard(owner);
    if (revoked) return TI_EAUTH;
    ti_request_cells(request, values);
    input = ti_java_longs(env, values, TI_JNI_FIELDS);
    if (!input) return TI_EAUTH;
    accepted = (*env)->CallBooleanMethod(env, owner->peer, owner->grant_method, input);
    (*env)->DeleteLocalRef(env, input);
    if (ti_clear_java_exception(env) || accepted != JNI_TRUE) return TI_EAUTH;
    /* Not yet an admitted ticket. Only tap/key callback will publish a ticket
     * after C11 ActionOwner consumes this exact action/attempt. */
    ti_guard(owner);
    revoked = owner->revoked;
    if (!revoked) {
        owner->proposal = *request;
        owner->proposed = 1;
    }
    ti_unguard(owner);
    return revoked ? TI_EAUTH : TI_OK;
}

static ti_status ti_geometry(void *user, const ti_display_ref_v1 *display,
                             uint32_t *width, uint32_t *height) {
    ti_native_owner *owner = (ti_native_owner *)user;
    JNIEnv *env = ti_current_env(owner);
    jlong data[TI_JNI_FIELDS] = {0};
    jlongArray input;
    jlong packed;
    int revoked;
    if (!env || !display || !width || !height) return TI_ESTALE;
    ti_guard(owner);
    revoked = owner->revoked;
    ti_unguard(owner);
    if (revoked) return TI_EAUTH;
    data[TI_JNI_VERSION] = 1;
    data[TI_JNI_DEVICE] = (jlong)display->device_session_id;
    data[TI_JNI_SESSION] = (jlong)display->session_generation;
    data[TI_JNI_DISPLAY] = (jlong)display->display_id;
    data[TI_JNI_DISPLAY_GEN] = (jlong)display->display_generation;
    data[TI_JNI_GEOM_GEN] = (jlong)display->geometry_generation;
    input = ti_java_longs(env, data, TI_JNI_FIELDS);
    if (!input) return TI_EUNSUPPORTED;
    packed = (*env)->CallLongMethod(env, owner->peer, owner->geometry_method, input);
    (*env)->DeleteLocalRef(env, input);
    if (ti_clear_java_exception(env) || packed <= 0) return TI_ESTALE;
    *width = (uint32_t)((uint64_t)packed >> 32);
    *height = (uint32_t)((uint64_t)packed & UINT32_MAX);
    if (!*width || !*height) return TI_ESTALE;
    return TI_OK;
}

static int ti_begin_ticket(ti_native_owner *owner, uint32_t kind,
                           int32_t display_id, uint64_t display_generation,
                           uint64_t geometry_generation, uint64_t action_id,
                           uint64_t attempt_id) {
    const ti_action_request_v1 *request;
    int valid;
    ti_guard(owner);
    request = &owner->proposal;
    valid = !owner->revoked && owner->proposed && !owner->pending &&
        request->intent.kind == kind &&
        request->display.display_id == (uint32_t)display_id &&
        request->display.display_generation == display_generation &&
        request->display.geometry_generation == geometry_generation &&
        request->intent.action_id == action_id &&
        request->intent.attempt_id == attempt_id;
    if (valid) {
        owner->pending = 1;
        owner->consumed = 0;
    }
    ti_unguard(owner);
    return valid;
}
static void ti_end_ticket(ti_native_owner *owner) {
    ti_guard(owner);
    owner->pending = 0;
    owner->consumed = 0;
    owner->proposed = 0;
    ti_unguard(owner);
}
static int32_t ti_tap(void *user, int32_t display_id,
                      uint64_t display_generation, uint64_t geometry_generation,
                      uint64_t action_id, uint64_t attempt_id,
                      int32_t x, int32_t y) {
    ti_native_owner *owner = (ti_native_owner *)user;
    JNIEnv *env;
    jlong values[TI_JNI_BACKEND_TAP_FIELDS];
    jlongArray input;
    jint result;
    if (!ti_begin_ticket(owner, TI_ACTION_GESTURE_TAP, display_id,
                         display_generation, geometry_generation,
                         action_id, attempt_id)) return TI_ANDROID_SHELL_UNKNOWN;
    env = ti_current_env(owner);
    if (!env) { ti_end_ticket(owner); return TI_ANDROID_SHELL_UNKNOWN; }
    values[0] = (jlong)display_id;
    values[1] = (jlong)display_generation;
    values[2] = (jlong)geometry_generation;
    values[3] = (jlong)action_id;
    values[4] = (jlong)attempt_id;
    values[5] = (jlong)x;
    values[6] = (jlong)y;
    input = ti_java_longs(env, values, TI_JNI_BACKEND_TAP_FIELDS);
    if (!input) { ti_end_ticket(owner); return TI_ANDROID_SHELL_UNKNOWN; }
    result = (*env)->CallIntMethod(env, owner->peer, owner->tap_method, input);
    (*env)->DeleteLocalRef(env, input);
    if (ti_clear_java_exception(env)) result = TI_ANDROID_SHELL_UNKNOWN;
    ti_end_ticket(owner);
    return (int32_t)result;
}
static int32_t ti_key(void *user, int32_t display_id,
                      uint64_t display_generation, uint64_t geometry_generation,
                      uint64_t action_id, uint64_t attempt_id, int32_t kind) {
    ti_native_owner *owner = (ti_native_owner *)user;
    JNIEnv *env;
    jlong values[TI_JNI_BACKEND_KEY_FIELDS];
    jlongArray input;
    jint result;
    if (kind != TI_ANDROID_SHELL_KEY_BACK ||
        !ti_begin_ticket(owner, TI_ACTION_SYSTEM_BACK, display_id,
                         display_generation, geometry_generation,
                         action_id, attempt_id)) return TI_ANDROID_SHELL_UNKNOWN;
    env = ti_current_env(owner);
    if (!env) { ti_end_ticket(owner); return TI_ANDROID_SHELL_UNKNOWN; }
    values[0] = (jlong)display_id;
    values[1] = (jlong)display_generation;
    values[2] = (jlong)geometry_generation;
    values[3] = (jlong)action_id;
    values[4] = (jlong)attempt_id;
    values[5] = (jlong)kind;
    input = ti_java_longs(env, values, TI_JNI_BACKEND_KEY_FIELDS);
    if (!input) { ti_end_ticket(owner); return TI_ANDROID_SHELL_UNKNOWN; }
    result = (*env)->CallIntMethod(env, owner->peer, owner->key_method, input);
    (*env)->DeleteLocalRef(env, input);
    if (ti_clear_java_exception(env)) result = TI_ANDROID_SHELL_UNKNOWN;
    ti_end_ticket(owner);
    return (int32_t)result;
}

JNIEXPORT jlong JNICALL
Java_io_turbointeract_android_NativeActionJni_open(
    JNIEnv *env, jobject thiz, jobject peer,
    jint provider_id, jint capabilities, jlong plan_generation,
    jlong device_session_id, jlong session_generation, jint display_id,
    jlong display_generation, jlong geometry_generation, jint attempt_capacity) {
    ti_native_owner *owner;
    ti_action_owner_config_v1 cfg = TI_ACTION_OWNER_CONFIG_V1_INIT;
    jclass cls;
    ti_status status;
    (void)thiz;
    if (!peer || provider_id <= 0 || capabilities <= 0 ||
        plan_generation <= 0 || device_session_id <= 0 ||
        session_generation <= 0 || display_id <= 0 ||
        display_generation <= 0 || geometry_generation <= 0 ||
        attempt_capacity <= 0 || attempt_capacity > TI_ACTION_OWNER_MAX_ATTEMPTS)
        return (jlong)0;
    owner = (ti_native_owner *)calloc(1u, sizeof(*owner));
    if (!owner) return (jlong)0;
    atomic_flag_clear(&owner->guard);
    if ((*env)->GetJavaVM(env, &owner->vm) != JNI_OK) goto error;
    owner->peer = (*env)->NewGlobalRef(env, peer);
    if (!owner->peer || ti_clear_java_exception(env)) goto error;
    cls = (*env)->GetObjectClass(env, peer);
    if (!cls || ti_clear_java_exception(env)) goto error;
    owner->grant_method = (*env)->GetMethodID(env, cls, "grant", "([J)Z");
    owner->geometry_method = (*env)->GetMethodID(env, cls, "geometry", "([J)J");
    owner->tap_method = (*env)->GetMethodID(env, cls, "tap", "([J)I");
    owner->key_method = (*env)->GetMethodID(env, cls, "key", "([J)I");
    (*env)->DeleteLocalRef(env, cls);
    if (ti_clear_java_exception(env) || !owner->grant_method ||
        !owner->geometry_method || !owner->tap_method || !owner->key_method)
        goto error;
    owner->backend = (ti_android_shell_backend_v1)TI_ANDROID_SHELL_BACKEND_V1_INIT;
    owner->backend.device_session_id = (uint64_t)device_session_id;
    owner->backend.session_generation = (uint64_t)session_generation;
    owner->backend.display_id = (uint32_t)display_id;
    owner->backend.display_generation = (uint64_t)display_generation;
    owner->backend.geometry = ti_geometry;
    owner->backend.tap = ti_tap;
    owner->backend.key = ti_key;
    owner->backend.user = owner;

    owner->provider_id = (uint32_t)provider_id;
    cfg.provider_id = (uint32_t)provider_id;
    cfg.capabilities = (uint32_t)capabilities;
    cfg.plan_generation = (uint64_t)plan_generation;
    cfg.display.device_session_id = (uint64_t)device_session_id;
    cfg.display.session_generation = (uint64_t)session_generation;
    cfg.display.display_id = (uint32_t)display_id;
    cfg.display.display_generation = (uint64_t)display_generation;
    cfg.display.geometry_generation = (uint64_t)geometry_generation;
    cfg.attempt_capacity = (uint32_t)attempt_capacity;
    cfg.grant = ti_grant;
    cfg.dispatch = ti_android_shell_backend_dispatch;
    cfg.user = owner;
    status = ti_action_owner_create(&cfg, &owner->owner);
    if (status != TI_OK) goto error;
    return (jlong)(uintptr_t)owner;
error:
    if (owner->peer) (*env)->DeleteGlobalRef(env, owner->peer);
    free(owner);
    return (jlong)0;
}

static jintArray ti_jni_result(JNIEnv *env, ti_status status,
                                const ti_action_receipt_v1 *receipt) {
    jint cells[3];
    jintArray result = (*env)->NewIntArray(env, 3);
    if (!result) return NULL;
    cells[0] = (jint)status;
    cells[1] = status == TI_OK ? (jint)receipt->outcome : (jint)0;
    cells[2] = status == TI_OK ? (jint)receipt->level : (jint)0;
    (*env)->SetIntArrayRegion(env, result, 0, 3, cells);
    return result;
}

JNIEXPORT jintArray JNICALL
Java_io_turbointeract_android_NativeActionJni_submit(
    JNIEnv *env, jobject thiz, jlong handle, jlongArray encoded) {
    ti_native_owner *owner = ti_owner(handle);
    ti_action_request_v1 request = TI_ACTION_REQUEST_V1_INIT;
    ti_action_receipt_v1 receipt = TI_ACTION_RECEIPT_V1_INIT;
    jlong v[TI_JNI_FIELDS];
    ti_status status;
    int i;
    (void)thiz;
    if (!owner || !encoded || (*env)->GetArrayLength(env, encoded) != TI_JNI_FIELDS)
        return ti_jni_result(env, TI_EINVAL, &receipt);
    (*env)->GetLongArrayRegion(env, encoded, 0, TI_JNI_FIELDS, v);
    if (ti_clear_java_exception(env)) return ti_jni_result(env, TI_EINVAL, &receipt);
    /* JNI is an internal interface, still reject unbounded signed values
     * before any jlong->uint64 conversion and reserve all callback identities. */
    if (v[TI_JNI_VERSION] != 1 || v[TI_JNI_ORIGIN] < 1 ||
        v[TI_JNI_ORIGIN] > 3 ||
        (v[TI_JNI_KIND] != TI_ACTION_GESTURE_TAP &&
         v[TI_JNI_KIND] != TI_ACTION_SYSTEM_BACK) ||
        v[TI_JNI_ACTION] <= 0 || v[TI_JNI_ATTEMPT] <= 0 ||
        v[TI_JNI_DEVICE] <= 0 || v[TI_JNI_SESSION] <= 0 ||
        v[TI_JNI_DISPLAY] <= 0 || v[TI_JNI_DISPLAY] > INT32_MAX ||
        v[TI_JNI_DISPLAY_GEN] <= 0 || v[TI_JNI_GEOM_GEN] <= 0 ||
        v[TI_JNI_PLAN] <= 0 || v[TI_JNI_DEADLINE] <= 0 ||
        v[TI_JNI_X] < 0 || v[TI_JNI_X] > 1000000 ||
        v[TI_JNI_Y] < 0 || v[TI_JNI_Y] > 1000000 ||
        v[TI_JNI_NOW] <= 0) return ti_jni_result(env, TI_EINVAL, &receipt);
    for (i = TI_JNI_EVIDENCE; i <= TI_JNI_CAPTURE; i++) {
        if (v[i] < 0) return ti_jni_result(env, TI_EINVAL, &receipt);
    }
    request.origin = (uint32_t)v[TI_JNI_ORIGIN];
    request.display.device_session_id = (uint64_t)v[TI_JNI_DEVICE];
    request.display.session_generation = (uint64_t)v[TI_JNI_SESSION];
    request.display.display_id = (uint32_t)v[TI_JNI_DISPLAY];
    request.display.display_generation = (uint64_t)v[TI_JNI_DISPLAY_GEN];
    request.display.geometry_generation = (uint64_t)v[TI_JNI_GEOM_GEN];
    request.intent.kind = (uint32_t)v[TI_JNI_KIND];
    /* Provider identity is immutable and trusted from nativeOpen. */
    request.intent.provider_id = owner->provider_id;
    request.intent.action_id = (uint64_t)v[TI_JNI_ACTION];
    request.intent.attempt_id = (uint64_t)v[TI_JNI_ATTEMPT];
    request.intent.evidence_id = (uint64_t)v[TI_JNI_EVIDENCE];
    request.intent.plan_generation = (uint64_t)v[TI_JNI_PLAN];
    request.intent.session_generation = (uint64_t)v[TI_JNI_SESSION];
    request.intent.display_generation = (uint64_t)v[TI_JNI_DISPLAY_GEN];
    request.intent.deadline_ns = (uint64_t)v[TI_JNI_DEADLINE];
    request.intent.x_normalized = (uint32_t)v[TI_JNI_X];
    request.intent.y_normalized = (uint32_t)v[TI_JNI_Y];
    request.source_observation_id = (uint64_t)v[TI_JNI_EVIDENCE];
    request.source_capture_sequence = (uint64_t)v[TI_JNI_CAPTURE];

    status = ti_action_owner_submit(owner->owner, &request,
                                     (uint64_t)v[TI_JNI_NOW], &receipt);
    return ti_jni_result(env, status, &receipt);
}

JNIEXPORT jboolean JNICALL
Java_io_turbointeract_android_NativeActionJni_confirm(
    JNIEnv *env, jobject thiz, jlong handle, jint kind,
    jlong device_session_id, jlong session_generation, jint display_id,
    jlong display_generation, jlong geometry_generation,
    jlong action_id, jlong attempt_id) {
    ti_native_owner *owner = ti_owner(handle);
    const ti_action_request_v1 *r;
    int accepted;
    (void)env; (void)thiz;
    if (!owner || device_session_id <= 0 || session_generation <= 0 ||
        display_id <= 0 || display_generation <= 0 || geometry_generation <= 0 ||
        action_id <= 0 || attempt_id <= 0 ||
        (kind != TI_ACTION_GESTURE_TAP && kind != TI_ACTION_SYSTEM_BACK))
        return JNI_FALSE;
    ti_guard(owner);
    r = &owner->proposal;
    accepted = !owner->revoked && owner->pending && !owner->consumed &&
        r->intent.kind == (uint32_t)kind &&
        r->display.device_session_id == (uint64_t)device_session_id &&
        r->display.session_generation == (uint64_t)session_generation &&
        r->display.display_id == (uint32_t)display_id &&
        r->display.display_generation == (uint64_t)display_generation &&
        r->display.geometry_generation == (uint64_t)geometry_generation &&
        r->intent.action_id == (uint64_t)action_id &&
        r->intent.attempt_id == (uint64_t)attempt_id;
    if (accepted) owner->consumed = 1;
    ti_unguard(owner);
    return accepted ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_io_turbointeract_android_NativeActionJni_updateGeometry(
    JNIEnv *env, jobject thiz, jlong handle, jlong generation) {
    ti_native_owner *owner = ti_owner(handle);
    (void)env; (void)thiz;
    if (!owner || generation <= 0) return (jint)TI_EINVAL;
    return (jint)ti_action_owner_update_geometry(owner->owner,
                                                  (uint64_t)generation);
}

JNIEXPORT jint JNICALL
Java_io_turbointeract_android_NativeActionJni_revoke(
    JNIEnv *env, jobject thiz, jlong handle) {
    ti_native_owner *owner = ti_owner(handle);
    (void)env; (void)thiz;
    if (!owner) return (jint)TI_EINVAL;
    ti_guard(owner);
    owner->revoked = 1;
    owner->pending = 0;
    owner->proposed = 0;
    ti_unguard(owner);
    return (jint)ti_action_owner_revoke(owner->owner);
}

JNIEXPORT jint JNICALL
Java_io_turbointeract_android_NativeActionJni_destroy(
    JNIEnv *env, jobject thiz, jlong handle) {
    ti_native_owner *owner = ti_owner(handle);
    ti_status status;
    (void)thiz;
    if (!owner) return (jint)TI_EINVAL;
    status = ti_action_owner_destroy(&owner->owner);
    if (status != TI_OK) return (jint)status;
    (*env)->DeleteGlobalRef(env, owner->peer);
    owner->peer = NULL;
    memset(&owner->proposal, 0, sizeof(owner->proposal));
    free(owner);
    return (jint)TI_OK;
}
