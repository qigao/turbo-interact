#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__)
/* Darwin extension APIs expose LOCAL_PEERCRED/xucred and real mkdtemp. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#elif !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "turbo_interact/sandbox_ipc.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#if defined(__APPLE__)
#include <sys/ucred.h>
#endif
#include <poll.h>
#include <time.h>
#include <unistd.h>
#endif

struct ti_sandbox_ipc {
    atomic_flag guard;
    ti_sandbox_ipc_config_v1 config;
    ti_sandbox_bridge *bridge;
    uint8_t token[TI_SANDBOX_TOKEN_BYTES];
    char token_hex[TI_SANDBOX_IPC_TOKEN_HEX_CAP];
    char path[TI_SANDBOX_IPC_PATH_CAP];
    uint32_t closed;
    uint32_t busy;
#if !defined(_WIN32)
    int server_fd;
    dev_t socket_device;
    ino_t socket_inode;
#endif
};

static void ti_ipc_lock(ti_sandbox_ipc *s) {
    while (atomic_flag_test_and_set_explicit(&s->guard, memory_order_acquire)) {}
}
static void ti_ipc_unlock(ti_sandbox_ipc *s) {
    atomic_flag_clear_explicit(&s->guard, memory_order_release);
}

static ti_status ti_ipc_config_check(const ti_sandbox_ipc_config_v1 *c) {
    ti_status st;
    if (!c || c->size != (uint32_t)sizeof(*c)) return TI_EINVAL;
    if (c->version != TI_ABI_VERSION) return TI_EVERSION;
    st = ti_display_ref_validate(&c->display);
    if (st != TI_OK) return st;
    if (!c->peer_uid || !c->provider_id || !c->plan_generation ||
        !c->task_id || !c->sandbox_run_id || !c->action_owner ||
        !c->allowed_tools ||
        (c->allowed_tools & ~(TI_SANDBOX_PHONE_TAP | TI_SANDBOX_PHONE_BACK)) ||
        c->reserved) return TI_EINVAL;
    return TI_OK;
}

static void ti_ipc_wipe(void *ptr, size_t count) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (count--) *p++ = 0;
}

#if !defined(_WIN32)
static int ti_ipc_random(uint8_t *out, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t off = 0u;
    if (fd < 0) return 0; /* No weak PRNG fallback. */
    while (off < len) {
        ssize_t n = read(fd, out + off, len - off);
        if (n > 0) off += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { close(fd); return 0; }
    }
    close(fd);
    return 1;
}

static void ti_ipc_hex(const uint8_t *bytes, size_t len, char *out) {
    static const char alphabet[] = "0123456789abcdef";
    size_t i;
    for (i = 0u; i < len; i++) {
        out[2u * i] = alphabet[bytes[i] >> 4];
        out[2u * i + 1u] = alphabet[bytes[i] & 15u];
    }
    out[2u * len] = '\0';
}

static int ti_ipc_private_directory(const char *directory) {
    struct stat statbuf;
    if (!directory || directory[0] != '/' ||
        strlen(directory) > 63u || strstr(directory, "/../") ||
        strstr(directory, "/./") ||
        (strlen(directory) >= 3u &&
         strcmp(directory + strlen(directory) - 3u, "/..") == 0)) return 0;
    if (lstat(directory, &statbuf) != 0 || !S_ISDIR(statbuf.st_mode)) return 0;
    return statbuf.st_uid == geteuid() && (statbuf.st_mode & 0077) == 0;
}

static int ti_ipc_socket_owned(const ti_sandbox_ipc *session) {
    struct stat s;
    return session->path[0] && lstat(session->path, &s) == 0 &&
        S_ISSOCK(s.st_mode) &&
        s.st_dev == session->socket_device && s.st_ino == session->socket_inode;
}

static void ti_ipc_close_server(ti_sandbox_ipc *session) {
    if (session->server_fd >= 0) {
        close(session->server_fd);
        session->server_fd = -1;
    }
    if (ti_ipc_socket_owned(session)) unlink(session->path);
}

