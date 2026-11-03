/* I4 review B-2 (b) and CK-27 (b)(c) for the I4 knobs: start the real CPU-shim
 * daemon binary and check, with no fixture, that startup applies
 * --oom-score-adj to /proc/<pid>/oom_score_adj (raise, keep a higher current,
 * 0 leaves it alone), that the startup lines carry the effective values, and
 * that the pool-status IPC reply carries the same values as the macros.
 * Linux only; run as a non-root user (raising oom_score_adj is unprivileged).
 * Usage: test-lw-startup <path to pg_cuvs_server built with PGCUVS_CPU_SHIM> */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cuvs_ipc.h"
#include "cuvs_lw_policy.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const char *daemon_bin;
static char tmpdir[256];

static int
read_adj(const char *path)
{
    FILE *f = fopen(path, "r");
    int v = -9999;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = -9999; fclose(f); }
    return v;
}

static int
set_own_adj(int v)
{
    FILE *f = fopen("/proc/self/oom_score_adj", "w");
    int ok = f && fprintf(f, "%d", v) > 0;
    if (f && fclose(f) != 0) ok = 0;
    return ok;
}

static char *
slurp(const char *path)
{
    static char buf[1 << 16];
    FILE *f = fopen(path, "r");
    size_t n = 0;
    buf[0] = '\0';
    if (f) { n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    buf[n] = '\0';
    return buf;
}

typedef struct Run {
    pid_t pid;
    char sock[320];
    char log[320];
} Run;

/* Starts the daemon with extra flags; returns 0 once its socket exists. */
static int
start(Run *r, int tag, const char *const *extra, int n_extra)
{
    char idx[320];
    const char *argv[32];
    int argc = 0;
    snprintf(r->sock, sizeof(r->sock), "%s/s%d", tmpdir, tag);
    snprintf(r->log, sizeof(r->log), "%s/log%d", tmpdir, tag);
    snprintf(idx, sizeof(idx), "%s/idx%d", tmpdir, tag);
    mkdir(idx, 0700);
    argv[argc++] = daemon_bin;
    argv[argc++] = "--socket"; argv[argc++] = r->sock;
    argv[argc++] = "--index-dir"; argv[argc++] = idx;
    for (int i = 0; i < n_extra; i++) argv[argc++] = extra[i];
    argv[argc] = NULL;
    r->pid = fork();
    if (r->pid == 0)
    {
        int fd = open(r->log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, 1); dup2(fd, 2);
        execv(daemon_bin, (char *const *)argv);
        _exit(127);
    }
    for (int i = 0; i < 200; i++)
    {
        struct stat st;
        int status;
        if (stat(r->sock, &st) == 0) return 0;
        if (waitpid(r->pid, &status, WNOHANG) == r->pid) { r->pid = -1; return -1; }
        usleep(50 * 1000);
    }
    return -1;
}

static void
stop(Run *r)
{
    int status;
    if (r->pid <= 0) return;
    kill(r->pid, SIGTERM);
    for (int i = 0; i < 100 && waitpid(r->pid, &status, WNOHANG) == 0; i++) usleep(50 * 1000);
    kill(r->pid, SIGKILL);
    waitpid(r->pid, &status, 0);
    r->pid = -1;
}

static int
pid_adj(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", (int)pid);
    return read_adj(path);
}

static void
expect_status(const Run *r, uint32_t policy, uint32_t overcommit, uint32_t margin,
              int setting, int effective, const char *result)
{
    CuvsRmmPoolStatus st;
    CHECK(cuvs_ipc_rmm_pool_status(r->sock, &st) == CUVS_STATUS_OK);
    CHECK(st.cagra_build_overflow == policy);
    CHECK(st.managed_overcommit == overcommit);
    CHECK(st.lw_margin_permille == margin);
    CHECK(st.oom_score_adj == setting);
    CHECK(st.oom_score_adj_effective == effective);
    CHECK(strcmp(st.oom_score_adj_result, result) == 0);
    CHECK(st.lw_n_devices >= 1 && st.lw_devices[0].lw_pending_bytes == 0);
}

int
main(int argc, char **argv)
{
    char line[256], expect[160];
    Run r;
    if (argc != 2) { fprintf(stderr, "usage: %s <pg_cuvs_server>\n", argv[0]); return 2; }
    daemon_bin = argv[1];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/lw_startup_%d", (int)getpid());
    mkdir(tmpdir, 0700);
    if (read_adj("/proc/self/oom_score_adj") > 100 || !set_own_adj(100))
    {
        fprintf(stderr, "cannot start from oom_score_adj 100 (current %d)\n",
                read_adj("/proc/self/oom_score_adj"));
        return 1;
    }

    /* 1. Defaults: raise the inherited 100 to the default 500. */
    CHECK(start(&r, 1, NULL, 0) == 0);
    CHECK(pid_adj(r.pid) == CUVS_OOM_SCORE_ADJ_DEFAULT);
    snprintf(expect, sizeof(expect), "oom_score_adj=%d applied", CUVS_OOM_SCORE_ADJ_DEFAULT);
    snprintf(line, sizeof(line), "managed large_workspace: cagra_build_overflow=%s "
             "managed_overcommit=%s lw_margin_permille=%d",
             cuvs_lw_overflow_name(CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT),
             CUVS_MANAGED_OVERCOMMIT_DEFAULT ? "on" : "off",
             (int)CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT);
    CHECK(strstr(slurp(r.log), line) != NULL);
    CHECK(strstr(slurp(r.log), expect) != NULL);
    expect_status(&r, CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT, CUVS_MANAGED_OVERCOMMIT_DEFAULT,
                  CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT, CUVS_OOM_SCORE_ADJ_DEFAULT,
                  CUVS_OOM_SCORE_ADJ_DEFAULT, expect);
    stop(&r);

    /* 2. Explicit values for all four knobs. */
    {
        const char *flags[] = {"--cagra-build-overflow", "error", "--managed-overcommit", "off",
                               "--managed-lw-margin-permille", "1300", "--oom-score-adj", "650"};
        CHECK(start(&r, 2, flags, 8) == 0);
        CHECK(pid_adj(r.pid) == 650);
        CHECK(strstr(slurp(r.log), "managed large_workspace: cagra_build_overflow=error "
                                   "managed_overcommit=off lw_margin_permille=1300") != NULL);
        CHECK(strstr(slurp(r.log), "oom_score_adj=650 applied") != NULL);
        expect_status(&r, CUVS_LW_OVERFLOW_ERROR, 0, 1300, 650, 650, "oom_score_adj=650 applied");
        stop(&r);
    }

    /* 3. A higher inherited value is kept, never lowered. */
    CHECK(set_own_adj(800));
    CHECK(start(&r, 3, NULL, 0) == 0);
    CHECK(pid_adj(r.pid) == 800);
    CHECK(strstr(slurp(r.log), "oom_score_adj kept current 800") != NULL);
    expect_status(&r, CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT, CUVS_MANAGED_OVERCOMMIT_DEFAULT,
                  CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT, CUVS_OOM_SCORE_ADJ_DEFAULT, 800,
                  "oom_score_adj kept current 800");
    stop(&r);

    /* 4. 0 leaves the inherited value untouched. */
    {
        const char *flags[] = {"--oom-score-adj", "0"};
        CHECK(start(&r, 4, flags, 2) == 0);
        CHECK(pid_adj(r.pid) == 800);
        CHECK(strstr(slurp(r.log), "oom_score_adj not applied: value 0") != NULL);
        stop(&r);
    }

    if (failures)
    {
        fprintf(stderr, "test_lw_startup: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS lw startup: real shim daemon applies --oom-score-adj to /proc/<pid> "
           "(default raise, explicit, keep higher, 0 untouched), startup lines and pool-status "
           "IPC match the macros and flags\n");
    return 0;
}
