#if defined(__APPLE__)
/* Darwin extension APIs expose LOCAL_PEERCRED/xucred and real mkdtemp. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#elif !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "turbo_interact/sandbox_ipc.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

typedef struct fixture {
    ti_action_owner *owner;
    ti_sandbox_ipc *ipc;
    unsigned grants;
    unsigned effects;
    unsigned permit;
} fixture;

static ti_status grant(void *user, const ti_action_request_v1 *request) {
    fixture *f = (fixture *)user;
    f->grants++;
    assert(request->origin == TI_ACTION_ORIGIN_SANDBOX ||
           request->origin == TI_ACTION_ORIGIN_MODEL);
    /* Only the sandbox needs its additional live grant; an independently
     * admitted model call must still work after the sandbox bridge closes. */
    return request->origin == TI_ACTION_ORIGIN_MODEL || f->permit ?
        TI_OK : TI_EAUTH;
}

static ti_status dispatch(void *user, const ti_action_request_v1 *req,
                          ti_action_receipt_v1 *receipt) {
    fixture *f = (fixture *)user;
    ti_action_receipt_v1 r = TI_ACTION_RECEIPT_V1_INIT;
    f->effects++;
    r.action_id = req->intent.action_id;
    r.attempt_id = req->intent.attempt_id;
    r.plan_generation = req->intent.plan_generation;
    r.session_generation = req->intent.session_generation;
    r.level = TI_RECEIPT_BACKEND_TERMINAL;
    r.outcome = TI_ACTION_COMPLETED;
    *receipt = r;
    return TI_OK;
}

static void owner_create(fixture *f) {
    ti_action_owner_config_v1 o = TI_ACTION_OWNER_CONFIG_V1_INIT;
    o.provider_id = 17u;
    o.capabilities = TI_ACTION_CAP_GESTURE | TI_ACTION_CAP_SYSTEM;
    o.attempt_capacity = 12u;
    o.plan_generation = 11u;
    o.display.device_session_id = 5u;
    o.display.session_generation = 9u;
    o.display.display_id = 7u;
    o.display.display_generation = 3u;
    o.display.geometry_generation = 1u;
    o.grant = grant;
    o.dispatch = dispatch;
    o.user = f;
    assert(ti_action_owner_create(&o, &f->owner) == TI_OK);
}

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>

static void ipc_open_with_uid(fixture *f, const char *private_dir, uint32_t peer_uid) {
    ti_sandbox_ipc_config_v1 c = TI_SANDBOX_IPC_CONFIG_V1_INIT;
    c.peer_uid = peer_uid;
    c.provider_id = 17u;
    c.allowed_tools = TI_SANDBOX_PHONE_TAP | TI_SANDBOX_PHONE_BACK;
    c.task_id = 100u;
    c.sandbox_run_id = 200u;
    c.plan_generation = 11u;
    c.display.device_session_id = 5u;
    c.display.session_generation = 9u;
    c.display.display_id = 7u;
    c.display.display_generation = 3u;
    c.display.geometry_generation = 1u;
    c.action_owner = f->owner;
    assert(ti_sandbox_ipc_open(&c, private_dir, &f->ipc) == TI_OK);
}

static void ipc_open(fixture *f, const char *private_dir) {
    ipc_open_with_uid(f, private_dir, (uint32_t)geteuid());
}

static int client_connect(const char *path) {
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    assert(strlen(path) < sizeof(addr.sun_path));
    memcpy(addr.sun_path, path, strlen(path) + 1u);
    assert(connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0);
    return fd;
}

static void send_str(int fd, const char *body) {
    size_t off = 0u;
    size_t len = strlen(body);
    while (off < len) {
        ssize_t n = send(fd, body + off, len - off, 0);
        assert(n > 0);
        off += (size_t)n;
    }
}

static ti_status transaction(ti_sandbox_ipc *ipc, const char *path,
                             const char *json, ti_status expected) {
    int fd = client_connect(path);
    char reply[180];
    ssize_t read_n;
    ti_status st;
    send_str(fd, json);
    st = ti_sandbox_ipc_serve_one(ipc, 1000000000u, 1000u);
    assert(st == expected);
    read_n = recv(fd, reply, sizeof(reply) - 1u, 0);
    assert(read_n > 0);
    reply[read_n] = '\0';
    assert(strstr(reply, expected == TI_OK ? "\"ok\":true" : "\"ok\":false") != NULL);
    assert(strstr(reply, "\"status\":") != NULL);
    close(fd);
    return st;
}