static ti_status ti_ipc_listen(ti_sandbox_ipc *s, const char *directory) {
    uint8_t random_suffix[16];
    char suffix[33];
    struct sockaddr_un addr;
    struct stat st;
    size_t len;
    if (!ti_ipc_private_directory(directory)) return TI_EAUTH;
    if (!ti_ipc_random(s->token, sizeof(s->token)) ||
        !ti_ipc_random(random_suffix, sizeof(random_suffix))) return TI_EUNSUPPORTED;
    ti_ipc_hex(s->token, sizeof(s->token), s->token_hex);
    ti_ipc_hex(random_suffix, sizeof(random_suffix), suffix);
    ti_ipc_wipe(random_suffix, sizeof(random_suffix));
    if (snprintf(s->path, sizeof(s->path), "%s/ti-%s.sock",
                 directory, suffix) >= (int)sizeof(addr.sun_path)) return TI_ERANGE;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    len = strlen(s->path);
    memcpy(addr.sun_path, s->path, len + 1u);
    s->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->server_fd < 0) return TI_EUNSUPPORTED;
    if (fcntl(s->server_fd, F_SETFD, FD_CLOEXEC) != 0 ||
        bind(s->server_fd, (struct sockaddr *)&addr,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len + 1u)) != 0) {
        close(s->server_fd);
        s->server_fd = -1;
        return TI_ECONFLICT;
    }
    if (chmod(s->path, 0600) != 0 || lstat(s->path, &st) != 0) {
        /* It was created in the already-private host directory. */
        unlink(s->path);
        close(s->server_fd);
        s->server_fd = -1;
        return TI_EAUTH;
    }
    s->socket_device = st.st_dev;
    s->socket_inode = st.st_ino;
    if (!S_ISSOCK(st.st_mode) || listen(s->server_fd, 4) != 0) {
        ti_ipc_close_server(s);
        return TI_EUNSUPPORTED;
    }
    return TI_OK;
}

static int ti_ipc_peer_uid(int fd, uint32_t *uid) {
#if defined(__linux__) || defined(__ANDROID__)
    struct ucred cred;
    socklen_t len = (socklen_t)sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
        len != sizeof(cred) || cred.uid == (uid_t)-1) return 0;
    *uid = (uint32_t)cred.uid;
    return 1;
#elif defined(__APPLE__)
    /* macOS has no getpeereid() with this SDK. Use the kernel-provided
     * LOCAL_PEERCRED struct xucred; reject unknown credential versions. */
    struct xucred cred;
    socklen_t len = (socklen_t)sizeof(cred);
    if (getsockopt(fd, 0, LOCAL_PEERCRED, &cred, &len) != 0 ||
        len != sizeof(cred) || cred.cr_version != XUCRED_VERSION ||
        cred.cr_uid == (uid_t)-1) return 0;
    *uid = (uint32_t)cred.cr_uid;
    return 1;
#else
    (void)fd; (void)uid;
    return 0;
#endif
}

/* One absolute monotonic deadline for accept + all read/write events.
 * Per-recv SO_RCVTIMEO alone is insufficient against slow trickle traffic. */
static uint64_t ti_ipc_time_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0) return 0u;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int ti_ipc_wait_ready(int fd, short events, uint64_t end_ns) {
    for (;;) {
        struct pollfd pfd;
        uint64_t now = ti_ipc_time_ns();
        int wait_ms;
        int n;
        if (!now || now >= end_ns) return 0;
        wait_ms = (int)((end_ns - now + 999999u) / 1000000u);
        pfd.fd = fd;
        pfd.events = events;
        pfd.revents = 0;
        n = poll(&pfd, 1, wait_ms);
        if (n > 0) return (pfd.revents & events) != 0;
        if (n == 0) return 0;
        if (errno != EINTR) return 0;
    }
}

static int ti_ipc_recv_line(int fd, char *out, size_t max,
                            uint64_t end_ns) {
    size_t count = 0u;
    for (;;) {
        char chunk[512];
        ssize_t n;
        size_t i;
        if (!ti_ipc_wait_ready(fd, POLLIN, end_ns)) return 0;
        n = recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) return 0; /* Partial / newline-less is not a request. */
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return 0;
        }
        for (i = 0u; i < (size_t)n; i++) {
            if (chunk[i] == '\n') {
                /* Exactly one request per socket. No pipelining. */
                if (i + 1u != (size_t)n || count == 0u) return 0;
                out[count] = '\0';
                return 1;
            }
            if (count >= max || chunk[i] == '\0') return 0;
            out[count++] = chunk[i];
        }
    }
}

