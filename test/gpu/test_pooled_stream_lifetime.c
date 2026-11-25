/*
 * GPU check that a failed request does not destroy a stream that a live index
 * still refers to (needs a CUDA device; build on the dev VM, not part of
 * make test-unit).
 *
 * With the RMM pool and dedicated streams on (the daemon defaults), an index
 * built on a pooled slot keeps device buffers that remember the slot's stream.
 * The slot goes back to the pool; the next build takes the same slot and
 * fails (injected OOM), so the slot is poisoned. Freeing the first index must
 * still work: RMM's pool looks up the buffer's stream when it is freed.
 * Before the fix the poisoned slot destroyed its stream and the free crashed.
 *
 * The CUDA work runs in a child process so a crash is reported as FAIL.
 * Usage: test_pooled_stream_lifetime [rounds]   (default 3)
 */
#include "cuvs_wrapper.h"
#include "cuvs_lw_policy.h"
#include "cuvs_ipc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define N_ROWS 2000
#define DIM 16

static float *
make_vecs(int64_t n, int dim)
{
    float *v = malloc((size_t) n * (size_t) dim * sizeof(float));
    unsigned s = 12345u;
    for (int64_t i = 0; i < n * dim; i++) {
        s = s * 1103515245u + 12345u;
        v[i] = (float) ((s >> 8) & 0xffff) / 65536.0f;
    }
    return v;
}

static CuvsCagraIndex
build(const float *v)
{
    return cuvs_cagra_build(v, N_ROWS, DIM, CUVS_METRIC_L2, 32, 64,
                            CUVS_CAGRA_BUILD_NN_DESCENT, 0, CUVS_LW_OVERFLOW_ERROR, NULL);
}

static int
self_query(CuvsCagraIndex h, const float *v, int64_t row)
{
    CuvsSearchResult r[4];
    return cuvs_cagra_search(h, v + row * DIM, DIM, 4, r, 0, 64, 1) == 0 &&
           r[0].item_id == row && fabsf(r[0].distance) < 1e-3f;
}

static int
run(int rounds)
{
    float *v = make_vecs(N_ROWS, DIM);

    if (!cuvs_gpu_available()) {
        fprintf(stderr, "no CUDA device\n");
        return 2;
    }
    if (cuvs_rmm_pool_init(0, (size_t) 64 << 20, 50) != 0) {
        fprintf(stderr, "FAIL rmm pool init\n");
        return 1;
    }
    for (int round = 0; round < rounds; round++) {
        CuvsCagraIndex a = build(v), b, c;
        if (!a || !self_query(a, v, 7)) {
            fprintf(stderr, "FAIL round %d: first build\n", round);
            return 1;
        }
        cuvs_set_inject_build_oom(1);
        b = build(v);   /* takes the slot a was built on and poisons it */
        cuvs_set_inject_build_oom(0);
        if (b) {
            fprintf(stderr, "FAIL round %d: injected build OOM did not fail\n", round);
            return 1;
        }
        printf("ok   round %d: injected build failure\n", round);
        fflush(stdout);
        cuvs_cagra_free(a, 0);   /* frees buffers on the poisoned slot's stream */
        printf("ok   round %d: index built before the failure freed\n", round);
        c = build(v);
        if (!c || !self_query(c, v, 11)) {
            fprintf(stderr, "FAIL round %d: build after the failure\n", round);
            return 1;
        }
        cuvs_cagra_free(c, 0);
        printf("ok   round %d: later build and search\n", round);
        fflush(stdout);
    }
    free(v);
    return 0;
}

int
main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 3;
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0)
        _exit(run(rounds));
    if (waitpid(pid, &status, 0) != pid) {
        perror("waitpid");
        return 1;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "FAIL child killed by signal %d (%s)\n",
                WTERMSIG(status), strsignal(WTERMSIG(status)));
        return 1;
    }
    if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL child exit %d\n", WEXITSTATUS(status));
        return WEXITSTATUS(status) == 2 ? 2 : 1;
    }
    printf("PASS pooled stream outlives a failed request (%d rounds)\n", rounds);
    return 0;
}