static void tap_json(char *out, size_t out_size, const char *token,
                     unsigned long long callid, unsigned long long action,
                     unsigned long long attempt, unsigned long long geometry) {
    int n = snprintf(out, out_size,
        "{\"token\":\"%s\",\"tool\":\"tap\",\"callId\":%llu,"
        "\"actionId\":%llu,\"attemptId\":%llu,\"observationId\":23,"
        "\"captureSequence\":5,\"displayGeneration\":3,"
        "\"geometryGeneration\":%llu,\"deadlineNs\":2000000000,"
        "\"x\":100000,\"y\":900000}\n",
        token, callid, action, attempt, geometry);
    assert(n > 0 && (size_t)n < out_size);
}

static void back_json(char *out, size_t out_size, const char *token,
                      unsigned long long callid, unsigned long long action) {
    int n = snprintf(out, out_size,
        "{\"token\":\"%s\",\"tool\":\"back\",\"callId\":%llu,"
        "\"actionId\":%llu,\"attemptId\":%llu,\"observationId\":0,"
        "\"captureSequence\":0,\"displayGeneration\":3,"
        "\"geometryGeneration\":1,\"deadlineNs\":2000000000}\n",
        token, callid, action, callid);
    assert(n > 0 && (size_t)n < out_size);
}

static void test_real_unix_socket_and_authorization(void) {
    fixture f = {0};
    char dir[] = "/tmp/ti-bridge-XXXXXX";
    char path[TI_SANDBOX_IPC_PATH_CAP];
    char token[TI_SANDBOX_IPC_TOKEN_HEX_CAP];
    char body[1024], wrong[65], rejected[2048];
    ti_action_request_v1 direct = TI_ACTION_REQUEST_V1_INIT;
    ti_action_receipt_v1 receipt = TI_ACTION_RECEIPT_V1_INIT;
    assert(mkdtemp(dir) != NULL);
    assert(chmod(dir, 0700) == 0);
    owner_create(&f);
    ipc_open(&f, dir);
    assert(ti_sandbox_ipc_credentials(f.ipc, path, sizeof(path),
        token, sizeof(token)) == TI_OK);
    assert(strlen(token) == 64u);
    assert(strlen(path) > 0u);
    assert(ti_sandbox_ipc_serve_one(f.ipc, 1000000000u, 10u) == TI_EEXPIRED);
    {
        /* A client that sends one byte and never finishes a line cannot
         * reset the overall RPC deadline by dribbling input. */
        int slow_fd = client_connect(path);
        send_str(slow_fd, "{");
        assert(ti_sandbox_ipc_serve_one(f.ipc, 1000000000u, 50u) == TI_EEXPIRED);
        close(slow_fd);
        assert(f.grants == 0u && f.effects == 0u);
    }

    /* Peer OS credentials come from SO_PEERCRED/getpeereid; a JSON UID is
     * unknown and rejected rather than treated as an identity claim. */
    tap_json(body, sizeof(body), token, 1u, 10u, 1u, 1u);
    (void)snprintf(rejected, sizeof(rejected), "%s", body);
    rejected[strlen(rejected) - 2u] = '\0'; /* Ends at closing brace. */
    strcat(rejected, ",\"uid\":2000}\n");
    transaction(f.ipc, path, rejected, TI_EINVAL);
    assert(f.grants == 0u);

    strcpy(wrong, token);
    wrong[0] = wrong[0] == 'a' ? 'b' : 'a';
    tap_json(body, sizeof(body), wrong, 1u, 10u, 1u, 1u);
    transaction(f.ipc, path, body, TI_EAUTH);
    assert(f.grants == 0u && f.effects == 0u);

    /* Correct Unix peer and token are necessary but NOT host authorization. */
    tap_json(body, sizeof(body), token, 1u, 10u, 1u, 1u);
    transaction(f.ipc, path, body, TI_EAUTH);
    assert(f.grants == 1u && f.effects == 0u);
    f.permit = 1u;

    tap_json(body, sizeof(body), token, 2u, 10u, 2u, 1u);
    transaction(f.ipc, path, body, TI_OK);
    assert(f.effects == 1u);
    transaction(f.ipc, path, body, TI_ECONFLICT);

    /* Replaying across a different call ID still hits the same ActionOwner. */
    tap_json(body, sizeof(body), token, 3u, 10u, 2u, 1u);
    transaction(f.ipc, path, body, TI_ECONFLICT);
    assert(f.effects == 1u);

    tap_json(body, sizeof(body), token, 4u, 11u, 1u, 44u);
    transaction(f.ipc, path, body, TI_ESTALE);
    assert(f.effects == 1u);

    back_json(body, sizeof(body), token, 5u, 12u);
    transaction(f.ipc, path, body, TI_OK);
    assert(f.effects == 2u);

    /* A Sandbox close cannot revoke a separately authorized model call. */
    assert(ti_sandbox_ipc_close(f.ipc) == TI_OK);
    assert(access(path, F_OK) != 0);
    assert(ti_sandbox_ipc_credentials(f.ipc, path, sizeof(path), token, sizeof(token)) == TI_EAUTH);
    assert(ti_sandbox_ipc_destroy(&f.ipc) == TI_OK);
    assert(f.ipc == NULL);

    direct.origin = TI_ACTION_ORIGIN_MODEL;
    direct.display.device_session_id = 5u;
    direct.display.session_generation = 9u;
    direct.display.display_id = 7u;
    direct.display.display_generation = 3u;
    direct.display.geometry_generation = 1u;
    direct.intent.kind = TI_ACTION_SYSTEM_BACK;
    direct.intent.provider_id = 17u;
    direct.intent.action_id = 20u;
    direct.intent.attempt_id = 1u;
    direct.intent.plan_generation = 11u;
    direct.intent.session_generation = 9u;
    direct.intent.display_generation = 3u;
    direct.intent.deadline_ns = 2000000000u;
    assert(ti_action_owner_submit(f.owner, &direct, 1000000000u, &receipt) == TI_OK);
    assert(f.effects == 3u);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
    assert(rmdir(dir) == 0);
}