/* Strict schema-specific JSON, not a general-purpose second JSON engine.
 * All recognized strings/keys are ASCII without escapes, integers decimal
 * uint64, unknown or duplicated fields fail closed, nesting is rejected. */
enum ti_ipc_field {
    TI_F_CALL, TI_F_ACTION, TI_F_ATTEMPT, TI_F_OBSERVATION,
    TI_F_CAPTURE, TI_F_DISPLAY, TI_F_GEOMETRY, TI_F_DEADLINE,
    TI_F_X, TI_F_Y, TI_F_TOKEN, TI_F_TOOL, TI_F_COUNT
};
static const char *ti_ipc_field_names[TI_F_COUNT] = {
    "callId", "actionId", "attemptId", "observationId", "captureSequence",
    "displayGeneration", "geometryGeneration", "deadlineNs",
    "x", "y", "token", "tool"
};
typedef struct ti_ipc_values {
    uint64_t n[TI_F_TOKEN];
    uint32_t found;
    char token[TI_SANDBOX_IPC_TOKEN_HEX_CAP];
    char tool[8];
} ti_ipc_values;

static void ti_ipc_space(const char **p) {
    while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t') ++*p;
}
static int ti_ipc_string(const char **p, char *dest, size_t cap) {
    size_t n = 0u;
    if (**p != '"') return 0;
    ++*p;
    while (**p && **p != '"') {
        unsigned char c = (unsigned char)**p;
        if (c < 0x20u || c >= 0x7fu || c == '\\' ||
            n + 1u >= cap) return 0;
        dest[n++] = **p;
        ++*p;
    }
    if (**p != '"') return 0;
    ++*p;
    dest[n] = '\0';
    return 1;
}
static int ti_ipc_unsigned(const char **p, uint64_t *out) {
    uint64_t n = 0u;
    const char *start = *p;
    if (**p < '0' || **p > '9') return 0;
    if (**p == '0' && (*p)[1] >= '0' && (*p)[1] <= '9') return 0;
    while (**p >= '0' && **p <= '9') {
        uint32_t d = (uint32_t)(**p - '0');
        if (n > (UINT64_MAX - d) / 10u) return 0;
        n = n * 10u + d;
        ++*p;
    }
    if (*p == start) return 0;
    *out = n;
    return 1;
}
static int ti_ipc_parse(const char *line, ti_ipc_values *v) {
    const char *p = line;
    int first = 1;
    memset(v, 0, sizeof(*v));
    ti_ipc_space(&p);
    if (*p++ != '{') return 0;
    for (;;) {
        char key[32];
        int index;
        ti_ipc_space(&p);
        if (*p == '}') { ++p; break; }
        if (!first) {
            if (*p++ != ',') return 0;
            ti_ipc_space(&p);
        }
        first = 0;
        if (!ti_ipc_string(&p, key, sizeof(key))) return 0;
        for (index = 0; index < TI_F_COUNT; index++) {
            if (strcmp(ti_ipc_field_names[index], key) == 0) break;
        }
        if (index == TI_F_COUNT || (v->found & (1u << index))) return 0;
        ti_ipc_space(&p);
        if (*p++ != ':') return 0;
        ti_ipc_space(&p);
        if (index == TI_F_TOKEN) {
            if (!ti_ipc_string(&p, v->token, sizeof(v->token))) return 0;
        } else if (index == TI_F_TOOL) {
            if (!ti_ipc_string(&p, v->tool, sizeof(v->tool))) return 0;
        } else if (!ti_ipc_unsigned(&p, &v->n[index])) return 0;
        v->found |= (1u << index);
    }
    ti_ipc_space(&p);
    if (*p != '\0') return 0;
    {
        uint32_t common = ((1u << TI_F_X) - 1u) |
                          (1u << TI_F_TOKEN) | (1u << TI_F_TOOL);
        if (strcmp(v->tool, "tap") == 0) {
            return v->found == (common | (1u << TI_F_X) | (1u << TI_F_Y));
        }
        if (strcmp(v->tool, "back") == 0) return v->found == common;
    }
    return 0;
}
static int ti_ipc_hex_decode(const char *text,
                             uint8_t out[TI_SANDBOX_TOKEN_BYTES]) {
    unsigned i;
    if (strlen(text) != 2u * TI_SANDBOX_TOKEN_BYTES) return 0;
    for (i = 0u; i < TI_SANDBOX_TOKEN_BYTES; i++) {
        unsigned j;
        unsigned result = 0u;
        for (j = 0u; j < 2u; j++) {
            unsigned char c = (unsigned char)text[2u * i + j];
            uint32_t v = c >= '0' && c <= '9' ? (uint32_t)(c - '0') :
                         c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10) : 16u;
            if (v == 16u) return 0;
            result = (result << 4) | v;
        }
        out[i] = (uint8_t)result;
    }
    return 1;
}

