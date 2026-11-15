/* The daemon must never observe EOF on the owner lease while the
 * backend is still waiting for the reply. On macOS, closing the backend's
 * copy of the in-flight owner socket right after SCM_RIGHTS send lets the
 * daemon's received copy read EOF (~0.2% of calls); Linux 6.8 did not.
 * Stress the real client against a mock daemon that checks the lease at
 * receive time. CPU only; no PG, daemon binary or GPU. */
#define _GNU_SOURCE 1
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "cuvs_ipc.h"
#include "cuvs_build_corpus.h"
#include "cuvs_hnsw_build_common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CALLS 3000

static void daemon_loop(int listener)
{
    for (int i = 0; i < CALLS; i++)
    {
        int c = accept(listener, 0, 0), vectors = -1, owner = -1;
        CuvsCmdFrame cmd;
        CuvsHnswFileRequest req;
        char error[128] = {0};
        if (c < 0 || recv(c, &cmd, sizeof(cmd), MSG_WAITALL) != sizeof(cmd)) _exit(2);
        if (cuvs_hnsw_receive_file_request(c, &req, &vectors, &owner, error, sizeof(error))
            != CUVS_STATUS_OK) _exit(3);
        if (cuvs_hnsw_owner_cancelled(owner))
        {
            fprintf(stderr, "owner lease EOF before reply at call %d\n", i);
            _exit(1);
        }
        CuvsHnswFileReply reply = {.header = {.status = CUVS_STATUS_BUILD_FAILED}};
        snprintf(reply.header.error, sizeof(reply.header.error), "lease ok");
        if (write(c, "R", 1) != 1 || cuvs_fd_send(c, -1, &reply, sizeof(reply))) _exit(4);
        close(vectors); close(owner); close(c);
    }
    _exit(0);
}

int main(void)
{
    char dir[] = "/tmp/pgcuvs-lease-XXXXXX", path[64], input[] = "/tmp/pgcuvs-lease-in-XXXXXX";
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    float first = -1.25f;
    int status, fd, listener;
    pid_t child;
    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/socket", dir);
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0 && !bind(listener, (struct sockaddr *)&addr, sizeof(addr)) && !listen(listener, 4));
    fd = mkstemp(input); assert(fd >= 0); unlink(input);
    assert(write(fd, &first, sizeof(first)) == sizeof(first));
    assert(!ftruncate(fd, 100 * 128 * sizeof(float)));
    child = fork(); assert(child >= 0);
    if (child == 0) daemon_loop(listener);
    int completed = 0;
    for (; completed < CALLS; completed++)
    {
        int result = -1; char err[128];
        CuvsHnswPolicy p; CuvsHnswBudgetSnapshot b; CuvsHnswAceReadback a; CuvsHnswNnReadback nn;
        int rc = cuvs_ipc_build_hnsw_file(path, fd, 100, 128, CUVS_METRIC_L2, 64, 128, 0,
            1ULL << 30, CUVS_HNSW_ALGO_AUTO, 64, &p, &b, &a, &nn, &result, err, sizeof(err));
        if (rc != CUVS_STATUS_BUILD_FAILED || strcmp(err, "lease ok")) break;
    }
    assert(waitpid(child, &status, 0) == child);
    unlink(path); rmdir(dir);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || completed != CALLS)
    {
        fprintf(stderr, "FAIL owner lease: daemon exit=%d completed=%d/%d\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1, completed, CALLS);
        return 1;
    }
    printf("PASS owner lease stays open until reply: %d real-client calls\n", CALLS);
    return 0;
}
