/*
 * TurboInteract reviewed one-shot sandbox phone-tool worker.
 * C11 POSIX executable: NO shell, model interpreter, fork/exec, filesystem
 * mutation, root/ADB, Binder, or direct Android input.
 *
 * The trusted parent sends exactly three newline-delimited UTF-8/ASCII lines:
 * TURBOINTERACT-SANDBOX/1 \n  absolute socket pathname \n  64 lowercase
 * hex characters \n  EOF. Never pass the token in argv/env/stdout.
 *
 * One explicit tap or BACK request is selected by a reviewed host argv.
 * Server OS peer credentials, token, task/run identity and live C11 ActionOwner
 * grant are authoritative; this worker cannot grant itself permission.
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#elif !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define TI_WORKER_PATH_BYTES 104u
#define TI_WORKER_TOKEN_BYTES 65u
#define TI_WORKER_HANDOFF_CAP 256u
#define TI_WORKER_REQUEST_CAP 512u
#define TI_WORKER_REPLY_CAP 128u

static void ti_wipe(void *memory, size_t count) {
    volatile unsigned char *p = (volatile unsigned char *)memory;
    while (count--) *p++ = 0;
}

static int ti_uint64(const char *source, uint64_t *value) {
    uint64_t number = 0u;
    const unsigned char *p = (const unsigned char *)source;
    if (!p || !*p) return 0;
    if (*p == '0' && p[1]) return 0;
    while (*p) {
        uint64_t digit;
        if (*p < '0' || *p > '9') return 0;
        digit = (uint64_t)(*p - '0');
        if (number > (UINT64_MAX - digit) / 10u) return 0;
        number = number * 10u + digit;
        ++p;
    }
    *value = number;
    return 1;
}

static int ti_read_handoff(char *path, size_t path_cap, char token[TI_WORKER_TOKEN_BYTES]) {
    static const char header[] = "TURBOINTERACT-SANDBOX/1\n";
    char bytes[TI_WORKER_HANDOFF_CAP];
    size_t total = 0u;
    size_t i, first, second, third;
    int result = 0;
    for (;;) {
        ssize_t n;
        if (total == sizeof(bytes)) goto finish;
        n = read(STDIN_FILENO, bytes + total, sizeof(bytes) - total);
        if (n > 0) { total += (size_t)n; continue; }
        if (n == 0) break;
        if (errno != EINTR) goto finish;
    }
    first = sizeof(header) - 1u;
    if (total <= first + TI_WORKER_TOKEN_BYTES ||
        memcmp(bytes, header, first) != 0) goto finish;
    second = first;
    while (second < total && bytes[second] != '\n') second++;
    if (second == total || second == first ||
        second - first >= path_cap || bytes[first] != '/') goto finish;
    for (i = first; i < second; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c < 33u || c > 126u || c == '\\') goto finish;
    }
    third = second + 1u;
    if (total != third + 65u || bytes[total - 1u] != '\n') goto finish;
    for (i = 0u; i < 64u; i++) {
        unsigned char c = (unsigned char)bytes[third + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) goto finish;
    }
    memcpy(path, bytes + first, second - first);
    path[second - first] = '\0';
    memcpy(token, bytes + third, 64u);
    token[64] = '\0';
    result = 1;
finish:
    ti_wipe(bytes, sizeof(bytes));
    return result;
}

static int ti_parse_reply(const char *line, int *status) {
    int end = -1;
    int n;
    unsigned outcome = 0u, level = 0u;
    char ok[6];
    /* The server's fixed reply is a small wire contract, not a task-result
     * verifier. The only success is that the host processed one request. */
    n = sscanf(line,
        "{\"ok\":%5[a-z],\"status\":%d,\"outcome\":%u,\"level\":%u}%n",
        ok, status, &outcome, &level, &end);
    if (n != 4 || end < 0 ||
        (line[end] != '\n' && line[end] != '\0') ||
        (line[end] == '\n' && line[end + 1] != '\0')) return 0;
    if (strcmp(ok, "true") != 0 && strcmp(ok, "false") != 0) return 0;
    if ((*status == 0) != (strcmp(ok, "true") == 0)) return 0;
    if (outcome > 4u || level > 3u) return 0;
    return 1;
}