static ti_status ti_ipc_apply(ti_sandbox_ipc *s, const char *json,
                              uint32_t peer_uid, uint64_t now_ns,
                              ti_action_receipt_v1 *receipt) {
    ti_ipc_values values;
    uint8_t token[TI_SANDBOX_TOKEN_BYTES];
    ti_sandbox_phone_call_v1 call = TI_SANDBOX_PHONE_CALL_V1_INIT;
    ti_status status;
    if (!ti_ipc_parse(json, &values)) return TI_EINVAL;
    if (!ti_ipc_hex_decode(values.token, token)) return TI_EAUTH;
    if (now_ns > UINT64_MAX - TI_SANDBOX_IPC_MAX_DEADLINE_NS ||
        values.n[TI_F_DEADLINE] <= now_ns ||
        values.n[TI_F_DEADLINE] - now_ns > TI_SANDBOX_IPC_MAX_DEADLINE_NS) {
        ti_ipc_wipe(token, sizeof(token));
        return TI_EEXPIRED;
    }
    if (values.n[TI_F_X] > 1000000u || values.n[TI_F_Y] > 1000000u ||
        values.n[TI_F_DISPLAY] == 0u ||
        values.n[TI_F_GEOMETRY] == 0u ||
        values.n[TI_F_CALL] == 0u ||
        values.n[TI_F_ACTION] == 0u ||
        values.n[TI_F_ATTEMPT] == 0u) {
        ti_ipc_wipe(token, sizeof(token));
        return TI_ERANGE;
    }
    call.tool = strcmp(values.tool, "tap") == 0 ? TI_SANDBOX_TOOL_TAP : TI_SANDBOX_TOOL_BACK;
    call.task_id = s->config.task_id;
    call.sandbox_run_id = s->config.sandbox_run_id;
    call.call_id = values.n[TI_F_CALL];
    call.action.origin = TI_ACTION_ORIGIN_SANDBOX;
    call.action.display = s->config.display;
    call.action.display.display_generation = values.n[TI_F_DISPLAY];
    call.action.display.geometry_generation = values.n[TI_F_GEOMETRY];
    call.action.source_observation_id = values.n[TI_F_OBSERVATION];
    call.action.source_capture_sequence = values.n[TI_F_CAPTURE];
    call.action.intent.kind = call.tool == TI_SANDBOX_TOOL_TAP ?
        TI_ACTION_GESTURE_TAP : TI_ACTION_SYSTEM_BACK;
    call.action.intent.provider_id = s->config.provider_id;
    call.action.intent.action_id = values.n[TI_F_ACTION];
    call.action.intent.attempt_id = values.n[TI_F_ATTEMPT];
    call.action.intent.evidence_id = values.n[TI_F_OBSERVATION];
    call.action.intent.plan_generation = s->config.plan_generation;
    call.action.intent.session_generation = s->config.display.session_generation;
    call.action.intent.display_generation = values.n[TI_F_DISPLAY];
    call.action.intent.deadline_ns = values.n[TI_F_DEADLINE];
    call.action.intent.x_normalized = (uint32_t)values.n[TI_F_X];
    call.action.intent.y_normalized = (uint32_t)values.n[TI_F_Y];
    status = ti_sandbox_bridge_execute(s->bridge, &call, peer_uid,
                                        token, now_ns, receipt);
    ti_ipc_wipe(token, sizeof(token));
    return status;
}

