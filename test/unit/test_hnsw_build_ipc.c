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
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int listener, fixture_case, polls;
static int cancel_wait(void) { return fixture_case == 1 && ++polls >= 3; }
static void *daemon_mock(void *unused) {
    (void)unused;
    int c = accept(listener, 0, 0), vectors = -1, owner = -1;
    CuvsCmdFrame cmd;
    CuvsHnswFileRequest req;
    char token;
    assert(c >= 0);
    assert(recv(c, &cmd, sizeof(cmd), MSG_WAITALL) == sizeof(cmd));
    assert(cmd.proto_version == 3 && cmd.op == CUVS_OP_BUILD_HNSW_FILE);
    assert(cuvs_fd_recv(c, &req, sizeof(req), &vectors) == 0);
    assert(req.version == 2 && req.host_budget == (1ULL << 30));
    assert(req.build_algo == CUVS_HNSW_ALGO_AUTO && req.ef_construction == 64);
    assert(cuvs_fd_recv(c, &token, 1, &owner) == 0);
    assert(cuvs_hnsw_regular_disk(vectors));
    float first = 0;
    assert(pread(vectors, &first, sizeof(first), 0) == sizeof(first) && first == -1.25f);
    assert(!cuvs_hnsw_owner_cancelled(owner));
    if (fixture_case == 1) {
        struct pollfd p = {.fd = owner, .events = POLLIN};
        assert(poll(&p, 1, 3000) > 0);
        assert(cuvs_hnsw_owner_cancelled(owner));
    } else if (fixture_case == 3) {
        CuvsHnswFileReply reply = {.header = {.status = CUVS_STATUS_BUILD_FAILED}};
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
        reply.policy.version = fixture_case == 4 ? 99 : CUVS_HNSW_POLICY_VERSION;
        reply.policy.bytes = sizeof(reply.policy);
        reply.policy.route = CUVS_HNSW_ROUTE_IVF_PQ;
        reply.policy.requested_algo = req.build_algo;
        reply.policy.requested_ef = req.ef_construction;
        reply.policy.guard_passed = 1;
        assert(write(c, "R", 1) == 1);
        assert(cuvs_fd_send(c, read_only, &reply, sizeof(reply)) == 0);
        close(read_only);
        if (fixture_case == 2 || fixture_case == 4)
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
    for (fixture_case = 0; fixture_case <= 4; fixture_case++) {
        pthread_t t;
        int result = -1;
        CuvsHnswPolicy policy;
        char err[128], bytes[16] = {0};
        polls = 0;
        assert(pthread_create(&t, 0, daemon_mock, 0) == 0);
        int rc = cuvs_ipc_build_hnsw_file(path, fd, 100, 128, CUVS_METRIC_L2,
            64, 128, 0, 1ULL << 30, CUVS_HNSW_ALGO_AUTO, 64, &policy, &result, err, sizeof(err));
        if (fixture_case == 1) { assert(rc == CUVS_STATUS_CANCELED && result == -1); }
        else if (fixture_case == 2 || fixture_case == 4) { assert(rc == CUVS_STATUS_ERROR && result == -1); }
        else if (fixture_case == 3) {
            assert(rc == CUVS_STATUS_BUILD_FAILED && result == -1);
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
    close(fd); close(listener); unlink(path); rmdir(dir);
    puts("PASS FD request/reply, read-only unlinked result/ACK, cancellation, writable-result rejection, capacity error; v3 retained");
}
