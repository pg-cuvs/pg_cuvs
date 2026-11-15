#define _GNU_SOURCE 1
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "cuvs_ipc.h"
#include "cuvs_build_corpus.h"
#include "cuvs_hnsw_build_common.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int listener, fixture_case, polls;
static int *accepts;
static uint32_t requested_algo(void) { return fixture_case >= 19 && fixture_case <= 24 ? CUVS_HNSW_ALGO_NN_DESCENT : fixture_case == 5 || fixture_case == 7 || fixture_case == 17 ? CUVS_HNSW_ALGO_ACE : fixture_case == 6 ? CUVS_HNSW_ALGO_IVF_PQ : CUVS_HNSW_ALGO_AUTO; }
static volatile int idle_calls, idle_limit, idle_done, reply_sent, mock_checked;
/* Ordering waits between the mock and the idle unit. A missing event fails
 * with a case-specific reason instead of hanging until an outer timeout. */
/* macOS poll(0) costs ~9 us: case 23's 1,000,000 idle polls take ~10 s there, 0.2 s on Linux. */
#define SYNC_WAIT_LIMIT_S 60
#define SYNC_WAIT(cond) do { \
    struct timespec t0_, t_; clock_gettime(CLOCK_MONOTONIC, &t0_); \
    while (!(cond)) { \
        clock_gettime(CLOCK_MONOTONIC, &t_); \
        if (t_.tv_sec - t0_.tv_sec >= SYNC_WAIT_LIMIT_S) { \
            fprintf(stderr, "ipc case %d sync wait timed out: %s\n", fixture_case, #cond); \
            fflush(stderr); abort(); } \
        sched_yield(); } } while (0)
/* K3 idle work: one unit per empty poll; cases 25-29. No sleeps or timing:
 * case 25: the mock replies only after the backend has finished all work;
 * case 26: the first unit blocks until the mock has sent its reply, so the
 *          next poll must take the reply while work remains;
 * case 27: the 5th unit blocks until the mock has checked the request, then
 *          the wait callback cancels before the next poll;
 * case 29: 1,000,000 cheap units (> the 864000-iteration wait limit) before
 *          the mock replies; idle units must not count toward that limit. */