static int ti_ipc_send_reply(int fd, ti_status status,
                             const ti_action_receipt_v1 *receipt,
                             uint64_t end_ns) {
    char line[128];
    size_t total;
    size_t pos = 0u;
    int count = snprintf(line, sizeof(line),
        "{\"ok\":%s,\"status\":%d,\"outcome\":%u,\"level\":%u}\n",
        status == TI_OK ? "true" : "false", (int)status,
        status == TI_OK ? receipt->outcome : 0u,
        status == TI_OK ? receipt->level : 0u);
    if (count <= 0 || (size_t)count >= sizeof(line)) return 0;
    total = (size_t)count;
    while (pos < total) {
        ssize_t n;
        if (!ti_ipc_wait_ready(fd, POLLOUT, end_ns)) return 0;
#if defined(MSG_NOSIGNAL)
        n = send(fd, line + pos, total - pos, MSG_NOSIGNAL);
#else
        n = send(fd, line + pos, total - pos, 0);
#endif
        if (n > 0) pos += (size_t)n;
        else if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                            errno == EWOULDBLOCK)) continue;
        else return 0;
    }
    return 1;
}

#endif /* !WIN32 */

ti_status ti_sandbox_ipc_open(const ti_sandbox_ipc_config_v1 *config,
                              const char *private_directory,
                              ti_sandbox_ipc **out) {
    ti_sandbox_ipc *s;
    ti_sandbox_bridge_config_v1 bc = TI_SANDBOX_BRIDGE_CONFIG_V1_INIT;
    ti_status st;
    if (!out) return TI_EINVAL;
    *out = NULL;
    st = ti_ipc_config_check(config);
    if (st != TI_OK) return st;
    if (!private_directory || !*private_directory) return TI_EINVAL;
#if defined(_WIN32)
    (void)s; (void)bc;
    return TI_EUNSUPPORTED; /* Native Windows local transport must be explicit. */
#else
    s = (ti_sandbox_ipc *)calloc(1u, sizeof(*s));
    if (!s) return TI_ENOMEM;
    atomic_flag_clear(&s->guard);
    s->config = *config;
    s->server_fd = -1;
    st = ti_ipc_listen(s, private_directory);
    if (st != TI_OK) { ti_ipc_wipe(s, sizeof(*s)); free(s); return st; }
    bc.peer_uid = config->peer_uid;
    bc.allowed_tools = config->allowed_tools;
    bc.task_id = config->task_id;
    bc.sandbox_run_id = config->sandbox_run_id;
    bc.action_owner = config->action_owner;
    memcpy(bc.token, s->token, TI_SANDBOX_TOKEN_BYTES);
    st = ti_sandbox_bridge_create(&bc, &s->bridge);
    ti_ipc_wipe(bc.token, sizeof(bc.token));
    if (st != TI_OK) {
        ti_ipc_close_server(s);
        ti_ipc_wipe(s, sizeof(*s));
        free(s);
        return st;
    }
    *out = s;
    return TI_OK;
#endif
}

ti_status ti_sandbox_ipc_credentials(ti_sandbox_ipc *s,
                                     char *path, size_t path_capacity,
                                     char *token_hex, size_t token_capacity) {
    if (!s || !path || !token_hex) return TI_EINVAL;
    if (path_capacity < TI_SANDBOX_IPC_PATH_CAP ||
        token_capacity < TI_SANDBOX_IPC_TOKEN_HEX_CAP) return TI_ERANGE;
    ti_ipc_lock(s);
    if (s->closed) { ti_ipc_unlock(s); return TI_EAUTH; }
    memcpy(path, s->path, strlen(s->path) + 1u);
    memcpy(token_hex, s->token_hex, TI_SANDBOX_IPC_TOKEN_HEX_CAP);
    ti_ipc_unlock(s);
    return TI_OK;
}

