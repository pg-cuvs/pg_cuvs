/*
 * Exercise the four BUILD clients against a mock daemon over a Unix socket.
 * A late reply should arrive while cancel is being polled. A silent peer
 * should let a cancel end the wait, and a lost peer should fail the build.
 */
#define _GNU_SOURCE 1
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

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

enum BuildOp { BUILD, BUILD_FLAT, BUILD_MULTI, BUILD_IVFPQ, BUILD_PAGES, BUILD_COUNT };
enum ReplyMode { LATE_OK, SILENT, PEER_LOST, NONCE_SILENT, NONCE_TRUNCATED };

static const char *const op_names[BUILD_COUNT] = {
    "BUILD", "BUILD_FLAT", "BUILD_MULTI", "BUILD_IVFPQ", "BUILD_PAGES"
};
static const uint32_t wire_ops[BUILD_COUNT] = {
    CUVS_OP_BUILD, CUVS_OP_BUILD_FLAT, CUVS_OP_BUILD, CUVS_OP_BUILD_IVFPQ, CUVS_OP_BUILD_PAGES
};
static const uint64_t page_nonce = 12345;
static const char build_notice[] = "mock build notice";

#define REPLY_DELAY_MS 600        /* more than two 250 ms poll slices */
#define MOCK_TIMEOUT_MS 3000      /* let the pre-fix blocking client see EOF */
#define PROMPT_LIMIT_MS 2000

typedef struct Mock {
    int listener;
    enum BuildOp op;
    enum ReplyMode mode;
    int ok;
} Mock;

static int failures;
static int cb_calls;
static int cancel_after;
static int cancel_on_nonce;

#ifdef CUVS_TEST_BUILD_REPLY_CLOCK
static int advance_reply_clock;
static _Thread_local int clock_offset_sec;

int __real_clock_gettime(clockid_t clock_id, struct timespec *ts);

int
__wrap_clock_gettime(clockid_t clock_id, struct timespec *ts)
{
    int rc = __real_clock_gettime(clock_id, ts);

    if (rc == 0 && clock_id == CLOCK_MONOTONIC)
        ts->tv_sec += clock_offset_sec;
    return rc;
}
#endif

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
    /* The saved notice is visible on this thread after the header is read. */
    if (cancel_on_nonce && strcmp(cuvs_ipc_last_notice_out(), build_notice) != 0)
        return 0;
#ifdef CUVS_TEST_BUILD_REPLY_CLOCK
    /* The deadline clock has started and the request is sent. Advance only
     * the client clock. The mock's socket and sleep still use real time. */
    clock_offset_sec = advance_reply_clock;
