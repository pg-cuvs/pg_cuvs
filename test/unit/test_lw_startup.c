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

static const char *
slurp(const char *path)
{
    static char buf[1 << 16];
    FILE *f = fopen(path, "r");
    size_t n;
    int complete;

    if (!f)
        return NULL;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    complete = !ferror(f) && feof(f);
    if (fclose(f) != 0 || !complete)
        return NULL;
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

static void
require_start(Run *r, int tag, const char *const *flags, int flag_count)
{
    if (start(r, tag, flags, flag_count) != 0)
    {
        const char *log = slurp(r->log);
        fprintf(stderr, "cannot start LW fixture %d: %s\n", tag,
                log ? log : "log unavailable");
        stop(r);
        exit(2);
    }
}

static const char *
require_log(Run *r)
{
    const char *log = slurp(r->log);

    if (!log || !*log)
    {
        fprintf(stderr, "cannot read a complete LW fixture startup log %s\n", r->log);
        stop(r);
        exit(2);
    }
    return log;
}

static int
pid_adj(Run *r)
{
    char path[64];
    int value;

    snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", (int)r->pid);
    value = read_adj(path);
    if (value < -CUVS_OOM_SCORE_ADJ_MAX || value > CUVS_OOM_SCORE_ADJ_MAX)
    {
        fprintf(stderr, "cannot read LW fixture OOM score from %s\n", path);
        stop(r);
        exit(2);
    }
    return value;
}

static void
expect_status(const Run *r, uint32_t policy, uint32_t overcommit, uint32_t margin,
              int setting, int effective, const char *result)
{
    CuvsRmmPoolStatus st = {0};
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
    const char *startup_format = "managed large_workspace: cagra_build_overflow=%s "
                                 "managed_overcommit=%s %s=%d";
    const char *overflow = cuvs_lw_overflow_name(CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT);
    const char *overcommit = CUVS_MANAGED_OVERCOMMIT_DEFAULT ? "on" : "off";
    const int default_margin = CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT;
    char line[256], expect[160];
    const char *log;
    int inherited, baseline, default_effective, explicit_value, kept_value;
    Run r;
    if (argc != 2) { fprintf(stderr, "usage: %s <pg_cuvs_server>\n", argv[0]); return 2; }
    daemon_bin = argv[1];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/lw_startup_%d", (int)getpid());
    mkdir(tmpdir, 0700);
    inherited = read_adj("/proc/self/oom_score_adj");
    baseline = inherited < 100 ? 100 : inherited;
    if (inherited < -CUVS_OOM_SCORE_ADJ_MAX || baseline >= CUVS_OOM_SCORE_ADJ_MAX ||
        (baseline != inherited && !set_own_adj(baseline)))
    {
        fprintf(stderr, "cannot start the oom_score_adj raise fixture from %d\n", inherited);
        return 2;
    }

    /* 1. The default raises a low baseline and keeps one at or above it. */
    default_effective = baseline < CUVS_OOM_SCORE_ADJ_DEFAULT ? CUVS_OOM_SCORE_ADJ_DEFAULT : baseline;
    require_start(&r, 1, NULL, 0);
    CHECK(pid_adj(&r) == default_effective);
    snprintf(expect, sizeof(expect), baseline < default_effective ? "oom_score_adj=%d applied" :
             "oom_score_adj kept current %d", default_effective);
    snprintf(line, sizeof(line), startup_format, overflow, overcommit,
             "lw_margin_permille", default_margin);
    log = require_log(&r);
    CHECK(strstr(log, line) != NULL);
    if (!strstr(log, line))
    {
        snprintf(line, sizeof(line), startup_format, overflow, overcommit,
                 "lw_margin", default_margin);
        if (strstr(log, line))
            fputs("FAIL LW startup log uses lw_margin instead of lw_margin_permille\n", stderr);
    }
    CHECK(strstr(log, expect) != NULL);
    CHECK(strstr(log, "build admission model: scratch_permille=") != NULL);
    CHECK(strstr(log, "validated_overflow_policy=managed") != NULL);
    /* The GPU Q1 measured rho* on (run 20261108-q-02 startup line). */
    CHECK(strstr(log, "validated_gpu=\"NVIDIA A100-SXM4-80GB\"") != NULL);
    CHECK(strstr(log, "overflow=managed overcommit=on") != NULL);
    CHECK(strstr(log, "dedicated_stream=on peak_meter=on") != NULL);
    CHECK(strstr(log, "evict_min_idle_s=600 evict_hit_bonus_s=30 evict_hit_cap=20") != NULL);
    CHECK(strstr(log, "recalibrate scratch_permille for the active policy") == NULL);
    expect_status(&r, CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT, CUVS_MANAGED_OVERCOMMIT_DEFAULT,
                  CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT, CUVS_OOM_SCORE_ADJ_DEFAULT,
                  default_effective, expect);
    stop(&r);

    /* 2. An explicit value above the inherited baseline exercises the write. */
    {
        char setting[16];
        const char *flags[] = {"--cagra-build-overflow", "error", "--managed-overcommit", "off",
                               "--managed-lw-margin-permille", "1300", "--oom-score-adj", setting};
        explicit_value = baseline < 650 ? 650 : baseline + 1;
        snprintf(setting, sizeof(setting), "%d", explicit_value);
        snprintf(expect, sizeof(expect), "oom_score_adj=%d applied", explicit_value);
        require_start(&r, 2, flags, 8);
        log = require_log(&r);
        CHECK(pid_adj(&r) == explicit_value);
        CHECK(strstr(log, "managed large_workspace: cagra_build_overflow=error "
                                   "managed_overcommit=off lw_margin_permille=1300") != NULL);
        CHECK(strstr(log, expect) != NULL);
        CHECK(strstr(log, "recalibrate scratch_permille for the active policy") != NULL);
        expect_status(&r, CUVS_LW_OVERFLOW_ERROR, 0, 1300, explicit_value, explicit_value, expect);
        stop(&r);
    }

    /* 3. A higher inherited value is kept, never lowered. */
    kept_value = baseline < 800 ? 800 : baseline;
    if (!set_own_adj(kept_value))
    {
        fprintf(stderr, "cannot set the LW fixture OOM score to %d\n", kept_value);
        return 2;
    }
    require_start(&r, 3, NULL, 0);
    log = require_log(&r);
    CHECK(pid_adj(&r) == kept_value);
    snprintf(expect, sizeof(expect), "oom_score_adj kept current %d", kept_value);
    CHECK(strstr(log, expect) != NULL);
    expect_status(&r, CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT, CUVS_MANAGED_OVERCOMMIT_DEFAULT,
                  CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT, CUVS_OOM_SCORE_ADJ_DEFAULT,
                  kept_value, expect);
    stop(&r);

    /* 4. 0 leaves the inherited value untouched. */
    {
        const char *flags[] = {"--oom-score-adj", "0"};
        require_start(&r, 4, flags, 2);
        log = require_log(&r);
        CHECK(pid_adj(&r) == kept_value);
        CHECK(strstr(log, "oom_score_adj not applied: value 0") != NULL);
        stop(&r);
    }

    if (failures)
    {
        fprintf(stderr, "test_lw_startup: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS lw startup: real shim daemon applies --oom-score-adj to /proc/<pid> "
           "(default raise or keep, explicit raise, keep higher, 0 untouched), startup lines and pool-status "
           "IPC match the macros and flags\n");
    return 0;
}
