#define _GNU_SOURCE 1
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "cuvs_ipc.h"
#include "cuvs_build_corpus.h"
#include "cuvs_hnsw_build_common.h"
#include <assert.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/time.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int listener, fixture_case, polls, accepts;
static uint32_t requested_algo(void) { return fixture_case == 5 || fixture_case == 7 ? CUVS_HNSW_ALGO_ACE : fixture_case == 6 ? CUVS_HNSW_ALGO_IVF_PQ : CUVS_HNSW_ALGO_AUTO; }
static int cancel_wait(void) { return fixture_case == 1 && ++polls >= 3; }
static void *daemon_mock(void *unused) {
    (void)unused;
    int c = accept(listener, 0, 0), vectors = -1, owner = -1;
    CuvsCmdFrame cmd;
    CuvsHnswFileRequest req;
    char token;
    assert(c >= 0); accepts++;
    assert(recv(c, &cmd, sizeof(cmd), MSG_WAITALL) == sizeof(cmd));
    assert(cmd.proto_version == 3 && cmd.op == CUVS_OP_BUILD_HNSW_FILE);
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
    assert(req.version == CUVS_HNSW_FILE_VERSION && req.version == 3 && req.host_budget == (1ULL << 30));
    assert(req.build_algo == requested_algo() && req.ef_construction == 64);
    assert(cuvs_hnsw_file_request_supported(&req));
    assert(cuvs_hnsw_regular_disk(vectors));
    float first = 0;
    assert(pread(vectors, &first, sizeof(first), 0) == sizeof(first) && first == -1.25f);
    assert(!cuvs_hnsw_owner_cancelled(owner));
    if (fixture_case == 1) {
        struct pollfd p = {.fd = owner, .events = POLLIN};
        assert(poll(&p, 1, 3000) > 0);
        assert(cuvs_hnsw_owner_cancelled(owner));
    } else if (fixture_case == 3 || fixture_case == 11 || fixture_case == 12) {
        CuvsHnswFileReply reply = {.header = {.status = CUVS_STATUS_BUILD_FAILED}};
        if (fixture_case == 11) reply.header.status = CUVS_STATUS_PROTO_MISMATCH;
        if (fixture_case == 12) reply.header.status = CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED;
        snprintf(reply.header.error, sizeof(reply.header.error), "synthetic capacity failure");
        assert(write(c, "R", 1) == 1);
        assert(cuvs_fd_send(c, -1, &reply, sizeof(reply)) == 0);
    } else {
        char path[] = "/tmp/pgcuvs-core-reply-XXXXXX";
        int writable = mkstemp(path);
        assert(writable >= 0);
        assert(write(writable, "serialized-hnsw", 15) == 15);
        int read_only = open(path, fixture_case == 2 ? O_RDWR : O_RDONLY);
        assert(read_only >= 0); unlink(path); close(writable);
        CuvsHnswFileReply reply = {.header = {.status = CUVS_STATUS_OK}};
        reply.policy.magic = CUVS_HNSW_POLICY_MAGIC;
        reply.policy.version = fixture_case == 4 ? 99 : fixture_case == 9 ? CUVS_HNSW_POLICY_LEGACY_VERSION : CUVS_HNSW_POLICY_VERSION;
        reply.policy.bytes = sizeof(reply.policy);
        reply.policy.route = fixture_case == 5 || fixture_case == 8 ? CUVS_HNSW_ROUTE_ACE : CUVS_HNSW_ROUTE_IVF_PQ;
        reply.policy.requested_algo = fixture_case == 10 ? CUVS_HNSW_ALGO_IVF_PQ : req.build_algo;
        reply.policy.requested_ef = req.ef_construction;
        reply.policy.guard_passed = 1;
        reply.policy.n_rows = fixture_case == 15 ? (uint64_t)cmd.n_vecs + 1 : (uint64_t)cmd.n_vecs;
        reply.policy.dim = cmd.dim; reply.policy.metric = cmd.metric;
        reply.policy.hierarchy = fixture_case == 16 ? !req.hierarchy : req.hierarchy;
        reply.policy.internal_builder = reply.policy.route;
        reply.policy.partitions = reply.policy.route == CUVS_HNSW_ROUTE_ACE ? 2 : 0;
        assert(write(c, "R", 1) == 1);
        assert(cuvs_fd_send(c, read_only, &reply, sizeof(reply)) == 0);
        close(read_only);
        if (fixture_case == 2 || fixture_case == 4 || (fixture_case >= 7 && fixture_case <= 10) || fixture_case >= 15)
            assert(recv(owner, &token, 1, 0) == 0); // invalid FD rejected, no ACK
        else
            assert(recv(owner, &token, 1, 0) == 1 && token == 'A');
    }
    close(vectors); close(owner); close(c);
    return 0;
}
int main(void) {
    char dir[] = "/tmp/pgcuvs-core-ipc-XXXXXX", path[200];
    char input[] = "/tmp/pgcuvs-core-input-XXXXXX";
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
    for (int sequence = 0; sequence < 15; sequence++) {
        fixture_case = sequence < 13 ? sequence : sequence + 2;
        pthread_t t;
        int result = -1;
        CuvsHnswPolicy policy;
        char err[128], bytes[16] = {0};
        polls = 0;
        assert(pthread_create(&t, 0, daemon_mock, 0) == 0);
        int rc = cuvs_ipc_build_hnsw_file(path, fd, 100, 128, CUVS_METRIC_L2,
            64, 128, 0, 1ULL << 30, requested_algo(), 64, &policy, &result, err, sizeof(err));
        if (fixture_case == 1) { assert(rc == CUVS_STATUS_CANCELED && result == -1); }
        else if (fixture_case == 2 || fixture_case == 4 || (fixture_case >= 7 && fixture_case <= 10) || fixture_case >= 15) { assert(rc == CUVS_STATUS_ERROR && result == -1); }
        else if (fixture_case == 3 || fixture_case == 11 || fixture_case == 12) {
            int expected_status = fixture_case == 11 ? CUVS_STATUS_PROTO_MISMATCH : fixture_case == 12 ? CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED : CUVS_STATUS_BUILD_FAILED;
            assert(rc == expected_status && result == -1);
            assert(strcmp(err, "synthetic capacity failure") == 0);
        }
        else {
            assert(rc == CUVS_STATUS_OK && result >= 0);
            assert(cuvs_hnsw_policy_valid(&policy));
            assert((fcntl(result, F_GETFL) & O_ACCMODE) == O_RDONLY);
            assert(read(result, bytes, 15) == 15 && strcmp(bytes, "serialized-hnsw") == 0);
            close(result);
        }
        assert(pthread_join(t, 0) == 0);
    }
    /* Actual client must reject reserved/unknown algo BEFORE uds_connect.
     * An unavailable path would otherwise return UNAVAILABLE, not ERROR. */
    for (unsigned i = 0; i < 4; i++) {
        int result = -1; CuvsHnswPolicy p; char err[128];
        uint32_t algo = i == 0 ? CUVS_HNSW_ALGO_NN_DESCENT : i == 1 ? UINT32_MAX : CUVS_HNSW_ALGO_AUTO;
        uint32_t ef = i == 2 ? 3 : i == 3 ? 1001 : 64;
        int before = accepts;
        int rc = cuvs_ipc_build_hnsw_file("/nonexistent/never-connect", fd, 100, 128,
            CUVS_METRIC_L2, 64, 128, 0, 1ULL << 30, algo, ef, &p, &result, err, sizeof(err));
        assert(rc == CUVS_STATUS_ERROR && result == -1 && p.magic == 0 && accepts == before);
    }
    assert(accepts == 15);
    /* Directly exercise the daemon's shared production prefix consumer.
     * Legacy/future send only16 bytes: it must reject without reading a tail. */
    for (unsigned k = 0; k < 5; k++) {
        fixture_case = k < 3 ? 13 : 14;
        pthread_t t; assert(!pthread_create(&t, NULL, daemon_mock, NULL));
        int c = socket(AF_UNIX, SOCK_STREAM, 0); assert(c >= 0);
        assert(!connect(c, (struct sockaddr *)&addr, sizeof(addr)));
        CuvsCmdFrame cmd = {0}; cmd.proto_magic = CUVS_PROTO_MAGIC; cmd.proto_version = CUVS_PROTO_VERSION;
        cmd.op = CUVS_OP_BUILD_HNSW_FILE;
        assert(write(c, &cmd, sizeof(cmd)) == sizeof(cmd));
        int channel[2] = {-1, -1};
        if (k < 3) {
            struct {uint32_t version, hierarchy; uint64_t host;} prefix = {k == 0 ? 1 : k == 1 ? 2 : 99, 0, 1ULL << 30};
            assert(!cuvs_fd_send(c, fd, &prefix, sizeof(prefix)));
        } else {
            CuvsHnswFileRequest req = {3, 0, 1ULL << 30, k == 3 ? CUVS_HNSW_ALGO_NN_DESCENT : UINT32_MAX, 64};
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
        assert(!pthread_join(t, NULL) && fcntl(fd, F_GETFD) >= 0);
    }
    assert(accepts == 20);
    close(fd); close(listener); unlink(path); rmdir(dir);
    puts("PASS 15 actual-client wire cases,4 preconnect refusals,5 shared-daemon prefix/settings cases; explicitACE/IVF, forgedroute/version/request rejection, cancellation/status13/protocolerror; global v3 retained");
}
