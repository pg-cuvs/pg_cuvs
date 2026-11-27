/* Exercise terminal BUILD waits against shortened socket timeouts. */
#define _GNU_SOURCE 1

#include "cuvs_ipc.h"
#include "cuvs_build_corpus.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int __real_setsockopt(int fd, int level, int name, const void *value, socklen_t size);

static _Thread_local int shortened_receive;
static _Thread_local long send_timeout_sec;

int
__wrap_setsockopt(int fd, int level, int name, const void *value, socklen_t size)
{
    if (level == SOL_SOCKET && name == SO_RCVTIMEO && size == sizeof(struct timeval))
    {
        struct timeval timeout;
        memcpy(&timeout, value, sizeof(timeout));
        if (timeout.tv_sec == 600)
        {
            int rc;

            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;
            rc = __real_setsockopt(fd, level, name, &timeout, sizeof(timeout));
            shortened_receive = rc == 0;
            return rc;
        }
    }
    if (level == SOL_SOCKET && name == SO_SNDTIMEO)
    {
        int rc = __real_setsockopt(fd, level, name, value, size);
        struct timeval actual;
        socklen_t actual_size = sizeof(actual);

        if (rc == 0 && getsockopt(fd, SOL_SOCKET, name, &actual, &actual_size) == 0)
            send_timeout_sec = actual.tv_sec;
        return rc;
    }
    return __real_setsockopt(fd, level, name, value, size);
}

enum BuildOp { BUILD, BUILD_FLAT, BUILD_MULTI, BUILD_IVFPQ, BUILD_PAGES, BUILD_COUNT };
enum ReplyMode { LATE_OK, PEER_LOST, SILENT };

static const char *const op_names[BUILD_COUNT] = {
    "BUILD", "BUILD_FLAT", "BUILD_MULTI", "BUILD_IVFPQ", "BUILD_PAGES"
};
static const uint32_t wire_ops[BUILD_COUNT] = {
    CUVS_OP_BUILD, CUVS_OP_BUILD_FLAT, CUVS_OP_BUILD, CUVS_OP_BUILD_IVFPQ, CUVS_OP_BUILD_PAGES
};
static const uint64_t page_nonce = 12345;
static const char build_notice[] = "mock build notice";

#define REPLY_DELAY_MS 600        /* longer than the injected 100 ms timeout */
#define MOCK_TIMEOUT_MS 3000
#define PROMPT_LIMIT_MS 2000

typedef struct Mock {
    int listener;
    enum BuildOp op;
    enum ReplyMode mode;
    int ok;
} Mock;

static int failures;
static int cb_calls;

static void
check(const char *op, const char *label, long actual, long expected)
{
    if (actual != expected)
    {
        fprintf(stderr, "FAIL %s / %s: got %ld, expected %ld\n", op, label, actual, expected);
        failures++;
    }
}

static int
wait_cb(void)
{
    cb_calls++;
    return 1;
}

static long
mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int
read_request(int fd, enum BuildOp op)
{
    CuvsCmdFrame cmd;
    CuvsPageBuildRequest page;
    char index_dir[CUVS_PAGE_INDEX_DIR_SIZE];
    CuvsPartialDesc partial;
    uint32_t n_partials = op == BUILD_MULTI ? 1 : 0;

    return recv(fd, &cmd, sizeof(cmd), MSG_WAITALL) == (ssize_t) sizeof(cmd) &&
           cmd.op == wire_ops[op] && cmd.n_partials == n_partials &&
           cmd.proto_magic == CUVS_PROTO_MAGIC && cmd.proto_version == CUVS_PROTO_VERSION &&
           (op != BUILD_PAGES ||
            (recv(fd, &page, sizeof(page), MSG_WAITALL) == (ssize_t) sizeof(page) &&
             page.length == sizeof(page))) &&
           recv(fd, index_dir, sizeof(index_dir), MSG_WAITALL) == (ssize_t) sizeof(index_dir) &&
           (op != BUILD_MULTI ||
            recv(fd, &partial, sizeof(partial), MSG_WAITALL) == (ssize_t) sizeof(partial));
}

static void *
mock_daemon(void *arg)
{
    Mock *mock = arg;
    struct pollfd listener = {.fd = mock->listener, .events = POLLIN};
    struct timeval timeout = {.tv_sec = MOCK_TIMEOUT_MS / 1000};
    CuvsReplyHeader reply = {.status = CUVS_STATUS_OK};
    int fd;

    if (poll(&listener, 1, MOCK_TIMEOUT_MS) <= 0)
        return NULL;
    fd = accept(mock->listener, NULL, NULL);
    if (fd < 0)
        return NULL;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        !read_request(fd, mock->op))
        goto done;

    if (mock->mode == LATE_OK)
    {
        usleep(REPLY_DELAY_MS * 1000);
        memcpy(reply.notice_out, build_notice, sizeof(build_notice));
        if (write(fd, &reply, sizeof(reply)) != (ssize_t) sizeof(reply))
            goto done;
        if (mock->op == BUILD_PAGES &&
            write(fd, &page_nonce, sizeof(page_nonce)) != (ssize_t) sizeof(page_nonce))
            goto done;
    }
    else if (mock->mode == SILENT)
    {
        struct pollfd client = {.fd = fd, .events = POLLIN};
        /* Stay connected without a reply until the cancelled client closes. */
        if (poll(&client, 1, MOCK_TIMEOUT_MS) <= 0)
            goto done;
    }
    mock->ok = 1;