static void test_wrong_actual_kernel_peer_is_rejected(void) {
    fixture f = {0};
    char dir[] = "/tmp/ti-uid-XXXXXX";
    char path[TI_SANDBOX_IPC_PATH_CAP];
    char token[TI_SANDBOX_IPC_TOKEN_HEX_CAP];
    char body[1024];
    uint32_t mismatched_uid = (uint32_t)geteuid() + 1u;
    assert(mkdtemp(dir) != NULL);
    assert(chmod(dir, 0700) == 0);
    assert(mismatched_uid != (uint32_t)geteuid());
    owner_create(&f);
    /* The OS-reported UID is the real process UID. The host-pinned expected
     * value must NEVER be read from a JSON field or the model request. */
    ipc_open_with_uid(&f, dir, mismatched_uid);
    assert(ti_sandbox_ipc_credentials(f.ipc, path, sizeof(path),
        token, sizeof(token)) == TI_OK);
    f.permit = 1u; /* Even an allow-all test host cannot bypass the OS UID. */
    tap_json(body, sizeof(body), token, 1u, 5u, 1u, 1u);
    transaction(f.ipc, path, body, TI_EAUTH);
    assert(f.grants == 0u && f.effects == 0u);
    assert(ti_sandbox_ipc_destroy(&f.ipc) == TI_OK);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
    assert(rmdir(dir) == 0);
}

static void test_directory_must_be_private(void) {
    fixture f = {0};
    ti_sandbox_ipc_config_v1 c = TI_SANDBOX_IPC_CONFIG_V1_INIT;
    char tmp[] = "/tmp";
    owner_create(&f);
    c.peer_uid = (uint32_t)geteuid();
    c.provider_id = 17u;
    c.allowed_tools = TI_SANDBOX_PHONE_TAP;
    c.plan_generation = 11u;
    c.task_id = 1u;
    c.sandbox_run_id = 2u;
    c.display.device_session_id = 5u;
    c.display.session_generation = 9u;
    c.display.display_id = 7u;
    c.display.display_generation = 3u;
    c.display.geometry_generation = 1u;
    c.action_owner = f.owner;
    assert(ti_sandbox_ipc_open(&c, tmp, &f.ipc) == TI_EAUTH);
    assert(f.ipc == NULL);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
}
#else
static void test_windows_has_explicit_unsupported_local_socket(void) {
    fixture f = {0};
    ti_sandbox_ipc_config_v1 c = TI_SANDBOX_IPC_CONFIG_V1_INIT;
    owner_create(&f);
    c.peer_uid = 1234u;
    c.provider_id = 17u;
    c.allowed_tools = TI_SANDBOX_PHONE_TAP;
    c.plan_generation = 11u;
    c.task_id = 1u;
    c.sandbox_run_id = 2u;
    c.display.device_session_id = 5u;
    c.display.session_generation = 9u;
    c.display.display_id = 7u;
    c.display.display_generation = 3u;
    c.display.geometry_generation = 1u;
    c.action_owner = f.owner;
    assert(ti_sandbox_ipc_open(&c, "C:/private", &f.ipc) == TI_EUNSUPPORTED);
    assert(f.ipc == NULL);
    assert(ti_action_owner_destroy(&f.owner) == TI_OK);
}
#endif

int main(void) {
#if !defined(_WIN32)
    test_real_unix_socket_and_authorization();
    test_wrong_actual_kernel_peer_is_rejected();
    test_directory_must_be_private();
#else
    test_windows_has_explicit_unsupported_local_socket();
#endif
    puts("sandbox socket: OS peer UID, token, scope and ActionOwner conformance passed");
    return 0;
}
