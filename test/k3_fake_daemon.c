/*
 * k3_fake_daemon.c - scripted stand-in for pg_cuvs_server, HNSW build only.
 * Used by test/k3_digest_integration.py (make test-k3-digest).
 *
 * Usage: k3_fake_daemon SOCKET STATEDIR
 * Each CUVS_OP_BUILD_HNSW_FILE request reads STATEDIR/mode at accept time:
 *   fail       reply BUILD_FAILED "k3 fake build failure"
 *   cancel     reply status CANCELED (a daemon-side cancel, no backend interrupt)
 *   ok         reply OK with a valid resident receipt and a read-only file
 *   nn-ok      reply OK with an NN-descent receipt, its NN readback, and a
 *              convertible flat hnswlib file (a ring) over the request's rows
 *   hold       never reply; return when the backend closes or cancels
 *   gate-fail  wait for STATEDIR/release, then reply as "fail"
 * Every other op (route preflight, ...) is closed unanswered, which the
 * backend treats as a failed forecast and falls back to the build op.
 * Each accepted build appends "build mode=<mode>" to STATEDIR/events.
 */
#define _GNU_SOURCE 1
#include "cuvs_ipc.h"
#include "cuvs_hnsw_build_common.h"
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static const char *state;

static void read_mode(char *mode, size_t len)
{
    char path[4096];
    FILE *f;
    snprintf(path, sizeof(path), "%s/mode", state);
    f = fopen(path, "r");
    mode[0] = '\0';
    if (f) { if (fgets(mode, (int)len, f)) mode[strcspn(mode, "\r\n ")] = '\0'; fclose(f); }
}

static void event(const char *mode)
{
    char path[4096];
    FILE *f;
    snprintf(path, sizeof(path), "%s/events", state);
    f = fopen(path, "a");
    if (f) { fprintf(f, "build mode=%s\n", mode); fclose(f); }
}

static int peer_closed(int c, int owner)
{
    struct pollfd p[2] = {{.fd = c, .events = POLLIN}, {.fd = owner, .events = POLLIN}};
    return poll(p, 2, 20) > 0;
}

static void reply_status(int c, uint32_t status, const char *error)
{
    CuvsHnswFileReply reply;
    memset(&reply, 0, sizeof(reply));
    reply.header.status = status;
    snprintf(reply.header.error, sizeof(reply.header.error), "%s", error);
    if (write(c, "R", 1) == 1) (void)cuvs_fd_send(c, -1, &reply, sizeof(reply));
}

static void reply_ok(int c, int owner, const CuvsCmdFrame *cmd, const CuvsHnswFileRequest *req)
{
    char path[4096], token;
    CuvsHnswFileReply reply;
    int w, ro;
    snprintf(path, sizeof(path), "%s/fake-hnsw-XXXXXX", state);
    w = mkstemp(path);
    if (w < 0) return;
    if (write(w, "k3-fake-hnsw", 12) != 12) { close(w); return; }
    ro = open(path, O_RDONLY);
    unlink(path); close(w);
    if (ro < 0) return;
    memset(&reply, 0, sizeof(reply));
    reply.header.status = CUVS_STATUS_OK;
    reply.policy.magic = CUVS_HNSW_POLICY_MAGIC;
    reply.policy.version = CUVS_HNSW_POLICY_VERSION;
    reply.policy.bytes = sizeof(reply.policy);
    reply.policy.route = CUVS_HNSW_ROUTE_IVF_PQ;
    reply.policy.requested_algo = req->build_algo;
    reply.policy.requested_ef = req->ef_construction;
    reply.policy.guard_passed = 1;
    reply.policy.n_rows = (uint64_t)cmd->n_vecs;
    reply.policy.dim = cmd->dim;
    reply.policy.metric = cmd->metric;
    reply.policy.hierarchy = req->hierarchy;
    reply.policy.internal_builder = reply.policy.route;
    reply.policy.degree = 64; reply.policy.intermediate = 128; reply.policy.ace_ef = 120;
    if (write(c, "R", 1) == 1 && cuvs_fd_send(c, ro, &reply, sizeof(reply)) == 0)
    {
        struct pollfd p = {.fd = owner, .events = POLLIN};
        if (poll(&p, 1, 5000) > 0) (void)recv(owner, &token, 1, 0);
    }
    close(ro);
}

