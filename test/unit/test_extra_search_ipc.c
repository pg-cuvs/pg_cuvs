/* Exercise the opt-in client over a private UDS without running PostgreSQL. */
#define _GNU_SOURCE 1

#include "cuvs_ipc.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum ReplyKind {
    NORMAL,
    TOO_MANY_RESULTS,
    DIFFERENT_GENERATION,
    EXCESS_COMPONENTS,
    LEGACY_UNKNOWN_OPCODE,
    MALFORMED_REFUSAL,
    REPLIED_UNAVAILABLE,
    LEGACY_FAST_CLOSE,
    GENERIC_FAST_CLOSE,
    SILENT_FAST_CLOSE,
    NO_BODY,
    CANCEL_BODY
};

typedef struct Mock {
    int listener;
    enum ReplyKind kind;
    CuvsCmdFrame command;
    CuvsExtraSearchRequest request;
    float query[4];
} Mock;

static atomic_int canceled;

/* For the fast-close cases, wait after the real command-frame write until
 * the peer has buffered its reply and closed. The payload send then fails
 * through the kernel instead of relying on which thread happens to run. */
#undef send
extern ssize_t send(int sockfd, const void *buf, size_t len, int flags);

static pthread_mutex_t close_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t close_ready = PTHREAD_COND_INITIALIZER;
static int wait_after_frame;
static int peer_closed;
static int payload_broken_pipe;

ssize_t
cuvs_test_send(int fd, const void *buffer, size_t len, int flags)
{
    ssize_t result = send(fd, buffer, len, flags);

    if (wait_after_frame && len == sizeof(CuvsCmdFrame) && result == (ssize_t) len)
    {
        pthread_mutex_lock(&close_lock);
        while (!peer_closed)
            pthread_cond_wait(&close_ready, &close_lock);
        pthread_mutex_unlock(&close_lock);
    }
    if (wait_after_frame && result < 0 && errno == EPIPE)
        payload_broken_pipe = 1;
    return result;
}

static int
closes_before_payload(enum ReplyKind kind)
{
    return kind == LEGACY_FAST_CLOSE || kind == GENERIC_FAST_CLOSE ||
           kind == SILENT_FAST_CLOSE;
}

static int
interrupt_pending(void)
{
    return atomic_load(&canceled);
}

static void *
reply_once(void *ptr)
{
    Mock *mock = ptr;
    int fd = accept(mock->listener, NULL, NULL);
    assert(fd >= 0);
    assert(recv(fd, &mock->command, sizeof(mock->command), MSG_WAITALL) ==
           (ssize_t) sizeof(mock->command));
    if (closes_before_payload(mock->kind))
    {
        CuvsReplyHeader early = {.status = CUVS_STATUS_ERROR};

        if (mock->kind != SILENT_FAST_CLOSE)
        {
            const char *error = mock->kind == LEGACY_FAST_CLOSE ? "unknown op" : "other error";
            snprintf(early.error, sizeof(early.error), "%s", error);
            assert(write(fd, &early, sizeof(early)) == (ssize_t) sizeof(early));
        }
        assert(close(fd) == 0);
        pthread_mutex_lock(&close_lock);
        peer_closed = 1;
        pthread_cond_signal(&close_ready);
        pthread_mutex_unlock(&close_lock);
        return NULL;
    }
    assert(recv(fd, &mock->request, sizeof(mock->request), MSG_WAITALL) ==
           (ssize_t) sizeof(mock->request));
    assert(recv(fd, mock->query, sizeof(mock->query), MSG_WAITALL) ==
           (ssize_t) sizeof(mock->query));

    CuvsReplyHeader header = {.n_results = mock->kind == TOO_MANY_RESULTS ? 3 : 2,
                             .latency_us = 137};
    CuvsExtraSearchReply details = {
        .generation = mock->kind == DIFFERENT_GENERATION ? 93 : 92,
        .scored_rows = mock->kind == EXCESS_COMPONENTS ? 5 : 4,
        .components = mock->kind == EXCESS_COMPONENTS ? 20 : 16
    };
    CuvsResult results[2] = {{.tid = 0x10001, .distance = 1},
                             {.tid = 0x10002, .distance = 2}};
    char closed;

    if (mock->kind == LEGACY_UNKNOWN_OPCODE)
    {
        header.status = CUVS_STATUS_ERROR;
        header.n_results = 0;
        snprintf(header.error, sizeof(header.error), "unknown op");
    }
    if (mock->kind == MALFORMED_REFUSAL)
        header.status = CUVS_EXTRA_SEARCH_UNSUPPORTED;
    if (mock->kind == REPLIED_UNAVAILABLE)
    {
        header.status = CUVS_STATUS_UNAVAILABLE;
        header.n_results = 0;
    }
    assert(write(fd, &header, sizeof(header)) == (ssize_t) sizeof(header));
    if (header.status == CUVS_STATUS_OK && mock->kind != TOO_MANY_RESULTS)
        assert(write(fd, &details, sizeof(details)) == (ssize_t) sizeof(details));
    if (mock->kind == NORMAL)
        assert(write(fd, results, sizeof(results)) == (ssize_t) sizeof(results));
    if (mock->kind == CANCEL_BODY)
        atomic_store(&canceled, 1);

    ssize_t end = recv(fd, &closed, sizeof(closed), 0);
    assert(end == 0 || (end < 0 && errno == ECONNRESET));
    close(fd);
    return NULL;
}