static int ti_exchange(const char *path, const char *request, int *status) {
    struct sockaddr_un address;
    struct timeval timeout = {2, 0};
    char reply[TI_WORKER_REPLY_CAP];
    size_t request_size = strlen(request), offset = 0u, have = 0u;
    int sock, result = 0;
    ssize_t n;
    if (strlen(path) >= sizeof(address.sun_path) || request_size == 0u ||
        request_size >= TI_WORKER_REQUEST_CAP) return 0;
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#if defined(SO_NOSIGPIPE)
    { int one = 1; (void)setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    if (connect(sock, (const struct sockaddr *)&address,
                (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                            strlen(path) + 1u)) != 0) goto finish;
    while (offset < request_size) {
#if defined(MSG_NOSIGNAL)
        n = send(sock, request + offset, request_size - offset, MSG_NOSIGNAL);
#else
        n = send(sock, request + offset, request_size - offset, 0);
#endif
        if (n > 0) offset += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else goto finish;
    }
    for (;;) {
        n = recv(sock, reply + have, sizeof(reply) - have - 1u, 0);
        if (n > 0) {
            size_t i;
            have += (size_t)n;
            reply[have] = '\0';
            for (i = 0u; i < have; ++i) {
                if (reply[i] == '\n') {
                    if (i + 1u != have) goto finish;
                    result = ti_parse_reply(reply, status);
                    goto finish;
                }
            }
            if (have + 1u >= sizeof(reply)) goto finish;
        } else if (n == 0) goto finish;
        else if (errno != EINTR) goto finish;
    }
finish:
    ti_wipe(reply, sizeof(reply));
    (void)close(sock);
    return result;
}

int main(int argc, char **argv) {
    uint64_t values[10] = {0};
    char path[TI_WORKER_PATH_BYTES] = {0};
    char token[TI_WORKER_TOKEN_BYTES] = {0};
    char request[TI_WORKER_REQUEST_CAP] = {0};
    int is_tap, formatted, status = -1, return_code = 2;
    int i;
    /* argv is a host-reviewed, fixed schema; token never belongs here. */
    if (argc < 2 || (strcmp(argv[1], "tap") != 0 && strcmp(argv[1], "back") != 0))
        return 2;
    is_tap = strcmp(argv[1], "tap") == 0;
    if (argc != (is_tap ? 12 : 10)) return 2;
    for (i = 2; i < argc; ++i) {
        if (!ti_uint64(argv[i], &values[i - 2])) return 2;
    }
    /* call/action/attempt, display and geometry generations and deadline */
    if (!values[0] || !values[1] || !values[2] ||
        !values[5] || !values[6] || !values[7] ||
        (is_tap && (!values[3] || !values[4] ||
                    values[8] > 1000000u || values[9] > 1000000u)))
        return 2;
    if (!ti_read_handoff(path, sizeof(path), token)) goto finish;
    if (is_tap) {
        formatted = snprintf(request, sizeof(request),
            "{\"token\":\"%s\",\"tool\":\"tap\",\"callId\":%" PRIu64
            ",\"actionId\":%" PRIu64 ",\"attemptId\":%" PRIu64
            ",\"observationId\":%" PRIu64 ",\"captureSequence\":%" PRIu64
            ",\"displayGeneration\":%" PRIu64
            ",\"geometryGeneration\":%" PRIu64 ",\"deadlineNs\":%" PRIu64
            ",\"x\":%" PRIu64 ",\"y\":%" PRIu64 "}\n",
            token, values[0], values[1], values[2], values[3], values[4],
            values[5], values[6], values[7], values[8], values[9]);
    } else {
        formatted = snprintf(request, sizeof(request),
            "{\"token\":\"%s\",\"tool\":\"back\",\"callId\":%" PRIu64
            ",\"actionId\":%" PRIu64 ",\"attemptId\":%" PRIu64
            ",\"observationId\":%" PRIu64 ",\"captureSequence\":%" PRIu64
            ",\"displayGeneration\":%" PRIu64
            ",\"geometryGeneration\":%" PRIu64 ",\"deadlineNs\":%" PRIu64 "}\n",
            token, values[0], values[1], values[2], values[3], values[4],
            values[5], values[6], values[7]);
    }
    if (formatted <= 0 || (size_t)formatted >= sizeof(request)) goto finish;
    if (!ti_exchange(path, request, &status)) goto finish;
    /* This is ONLY an RPC outcome. It does not establish UI/task success. */
    return_code = status == 0 ? 0 : 3;
finish:
    ti_wipe(path, sizeof(path));
    ti_wipe(token, sizeof(token));
    ti_wipe(request, sizeof(request));
    return return_code;
}