#endif
    cb_calls++;
    return cancel_after > 0 && cb_calls >= cancel_after;
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
        usleep(REPLY_DELAY_MS * 1000);
    if (mock->mode != PEER_LOST && mock->mode != SILENT)
    {
        memcpy(reply.notice_out, build_notice, sizeof(build_notice));
        if (write(fd, &reply, sizeof(reply)) != (ssize_t) sizeof(reply))
            goto done;
        if (mock->op == BUILD_PAGES && mock->mode != NONCE_SILENT)
        {
            size_t bytes = mock->mode == NONCE_TRUNCATED ? sizeof(page_nonce) / 2 :
                                                          sizeof(page_nonce);
            if (write(fd, &page_nonce, bytes) != (ssize_t) bytes)
                goto done;
        }
    }
    if (mock->mode == SILENT || mock->mode == NONCE_SILENT)
    {
        struct pollfd peer = {.fd = fd, .events = POLLIN};

        /* Leave the request outstanding until the client closes or the
         * watchdog releases a client that is still blocking in read(). */
        (void) poll(&peer, 1, MOCK_TIMEOUT_MS);
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
run_case(enum BuildOp op, const char *path, enum ReplyMode mode, int advance_seconds)
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
    cancel_on_nonce = mode == NONCE_SILENT;
    cancel_after = (mode == SILENT || mode == NONCE_SILENT) && !advance_seconds ? 3 : 0;
#ifdef CUVS_TEST_BUILD_REPLY_CLOCK
    advance_reply_clock = advance_seconds;
#endif
    start = mono_ms();
    rc = call_op(op, path, &nonce);
#ifdef CUVS_TEST_BUILD_REPLY_CLOCK
    clock_offset_sec = 0;
    advance_reply_clock = 0;
#endif
    elapsed = mono_ms() - start;
    pthread_join(thread, NULL);
    close(mock.listener);
    unlink(path);

    check(op_names[op], "mock daemon read request and completed its script", mock.ok, 1);
    switch (mode)
    {
        case LATE_OK:
            check(op_names[op], "late reply status", rc, CUVS_STATUS_OK);
            check(op_names[op], "polled while waiting", cb_calls > 1, 1);
            check(op_names[op], "daemon notice", strcmp(cuvs_ipc_last_notice_out(), build_notice), 0);
            if (op == BUILD_PAGES)
                check(op_names[op], "graph nonce", (long) nonce, (long) page_nonce);
            break;
        case SILENT:
        case NONCE_SILENT:
            if (advance_seconds)
            {
                check(op_names[op], "deadline exceeded on the client clock", rc, CUVS_STATUS_ERROR);
                check(op_names[op], "deadline completed before mock timeout",
                      elapsed < PROMPT_LIMIT_MS, 1);
                break;
            }
            check(op_names[op], mode == SILENT ? "reply cancel status" : "nonce cancel status",
                  rc, CUVS_STATUS_CANCELED);
            check(op_names[op], "cancel completed before mock timeout", elapsed < PROMPT_LIMIT_MS, 1);
            break;
        case PEER_LOST:
        case NONCE_TRUNCATED:
            check(op_names[op], mode == PEER_LOST ? "peer loss status" : "truncated nonce status",
                  rc, CUVS_STATUS_ERROR);
            check(op_names[op], "peer loss completed promptly", elapsed < PROMPT_LIMIT_MS, 1);
            break;
    }
}

int
main(int argc, char **argv)
{
    char dir[] = "/tmp/pgcuvs-bw-XXXXXX";
    char path[sizeof(dir) + sizeof("/mock.sock")];
    int deadline_only = 0;

#ifdef CUVS_TEST_BUILD_REPLY_CLOCK
    deadline_only = argc == 2 && strcmp(argv[1], "--deadline") == 0;
#else
    (void) argv;
#endif
    if (argc != 1 && !deadline_only)
        return EXIT_FAILURE;
    if (mkdtemp(dir) == NULL)
    {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }
    snprintf(path, sizeof(path), "%s/mock.sock", dir);
    signal(SIGPIPE, SIG_IGN);
    alarm(90);
    cuvs_ipc_set_wait_callback(wait_cb);

    if (deadline_only)
    {
        run_case(BUILD, path, LATE_OK, 601);
        run_case(BUILD, path, SILENT, 86401);
    }
    else
    {
        for (enum BuildOp op = BUILD; op < BUILD_COUNT; op++)
        {
#ifdef __APPLE__
            /* IVF-PQ stages a named shm key longer than the macOS name limit. */
            if (op == BUILD_IVFPQ)
            {
                puts("SKIP BUILD_IVFPQ on macOS (named-shm key length)");
                continue;
            }
#endif
            uint64_t nonce = 0;

            run_case(op, path, LATE_OK, 0);
            run_case(op, path, SILENT, 0);
            run_case(op, path, PEER_LOST, 0);

            cancel_after = 0;
            check(op_names[op], "no listener", call_op(op, path, &nonce), CUVS_STATUS_UNAVAILABLE);
            cb_calls = 0;
            cancel_after = 1;
            check(op_names[op], "connect cancel status", call_op(op, path, &nonce), CUVS_STATUS_CANCELED);
        }
        run_case(BUILD_PAGES, path, NONCE_SILENT, 0);
        run_case(BUILD_PAGES, path, NONCE_TRUNCATED, 0);
    }
    alarm(0);
    rmdir(dir);

    if (failures)
    {
        fprintf(stderr, "test_build_reply_wait: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts(deadline_only ? "test_build_reply_wait: deadline checks passed" :
                         "test_build_reply_wait: all checks passed");
    return EXIT_SUCCESS;
}