done:
    close(fd);
    return NULL;
}

static int
call_op(enum BuildOp op, const char *path, uint64_t *nonce_out)
{
    CuvsBuildCorpus corpus = {.kind = CORPUS_SHM, .fd = -1, .shm_name = "/pgcuvs-mock"};
    CuvsPartialDesc partial = {.shm_name = "/pgcuvs-mock-p0", .n_vecs = 2};
    CuvsPageBuildRequest page = {.length = sizeof(page)};
    const float vecs[4] = {1, 2, 3, 4};
    const uint64_t tids[2] = {1, 2};

    switch (op)
    {
        case BUILD:
        case BUILD_PAGES:
            return cuvs_ipc_build(path, 1, 2, &corpus, NULL, NULL, 2, 2, 0, "/tmp",
                                  3, 4, 1, 0, 0, 0, 0,
                                  op == BUILD_PAGES ? &page : NULL, nonce_out);
        case BUILD_FLAT:
            return cuvs_ipc_build_flat(path, 1, 2, &corpus, NULL, NULL, 2, 2, 0, "/tmp", 3, 4);
        case BUILD_MULTI:
            return cuvs_ipc_build_multi(path, 1, 2, &partial, 1, 2, 2, 0, "/tmp",
                                        3, 4, 0, 0, 0, 0, 0);
        case BUILD_IVFPQ:
            return cuvs_ipc_build_ivfpq(path, 1, 2, vecs, tids, 2, 2, 0, "/tmp", 3, 4, 0, 0, 0);
        default:
            abort();
    }
}

static void
run_case(enum BuildOp op, const char *path, enum ReplyMode mode)
{
    Mock mock = {.listener = socket(AF_UNIX, SOCK_STREAM, 0), .op = op, .mode = mode};
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    pthread_t thread;
    uint64_t nonce = 0;
    long start, elapsed;
    int rc;

    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (mock.listener < 0 || bind(mock.listener, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
        listen(mock.listener, 1) != 0 || pthread_create(&thread, NULL, mock_daemon, &mock) != 0)
    {
        fprintf(stderr, "FAIL %s / mock daemon setup\n", op_names[op]);
        exit(EXIT_FAILURE);
    }

    cb_calls = 0;
    cancel_after = mode == SILENT ? 2 : 0;
    shortened_receive = 0;
    send_timeout_sec = -1;
    start = mono_ms();
    rc = call_op(op, path, &nonce);
    elapsed = mono_ms() - start;
    pthread_join(thread, NULL);
    close(mock.listener);
    unlink(path);

    check(op_names[op], "mock daemon read request and completed its script", mock.ok, 1);
    check(op_names[op], "600-second receive timeout was shortened by the fixture", shortened_receive, 1);
    check(op_names[op], "send timeout after connect", send_timeout_sec, 600);
    switch (mode)
    {
        case LATE_OK:
            check(op_names[op], "wait callback was polled before the late reply", cb_calls > 0, 1);
            check(op_names[op], "late reply status", rc, CUVS_STATUS_OK);
            check(op_names[op], "daemon notice", strcmp(cuvs_ipc_last_notice_out(), build_notice), 0);
            if (op == BUILD_PAGES)
                check(op_names[op], "graph nonce", (long) nonce, (long) page_nonce);
            break;
        case PEER_LOST:
            check(op_names[op], "peer loss status", rc, CUVS_STATUS_ERROR);
            check(op_names[op], "peer loss completed promptly", elapsed < PROMPT_LIMIT_MS, 1);
            break;
        case SILENT:
            check(op_names[op], "silent daemon status", rc, CUVS_STATUS_CANCELED);
            check(op_names[op], "silent daemon cancel was prompt", elapsed < PROMPT_LIMIT_MS, 1);
            check(op_names[op], "silent daemon cancel callback", cb_calls >= cancel_after, 1);
            break;
    }
}

int
main(void)
{
    char dir[] = "/tmp/pgcuvs-bw-XXXXXX";
    char path[sizeof(dir) + sizeof("/mock.sock")];

    if (mkdtemp(dir) == NULL)
    {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }
    snprintf(path, sizeof(path), "%s/mock.sock", dir);
    signal(SIGPIPE, SIG_IGN);
    alarm(90);
    cuvs_ipc_set_wait_callback(wait_cb);

    for (enum BuildOp op = BUILD; op < BUILD_COUNT; op++)
    {
        run_case(op, path, LATE_OK);
        run_case(op, path, PEER_LOST);
        run_case(op, path, SILENT);
    }
    alarm(0);
    rmdir(dir);

    if (failures)
    {
        fprintf(stderr, "test_build_terminal_wait: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("test_build_terminal_wait: all checks passed");
    return EXIT_SUCCESS;
}