/* A flat hnswlib image over the request's float32 rows: node i links to its
 * two ring neighbours, label i, no upper levels. Returns a read-only fd. */
static int flat_hnsw_file(int vectors, uint64_t n, uint32_t dim)
{
    struct {
        size_t offsetLevel0, maxElements, curElementCount, sizeDataPerElement,
               labelOffset, offsetData;
        int maxlevel, enterpointNode;
        size_t maxM, maxM0, M;
        double mult;
        size_t efConstruction;
    } h;
    char path[4096];
    size_t row_bytes;
    unsigned char *row;
    uint32_t no_upper = 0;
    int w, ro = -1;
    memset(&h, 0, sizeof(h));
    h.maxM0 = 2; h.maxM = 1; h.M = 1; h.mult = 1.0; h.efConstruction = 100;
    h.offsetData = sizeof(int) + h.maxM0 * sizeof(int);
    h.labelOffset = h.offsetData + (size_t)dim * sizeof(float);
    h.sizeDataPerElement = h.labelOffset + sizeof(size_t);
    h.maxElements = h.curElementCount = n;
    row_bytes = h.sizeDataPerElement;
    row = calloc(1, row_bytes);
    snprintf(path, sizeof(path), "%s/fake-nn-hnsw-XXXXXX", state);
    w = mkstemp(path);
    if (!row || w < 0) goto out;
    if (write(w, &h, sizeof(h)) != (ssize_t)sizeof(h)) goto out;
    for (uint64_t i = 0; i < n; i++)
    {
        int count = 2, links[2] = {(int)((i + 1) % n), (int)((i + n - 1) % n)};
        size_t label = (size_t)i;
        memcpy(row, &count, sizeof(count));
        memcpy(row + sizeof(int), links, sizeof(links));
        if (pread(vectors, row + h.offsetData, (size_t)dim * sizeof(float),
                  (off_t)(i * dim * sizeof(float))) != (ssize_t)(dim * sizeof(float))) goto out;
        memcpy(row + h.labelOffset, &label, sizeof(label));
        if (write(w, row, row_bytes) != (ssize_t)row_bytes) goto out;
    }
    for (uint64_t i = 0; i < n; i++)
        if (write(w, &no_upper, sizeof(no_upper)) != (ssize_t)sizeof(no_upper)) goto out;
    ro = open(path, O_RDONLY);
out:
    if (w >= 0) { unlink(path); close(w); }
    free(row);
    return ro;
}