static void
check_reply(const char *socket_path, enum ReplyKind kind, int expected)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    Mock mock = {.listener = socket(AF_UNIX, SOCK_STREAM, 0), .kind = kind};
    assert(mock.listener >= 0);
    assert(strlen(socket_path) < sizeof(addr.sun_path));
    strcpy(addr.sun_path, socket_path);
    assert(bind(mock.listener, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    assert(listen(mock.listener, 1) == 0);

    atomic_store(&canceled, 0);
    wait_after_frame = closes_before_payload(kind);
    peer_closed = 0;
    payload_broken_pipe = 0;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, reply_once, &mock) == 0);

    float query[4] = {1, 2, 3, 4};
    CuvsResult results[2];
    int count = 99;
    uint32_t latency_us = 99;
    CuvsExtraSearchReply details = {.generation = 1};
    int64_t deadline = cuvs_ipc_extra_search_deadline_ms();
    if (kind == NO_BODY)
        deadline -= CUVS_EXTRA_SEARCH_TIMEOUT_MS - 1250;
    int status = cuvs_ipc_search_extra(socket_path, 1, 2, query, 4, 2, 92, 16,
                                       3, 42, deadline, results, &count, &details, &latency_us);
    assert(pthread_join(thread, NULL) == 0);
    assert(close(mock.listener) == 0);
    assert(unlink(socket_path) == 0);
    assert(mock.command.proto_magic == CUVS_PROTO_MAGIC);
    assert(mock.command.proto_version == CUVS_PROTO_VERSION);
    assert(mock.command.op == CUVS_OP_SEARCH_EXTRA);
    assert(mock.command.db_oid == 1 && mock.command.index_oid == 2);
    assert(mock.command.k == 2 && mock.command.dim == 4);
    if (closes_before_payload(kind))
        assert(payload_broken_pipe);
    else
    {
        assert(mock.request.version == CUVS_EXTRA_SEARCH_VERSION);
        assert(mock.request.generation == 92 && mock.request.max_components == 16);
        assert(mock.request.dir_dev == 3 && mock.request.dir_ino == 42);
        assert(mock.request.deadline_ms == deadline);
        assert(memcmp(query, mock.query, sizeof(query)) == 0);
    }
    if (status != expected)
        fprintf(stderr, "case %d: status %d, expected %d\n", kind, status, expected);
    assert(status == expected);

    if (kind == NORMAL)
    {
        assert(latency_us == 137);
        assert(count == 2 && details.generation == 92 && details.components == 16);
        assert(results[0].tid == 0x10001 && results[1].tid == 0x10002);
    }
    else
        assert(latency_us == 0 && count == 0 && details.generation == 0);
}

int
main(void)
{
    char dir[] = "/tmp/cuvs-extra-ipc-XXXXXX";
    char socket_path[sizeof(dir) + 8];

    assert(mkdtemp(dir) != NULL);
    snprintf(socket_path, sizeof(socket_path), "%s/sock", dir);
    cuvs_ipc_set_daemon_uid((int) getuid());
    cuvs_ipc_set_wait_callback(interrupt_pending);
    check_reply(socket_path, LEGACY_FAST_CLOSE, CUVS_EXTRA_SEARCH_UNSUPPORTED);
    check_reply(socket_path, GENERIC_FAST_CLOSE, CUVS_STATUS_ERROR);
    check_reply(socket_path, SILENT_FAST_CLOSE, CUVS_STATUS_ERROR);
    check_reply(socket_path, NORMAL, CUVS_STATUS_OK);
    check_reply(socket_path, TOO_MANY_RESULTS, CUVS_STATUS_ERROR);
    check_reply(socket_path, DIFFERENT_GENERATION, CUVS_EXTRA_SEARCH_GENERATION);
    check_reply(socket_path, EXCESS_COMPONENTS, CUVS_STATUS_ERROR);
    check_reply(socket_path, LEGACY_UNKNOWN_OPCODE, CUVS_EXTRA_SEARCH_UNSUPPORTED);
    check_reply(socket_path, MALFORMED_REFUSAL, CUVS_STATUS_ERROR);
    check_reply(socket_path, REPLIED_UNAVAILABLE, CUVS_STATUS_ERROR);
    check_reply(socket_path, NO_BODY, CUVS_EXTRA_SEARCH_DEADLINE);
    check_reply(socket_path, CANCEL_BODY, CUVS_STATUS_CANCELED);
    assert(rmdir(dir) == 0);
    puts("opt-in search IPC: fast-close fallback, latency, caps and cancellation passed");
    return 0;
}
