/*
 * k3_fake_daemon.c - scripted stand-in for pg_cuvs_server, HNSW build only.
 * Used by test/k3_digest_integration.py (make test-k3-digest).
 *
 * Usage: k3_fake_daemon SOCKET STATEDIR
 * Each CUVS_OP_BUILD_HNSW_FILE request reads STATEDIR/mode at accept time:
 *   fail       reply BUILD_FAILED "k3 fake build failure"
 *   cancel     reply status CANCELED (a daemon-side cancel, no backend interrupt)
 *   ok         reply OK with a valid resident receipt and a read-only file
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