static void reply_nn_ok(int c, int owner, int vectors, const CuvsCmdFrame *cmd,
                        const CuvsHnswFileRequest *req)
{
    CuvsHnswFileReply reply;
    char token;
    uint64_t n = (uint64_t)cmd->n_vecs;
    int ro = flat_hnsw_file(vectors, n, cmd->dim);
    if (ro < 0) { reply_status(c, CUVS_STATUS_BUILD_FAILED, "k3 fake nn file failure"); return; }
    memset(&reply, 0, sizeof(reply));
    reply.header.status = CUVS_STATUS_OK;
    reply.policy.magic = CUVS_HNSW_POLICY_MAGIC;
    reply.policy.version = CUVS_HNSW_POLICY_NN_VERSION;
    reply.policy.bytes = sizeof(reply.policy);
    reply.policy.route = CUVS_HNSW_ROUTE_NN_DESCENT;
    reply.policy.internal_builder = CUVS_HNSW_ROUTE_NN_DESCENT;
    reply.policy.requested_algo = req->build_algo;
    reply.policy.requested_ef = req->ef_construction;
    reply.policy.guard_passed = 1;
    reply.policy.n_rows = n;
    reply.policy.dim = cmd->dim;
    reply.policy.metric = cmd->metric;
    reply.policy.hierarchy = req->hierarchy;
    reply.policy.degree = 16; reply.policy.intermediate = 16;
    reply.nn.magic = CUVS_HNSW_NN_READBACK_MAGIC;
    reply.nn.version = 1;
    reply.nn.bytes = sizeof(reply.nn);
    reply.nn.graph_degree = reply.policy.intermediate;
    reply.nn.internal_degree = reply.policy.intermediate * 3 / 2 < n
        ? reply.policy.intermediate * 3 / 2 : (uint32_t)(n - 1);
    reply.nn.sdk_default_iterations = 20;
    reply.nn.max_iterations = cuvs_hnsw_scaled_quality(20, req->ef_construction, 1, 65535);
    reply.nn.dtype = CUVS_HNSW_NN_DTYPE_FP32;
    reply.nn.termination_threshold = 0.0001f;
    if (write(c, "R", 1) == 1 && cuvs_fd_send(c, ro, &reply, sizeof(reply)) == 0)
    {
        struct pollfd p = {.fd = owner, .events = POLLIN};
        if (poll(&p, 1, 5000) > 0) (void)recv(owner, &token, 1, 0);
    }
    close(ro);
}

static void *serve(void *arg)
{
    int c = (int)(intptr_t)arg, vectors = -1, owner = -1;
    CuvsCmdFrame cmd;
    CuvsHnswFileRequest req;
    char mode[64], error[128], release[4096];
    if (recv(c, &cmd, sizeof(cmd), MSG_WAITALL) != (ssize_t)sizeof(cmd) || cmd.op != CUVS_OP_BUILD_HNSW_FILE)
    { close(c); return NULL; }
    if (cuvs_hnsw_receive_file_request(c, &req, &vectors, &owner, error, sizeof(error)) != CUVS_STATUS_OK)
    { reply_status(c, CUVS_STATUS_BUILD_FAILED, error); goto out; }
    read_mode(mode, sizeof(mode));
    event(mode);
    snprintf(release, sizeof(release), "%s/release", state);
    if (!strcmp(mode, "hold"))
        while (!peer_closed(c, owner)) ;
    else if (!strcmp(mode, "gate-fail"))
    {
        while (access(release, F_OK) != 0)
            usleep(20000);
        reply_status(c, CUVS_STATUS_BUILD_FAILED, "k3 fake build failure");
    }
    else if (!strcmp(mode, "cancel"))
        reply_status(c, CUVS_STATUS_CANCELED, "k3 fake daemon cancel");
    else if (!strcmp(mode, "ok"))
        reply_ok(c, owner, &cmd, &req);
    else if (!strcmp(mode, "nn-ok"))
        reply_nn_ok(c, owner, vectors, &cmd, &req);
    else
        reply_status(c, CUVS_STATUS_BUILD_FAILED, "k3 fake build failure");
out:
    if (vectors >= 0) close(vectors);
    if (owner >= 0) close(owner);
    close(c);
    return NULL;
}

int main(int argc, char **argv)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    int l;
    if (argc != 3) { fprintf(stderr, "usage: %s SOCKET STATEDIR\n", argv[0]); return 64; }
    state = argv[2];
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", argv[1]);
    unlink(argv[1]);
    l = socket(AF_UNIX, SOCK_STREAM, 0);
    if (l < 0 || bind(l, (struct sockaddr *)&addr, sizeof(addr)) || listen(l, 16)) { perror("listen"); return 1; }
    chmod(argv[1], 0666);
    for (;;)
    {
        pthread_t t;
        int c = accept(l, NULL, NULL);
        if (c < 0) continue;
        if (pthread_create(&t, NULL, serve, (void *)(intptr_t)c)) close(c);
        else pthread_detach(t);
    }
}
