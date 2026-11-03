/*
 * The "last build was OOM" flag is per thread (v6 1.4 cuvs_wrapper (a)): the
 * daemon builds on one thread per request and reads the flag right after its
 * own build. A process-wide flag let a concurrent build on another thread
 * clear or set it. Checked against the CPU shim, which mirrors the wrapper.
 */
#include "cuvs_wrapper.h"
#include "cuvs_lw_policy.h"

#include <pthread.h>
#include <stdio.h>

static int g_failures;
#define CHECK(label, cond)                                                  \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s\n", (label));                          \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static const float g_vecs[8 * 4] = {0};

/* A non-OOM build on another thread must not touch this thread's flag. */
static void *
other_thread_builds(void *arg)
{
    CuvsCagraIndex h = cuvs_cagra_build(g_vecs, 8, 4, 0, 0, 0, 0, 0, CUVS_LW_OVERFLOW_ERROR, NULL);
    (void) arg;
    if (h)
        cuvs_cagra_free(h, 0);
    return NULL;
}

/* Another thread reading the flag must see its own (no OOM), not ours. */
static void *
other_thread_reads(void *arg)
{
    *(int *) arg = cuvs_last_build_was_oom();
    return NULL;
}

int
main(void)
{
    pthread_t t;
    int seen = -1;

    /* Arrange: this thread's build fails with an injected OOM. */
    cuvs_set_inject_build_oom(1);
    CHECK("injected build fails", cuvs_cagra_build(g_vecs, 8, 4, 0, 0, 0, 0, 0, CUVS_LW_OVERFLOW_ERROR, NULL) == NULL);

    /* Act: another thread builds successfully, a third reads its flag. */
    pthread_create(&t, NULL, other_thread_builds, NULL);
    pthread_join(t, NULL);
    pthread_create(&t, NULL, other_thread_reads, &seen);
    pthread_join(t, NULL);

    /* Assert */
    CHECK("other thread does not see our OOM", seen == 0);
    CHECK("our OOM survives another thread's build", cuvs_last_build_was_oom() == 1);
    CHECK("read clears", cuvs_last_build_was_oom() == 0);
    if (g_failures)
        return 1;
    printf("test_build_oom_thread: all checks passed\n");
    return 0;
}