ti_status ti_sandbox_ipc_serve_one(ti_sandbox_ipc *s,
                                   uint64_t now_ns, uint32_t timeout_ms) {
    ti_status status = TI_EUNSUPPORTED;
    if (!s || !now_ns || timeout_ms == 0u || timeout_ms > 10000u) return TI_EINVAL;
    ti_ipc_lock(s);
    if (s->closed) { ti_ipc_unlock(s); return TI_EAUTH; }
    if (s->busy) { ti_ipc_unlock(s); return TI_ECONFLICT; }
    s->busy = 1u;
    ti_ipc_unlock(s);
#if !defined(_WIN32)
    {
        const uint64_t started_ns = ti_ipc_time_ns();
        const uint64_t end_ns = started_ns + (uint64_t)timeout_ms * 1000000u;
        struct timeval timeout;
        int fd = -1;
        uint32_t peer_uid = 0u;
        char line[TI_SANDBOX_IPC_MAX_LINE + 1u];
        ti_action_receipt_v1 receipt = TI_ACTION_RECEIPT_V1_INIT;
        uint64_t sampled;
        int flags;
        if (!started_ns || end_ns <= started_ns) {
            status = TI_EUNSUPPORTED;
            goto finish;
        }
        if (!ti_ipc_wait_ready(s->server_fd, POLLIN, end_ns)) {
            status = TI_EEXPIRED;
            goto finish;
        }
        fd = accept(s->server_fd, NULL, NULL);
        if (fd < 0) { status = TI_EUNSUPPORTED; goto finish; }
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            status = TI_EUNSUPPORTED;
            close(fd);
            goto finish;
        }
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
        timeout.tv_sec = (time_t)(timeout_ms / 1000u);
        timeout.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#if defined(SO_NOSIGPIPE)
        {
            int one = 1;
            (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        }
#endif
        if (!ti_ipc_peer_uid(fd, &peer_uid) || peer_uid != s->config.peer_uid) {
            status = TI_EAUTH;
        } else if (!ti_ipc_recv_line(fd, line, TI_SANDBOX_IPC_MAX_LINE, end_ns)) {
            status = ti_ipc_time_ns() >= end_ns ? TI_EEXPIRED : TI_EINVAL;
        } else {
            sampled = ti_ipc_time_ns();
            if (!sampled || sampled >= end_ns || sampled < started_ns ||
                now_ns > UINT64_MAX - (sampled - started_ns)) {
                status = TI_EEXPIRED;
            } else {
                /* The original trusted host clock is advanced by the actual
                 * accept/read elapsed time, so a slow sender cannot revive an
                 * action whose deadline already passed during socket reads. */
                status = ti_ipc_apply(s, line, peer_uid,
                                      now_ns + sampled - started_ns, &receipt);
            }
        }
        if (!ti_ipc_send_reply(fd, status, &receipt, end_ns) &&
            status == TI_OK) status = TI_EUNSUPPORTED;
        close(fd);
finish:
        ti_ipc_wipe(line, sizeof(line));
    }
#endif
    ti_ipc_lock(s);
    s->busy = 0u;
    ti_ipc_unlock(s);
    return status;
}

ti_status ti_sandbox_ipc_close(ti_sandbox_ipc *s) {
    if (!s) return TI_EINVAL;
    ti_ipc_lock(s);
    if (s->busy) { ti_ipc_unlock(s); return TI_ECONFLICT; }
    if (!s->closed) {
        s->closed = 1u;
        (void)ti_sandbox_bridge_close(s->bridge);
#if !defined(_WIN32)
        ti_ipc_close_server(s);
#endif
        ti_ipc_wipe(s->token, sizeof(s->token));
        ti_ipc_wipe(s->token_hex, sizeof(s->token_hex));
    }
    ti_ipc_unlock(s);
    return TI_OK;
}

ti_status ti_sandbox_ipc_destroy(ti_sandbox_ipc **s) {
    ti_sandbox_ipc *value;
    ti_status status;
    if (!s || !*s) return TI_EINVAL;
    value = *s;
    status = ti_sandbox_ipc_close(value);
    if (status != TI_OK) return status;
    status = ti_sandbox_bridge_destroy(&value->bridge);
    if (status != TI_OK) return status;
    *s = NULL;
    ti_ipc_wipe(value, sizeof(*value));
    free(value);
    return TI_OK;
}