static int idle_work(void *arg) {
    (void)arg;
    idle_calls++;
    if (fixture_case == 26 && idle_calls == 1)
        SYNC_WAIT(__atomic_load_n(&reply_sent, __ATOMIC_ACQUIRE));
    if (fixture_case == 27 && idle_calls == 5)
        SYNC_WAIT(__atomic_load_n(&mock_checked, __ATOMIC_ACQUIRE));
    if (idle_calls >= idle_limit) { __atomic_store_n(&idle_done, 1, __ATOMIC_RELEASE); return 0; }
    return 1;
}
static int cancel_wait(void) { return (fixture_case == 1 && ++polls >= 3) || (fixture_case == 27 && idle_calls >= 5); }
static void *daemon_mock(void *unused) {
    (void)unused;
    int c = accept(listener, 0, 0), vectors = -1, owner = -1;
    CuvsCmdFrame cmd;
    CuvsHnswFileRequest req;
    char token;
    assert(c >= 0); (*accepts)++;
    assert(recv(c, &cmd, sizeof(cmd), MSG_WAITALL) == sizeof(cmd));
    assert(cmd.proto_version == CUVS_PROTO_VERSION && cmd.op == CUVS_OP_BUILD_HNSW_FILE);
    struct timeval timeout = {1, 0};
    assert(!setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    char error[128] = {0};
    int request_status = cuvs_hnsw_receive_file_request(c, &req, &vectors, &owner, error, sizeof(error));
    if (fixture_case == 13 || fixture_case == 14) {
        assert(request_status == (fixture_case == 13 ? CUVS_STATUS_PROTO_MISMATCH : CUVS_STATUS_BUILD_FAILED));
        CuvsHnswFileReply reply = {0}; reply.header.status = request_status;
        snprintf(reply.header.error, sizeof(reply.header.error), "%s", error);
        assert(write(c, "R", 1) == 1 && !cuvs_fd_send(c, -1, &reply, sizeof(reply)));
        if (vectors >= 0) close(vectors);
        if (owner >= 0) close(owner);
        close(c); return NULL;
    }
    assert(request_status == CUVS_STATUS_OK);
    assert(req.version == CUVS_HNSW_FILE_VERSION && req.version == 6 && req.host_budget == (1ULL << 30));
    assert(req.build_algo == requested_algo() && req.ef_construction == 64);
    assert(cuvs_hnsw_file_request_supported(&req));
    assert(cuvs_hnsw_regular_disk(vectors));
    float first = 0;
    assert(pread(vectors, &first, sizeof(first), 0) == sizeof(first) && first == -1.25f);
    if (fixture_case != 1) assert(!cuvs_hnsw_owner_cancelled(owner));
    if (fixture_case == 1 || fixture_case == 27) {
        struct pollfd p = {.fd = owner, .events = POLLIN};
        __atomic_store_n(&mock_checked, 1, __ATOMIC_RELEASE);
        assert(poll(&p, 1, 3000) > 0);
        assert(cuvs_hnsw_owner_cancelled(owner));
    } else if (fixture_case == 3 || fixture_case == 11 || fixture_case == 12 || fixture_case == 28) {
        CuvsHnswFileReply reply = {.header = {.status = CUVS_STATUS_BUILD_FAILED}};
        if (fixture_case == 11) reply.header.status = CUVS_STATUS_PROTO_MISMATCH;
        if (fixture_case == 12) reply.header.status = CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED;
        if (fixture_case == 28) reply.header.status = CUVS_STATUS_CANCELED; /* daemon-side cancel */
        snprintf(reply.header.error, sizeof(reply.header.error), "synthetic capacity failure");
        /* The decision bytes travel after the receipt on every reply. */
        reply.budget = (CuvsHnswBudgetSnapshot){11, 22, 33, 44, 55, 66 + (uint64_t)fixture_case};
        assert(write(c, "R", 1) == 1);
        assert(cuvs_fd_send(c, -1, &reply, sizeof(reply)) == 0);
    } else {
        char path[] = "/tmp/pgcuvs-core-reply-XXXXXX";
        if (fixture_case == 25 || fixture_case == 29) /* reply only after the backend's idle work */
            SYNC_WAIT(__atomic_load_n(&idle_done, __ATOMIC_ACQUIRE));
        if (fixture_case == 26) /* reply only once the backend is inside its first idle unit */
            SYNC_WAIT(__atomic_load_n(&idle_calls, __ATOMIC_ACQUIRE) >= 1);
        int writable = mkstemp(path);
        assert(writable >= 0);
        assert(write(writable, "serialized-hnsw", 15) == 15);
        int read_only = open(path, fixture_case == 2 ? O_RDWR : O_RDONLY);
        assert(read_only >= 0); unlink(path); close(writable);
        CuvsHnswFileReply reply = {.header = {.status = CUVS_STATUS_OK}};
        reply.policy.magic = CUVS_HNSW_POLICY_MAGIC;
        reply.policy.version = fixture_case == 4 ? 99 : fixture_case == 9 ? CUVS_HNSW_POLICY_LEGACY_VERSION : CUVS_HNSW_POLICY_VERSION;
        reply.policy.bytes = sizeof(reply.policy);
        reply.policy.route = fixture_case == 5 || fixture_case == 8 || fixture_case == 17 ? CUVS_HNSW_ROUTE_ACE : CUVS_HNSW_ROUTE_IVF_PQ;
        reply.policy.requested_algo = fixture_case == 10 ? CUVS_HNSW_ALGO_IVF_PQ : req.build_algo;
        reply.policy.requested_ef = req.ef_construction;
        reply.policy.guard_passed = 1;
        reply.policy.n_rows = fixture_case == 15 ? (uint64_t)cmd.n_vecs + 1 : (uint64_t)cmd.n_vecs;
        reply.policy.dim = cmd.dim; reply.policy.metric = cmd.metric;
        reply.policy.hierarchy = fixture_case == 16 ? !req.hierarchy : req.hierarchy;
        reply.policy.internal_builder = reply.policy.route;
        reply.policy.partitions = reply.policy.route == CUVS_HNSW_ROUTE_ACE ? 2 : 0;
        reply.policy.degree = 64; reply.policy.intermediate = 128; reply.policy.ace_ef = 120;
        /* v5: an ACE receipt travels with its partition-builder readback; case 17 omits it. */
        if (reply.policy.route == CUVS_HNSW_ROUTE_ACE && fixture_case != 17)
            cuvs_hnsw_ace_readback_fill(&reply.ace, CUVS_HNSW_ACE_QUALITY_AUTO, 120, 64,
                reply.policy.n_rows, 2, 20);
        /* case 18: a resident receipt must not carry a readback tail. */
        if (fixture_case == 18)
            cuvs_hnsw_ace_readback_fill(&reply.ace, CUVS_HNSW_ACE_QUALITY_AUTO, 120, 64,
                reply.policy.n_rows, 2, 20);
        if (fixture_case >= 19 && fixture_case <= 24) {
            reply.policy.version = CUVS_HNSW_POLICY_NN_VERSION;
            reply.policy.route = CUVS_HNSW_ROUTE_NN_DESCENT;
            reply.policy.internal_builder = CUVS_HNSW_ROUTE_NN_DESCENT;
            reply.policy.intermediate = 96; reply.policy.ace_ef = 0;
            reply.nn.magic = CUVS_HNSW_NN_READBACK_MAGIC; reply.nn.version = 1;
            reply.nn.bytes = sizeof(reply.nn); reply.nn.graph_degree = 96; reply.nn.internal_degree = 99;
            reply.nn.sdk_default_iterations = 20; reply.nn.max_iterations = 20;
            reply.nn.dtype = CUVS_HNSW_NN_DTYPE_FP32; reply.nn.termination_threshold = 0.0001f;
            if (fixture_case == 20) reply.policy.internal_builder = CUVS_HNSW_ROUTE_IVF_PQ;
            if (fixture_case == 21) reply.nn.max_iterations = 21;
            if (fixture_case == 22) reply.policy.route = CUVS_HNSW_ROUTE_IVF_PQ;
            if (fixture_case == 23) reply.policy.training_actual = 10;
        }
        assert(write(c, "R", 1) == 1);
        if (fixture_case == 24) {
            reply.header.status = CUVS_STATUS_PROTO_MISMATCH;
            assert(cuvs_fd_send(c, -1, &reply, sizeof(reply) - sizeof(reply.nn)) == 0);
            close(read_only); close(vectors); close(owner); close(c); return NULL;
        }
        assert(cuvs_fd_send(c, read_only, &reply, sizeof(reply)) == 0);
        __atomic_store_n(&reply_sent, 1, __ATOMIC_RELEASE);
        close(read_only);
        if (fixture_case == 2 || fixture_case == 4 || (fixture_case >= 7 && fixture_case <= 10) || (fixture_case >= 15 && fixture_case <= 24 && fixture_case != 19))
            assert(recv(owner, &token, 1, 0) == 0); // invalid FD/receipt/readback rejected, no ACK
        else
            assert(recv(owner, &token, 1, 0) == 1 && token == 'A');
    }
    close(vectors); close(owner); close(c);
    return 0;
}
int main(void) {
    char dir[] = "/tmp/pgcuvs-core-ipc-XXXXXX", path[200];
    char input[] = "/tmp/pgcuvs-core-input-XXXXXX";
    accepts = mmap(NULL, sizeof(*accepts), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(accepts != MAP_FAILED); *accepts = 0;
    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/socket", dir);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listener, 2) == 0);
    int fd = mkstemp(input); assert(fd >= 0); unlink(input);
    float first = -1.25f;
    assert(write(fd, &first, sizeof(first)) == sizeof(first));
    assert(ftruncate(fd, 100 * 128 * sizeof(float)) == 0);
    cuvs_ipc_set_wait_callback(cancel_wait);
    for (int sequence = 0; sequence < 23; sequence++) {
        fixture_case = sequence < 13 ? sequence : sequence + 2;
        int child_status;
        pid_t child;
        int result = -1;
        CuvsHnswPolicy policy;
        CuvsHnswAceReadback ace;
        CuvsHnswNnReadback nn;
        CuvsHnswBudgetSnapshot budget;
        char err[128], bytes[16] = {0};
        polls = 0;
        child = fork(); assert(child >= 0);
        if (child == 0) { daemon_mock(NULL); _exit(0); }
        int rc = cuvs_ipc_build_hnsw_file(path, fd, 100, 128, CUVS_METRIC_L2,
            64, 128, 0, 1ULL << 30, requested_algo(), 64, &policy, &budget, &ace, &nn, &result, err, sizeof(err));
        if (fixture_case == 1) { assert(rc == CUVS_STATUS_CANCELED && result == -1); }
        else if (fixture_case == 2 || fixture_case == 4 || (fixture_case >= 7 && fixture_case <= 10) || (fixture_case >= 15 && fixture_case <= 24 && fixture_case != 19)) { assert(rc == CUVS_STATUS_ERROR && result == -1); if (fixture_case == 24) assert(strstr(err, "co-deploy")); }
        else if (fixture_case == 3 || fixture_case == 11 || fixture_case == 12) {
            int expected_status = fixture_case == 11 ? CUVS_STATUS_PROTO_MISMATCH : fixture_case == 12 ? CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED : CUVS_STATUS_BUILD_FAILED;
            assert(rc == expected_status && result == -1);
            assert(strcmp(err, "synthetic capacity failure") == 0);
            /* status 13 (case 12) DETAIL inputs reach the backend unchanged */
            assert(budget.host_estimate == 11 && budget.host_budget == 22
                   && budget.gpu_estimate == 33 && budget.gpu_budget == 44
                   && budget.pool_used == 55 && budget.logical_used == 66 + (uint64_t)fixture_case);
        }
        else {
            assert(rc == CUVS_STATUS_OK && result >= 0);
            assert(cuvs_hnsw_policy_valid(&policy));
            if (policy.route == CUVS_HNSW_ROUTE_ACE)
                assert(cuvs_hnsw_ace_readback_valid(&ace, &policy) && ace.local_intermediate == 96
                       && ace.quality == 7 && ace.local_iterations == 12);
            else
                assert(ace.magic == 0);
            if (fixture_case == 19)
                assert(cuvs_hnsw_nn_readback_valid(&nn, &policy) && nn.max_iterations == 20);
            assert((fcntl(result, F_GETFL) & O_ACCMODE) == O_RDONLY);
            assert(read(result, bytes, 15) == 15 && strcmp(bytes, "serialized-hnsw") == 0);
            close(result);
        }
        assert(waitpid(child, &child_status, 0) == child);
        assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    }
    /* Actual client must reject unknown algo BEFORE uds_connect.
     * An unavailable path would otherwise return UNAVAILABLE, not ERROR. */
    for (unsigned i = 0; i < 4; i++) {
        int result = -1; CuvsHnswPolicy p; CuvsHnswAceReadback a; CuvsHnswBudgetSnapshot b = {1, 1, 1, 1, 1, 1}; char err[128];
        uint32_t algo = i == 0 ? 99u : i == 1 ? UINT32_MAX : CUVS_HNSW_ALGO_AUTO;
        uint32_t ef = i == 2 ? 3 : i == 3 ? 1001 : 64;
        int before = *accepts;
        int rc = cuvs_ipc_build_hnsw_file("/nonexistent/never-connect", fd, 100, 128,
            CUVS_METRIC_L2, 64, 128, 0, 1ULL << 30, algo, ef, &p, &b, &a, NULL, &result, err, sizeof(err));
        assert(rc == CUVS_STATUS_ERROR && result == -1 && p.magic == 0 && *accepts == before);
        assert(b.host_estimate == 0 && b.pool_used == 0 && b.logical_used == 0); /* zeroed, no reply */
    }
    assert(*accepts == 23);
    /* Directly exercise the daemon's shared production prefix consumer.
     * Legacy/future send only16 bytes: it must reject without reading a tail. */
    for (unsigned k = 0; k < 5; k++) {
        fixture_case = k < 3 ? 13 : 14;
        int child_status; pid_t child = fork(); assert(child >= 0);
        if (child == 0) { daemon_mock(NULL); _exit(0); }
        int c = socket(AF_UNIX, SOCK_STREAM, 0); assert(c >= 0);
        assert(!connect(c, (struct sockaddr *)&addr, sizeof(addr)));
        CuvsCmdFrame cmd = {0}; cmd.proto_magic = CUVS_PROTO_MAGIC; cmd.proto_version = CUVS_PROTO_VERSION;
        cmd.op = CUVS_OP_BUILD_HNSW_FILE;
        assert(write(c, &cmd, sizeof(cmd)) == sizeof(cmd));
        int channel[2] = {-1, -1};
        if (k < 3) {
            struct {uint32_t version, hierarchy; uint64_t host;} prefix = {k == 0 ? 1 : k == 1 ? 5 : 99, 0, 1ULL << 30};
            assert(!cuvs_fd_send(c, fd, &prefix, sizeof(prefix)));
        } else {
            CuvsHnswFileRequest req = {CUVS_HNSW_FILE_VERSION, 0, 1ULL << 30, k == 3 ? 99u : UINT32_MAX, 64};
            assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, channel));
            assert(!cuvs_fd_send(c, fd, &req, sizeof(req)));
            char token = 'O'; assert(!cuvs_fd_send(c, channel[1], &token, 1));
        }
        char token; int returned = -1; CuvsHnswFileReply reply;
        assert(read(c, &token, 1) == 1 && token == 'R');
        assert(!cuvs_fd_recv(c, &reply, sizeof(reply), &returned) && returned == -1);
        assert(reply.header.status == (k < 3 ? CUVS_STATUS_PROTO_MISMATCH : CUVS_STATUS_BUILD_FAILED));
        if (k < 3) assert(strstr(reply.header.error, "payload version mismatch"));
        close(c); if (channel[0] >= 0) {close(channel[0]); close(channel[1]);}
        assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status)
               && WEXITSTATUS(child_status) == 0 && fcntl(fd, F_GETFD) >= 0);
    }
    assert(*accepts == 28);
    /* K3: idle work runs while the reply is pending, never delays reading a
     * ready reply, and does not hide a cancel. */
    for (int k = 25; k <= 29; k++) {
        pthread_t t; int result = -1; CuvsHnswPolicy p; CuvsHnswAceReadback a; CuvsHnswBudgetSnapshot b; char err[128], bytes[16] = {0};
        fixture_case = k; idle_calls = 0; polls = 0; idle_done = 0; reply_sent = 0; mock_checked = 0;
        idle_limit = k == 26 || k == 27 || k == 29 ? 1000000 : 100;
        assert(pthread_create(&t, 0, daemon_mock, 0) == 0);
        int rc = cuvs_ipc_build_hnsw_file_idle(path, fd, 100, 128, CUVS_METRIC_L2, 64, 128, 0, 1ULL << 30,
            requested_algo(), 64, &p, &b, &a, NULL, &result, err, sizeof(err), idle_work, NULL);
        if (k == 27) assert(rc == CUVS_STATUS_CANCELED && result == -1 && idle_calls == 5
                            && cuvs_ipc_hnsw_backend_cancelled() == 1);
        /* A daemon reply with status CANCELED is not a backend interrupt. */
        else if (k == 28) assert(rc == CUVS_STATUS_CANCELED && result == -1 && cuvs_ipc_hnsw_backend_cancelled() == 0);
        else {
            assert(rc == CUVS_STATUS_OK && result >= 0 && cuvs_ipc_hnsw_backend_cancelled() == 0);
            assert(read(result, bytes, 15) == 15 && strcmp(bytes, "serialized-hnsw") == 0);
            close(result);
            if (k == 25) assert(idle_calls == 100);         /* all work done before the reply */
            else if (k == 26) assert(idle_calls == 1 && !idle_done); /* reply taken while work remained */
            else assert(idle_calls == 1000000);             /* M1: idle units are not wait-limit ticks */
        }
        printf("K3 idle case %d: rc=%d idle_calls=%d\n", k, rc, idle_calls);
        assert(pthread_join(t, 0) == 0);
    }
    assert(*accepts == 33);
    close(fd); close(listener); unlink(path); rmdir(dir);
    assert(munmap(accepts, sizeof(*accepts)) == 0);
    printf("PASS 23 actual-client wire cases (v6 ACE readback bound, none on resident),4 preconnect refusals,5 shared-daemon prefix/settings cases,5 K3 idle-wait cases; explicitACE/IVF, forgedroute/version/request rejection, cancellation/status13/protocolerror; current global v%u retained\n", CUVS_PROTO_VERSION);
}
