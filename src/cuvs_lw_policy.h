#ifndef CUVS_LW_POLICY_H
#define CUVS_LW_POLICY_H

/* I4 / ADR-088: CAGRA large_workspace overflow policy, pure decisions.
 *
 * Dependency-free (no PG, CUDA, daemon state) so the daemon, the CPU shim and
 * the unit tests share one definition. Every byte quantity saturates at
 * CUVS_LW_SATURATED (SIZE_MAX/2 on the C3 convention), which always refuses.
 *
 * Plan v6 r18 C3: L_shape = max(4 n ld + 4 n igd, n (5 igd + 4)) bytes,
 * lw_ub = ceil(L_shape x margin_permille / 1000). 1.4 (I4 files):
 * phys_need = max(0, N_m - (reserved - pool_used)) + lw_ub + lw_pending,
 * spill_est = max(0, phys_need - phys_free). Overcommit off: spill_est > 0 is
 * AR6. On: spill_est > host_guard is AR7. Policy error never yields AR6/AR7.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cuvs_admission_contract.h"

typedef enum CuvsLwOverflow {
    CUVS_LW_OVERFLOW_ERROR = 0,   /* large_workspace stays in the RMM pool */
    CUVS_LW_OVERFLOW_MANAGED = 1  /* build-only managed resource (spills to host) */
} CuvsLwOverflow;

/* KB-4, KB-5, KB-6, KB-24 defaults (UC-3 A + (d), 2026-11-03). The margin is
 * the pre-LW-8 provisional value; LW-8/LW-8x replace this single literal. */
#define CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT       CUVS_LW_OVERFLOW_MANAGED
#define CUVS_MANAGED_OVERCOMMIT_DEFAULT         1
#define CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT 1100
#define CUVS_MANAGED_LW_MARGIN_PERMILLE_MIN     1000u
#define CUVS_MANAGED_LW_MARGIN_PERMILLE_MAX     100000u
#define CUVS_OOM_SCORE_ADJ_DEFAULT              500
#define CUVS_OOM_SCORE_ADJ_MAX                  1000
/* Startup oom_score_adj result text (log line and pool status). */
#define CUVS_LW_OOM_RESULT_LEN                  96
/* Wrapper default for intermediate_graph_degree <= 0 (cuvs_wrapper.cu). */
#define CUVS_LW_DEFAULT_IGD                     128u

#define CUVS_LW_SATURATED ((uint64_t)(SIZE_MAX / 2))

static inline uint64_t
cuvs_lw_sat_add(uint64_t a, uint64_t b)
{
    return (a >= CUVS_LW_SATURATED || b >= CUVS_LW_SATURATED ||
            a > CUVS_LW_SATURATED - b) ? CUVS_LW_SATURATED : a + b;
}

static inline uint64_t
cuvs_lw_sat_mul(uint64_t a, uint64_t b)
{
    if (a == 0 || b == 0) return 0;
    return (a >= CUVS_LW_SATURATED || b > CUVS_LW_SATURATED / a)
        ? CUVS_LW_SATURATED : a * b;
}

/* L_shape in bytes: max of the nn_descent sort_knn_graph term (dataset copy
 * at the padded stride ld = 4 ceil(dim/4), plus the input graph) and the
 * prune term. Applies to AUTO, ivf_pq and nn_descent alike (C3). */
static inline uint64_t
cuvs_lw_shape_bytes(int64_t n, uint32_t dim, int igd)
{
    uint64_t rows = n > 0 ? (uint64_t)n : 0;
    uint64_t ld = ((uint64_t)dim + 3u) / 4u * 4u;
    uint64_t g = igd > 0 ? (uint64_t)igd : CUVS_LW_DEFAULT_IGD;
    uint64_t sort_term, prune_term;
    if (rows > CUVS_LW_SATURATED / 4u) return CUVS_LW_SATURATED;
    sort_term = cuvs_lw_sat_add(cuvs_lw_sat_mul(4u * rows, ld),
                                cuvs_lw_sat_mul(4u * rows, g));
    prune_term = cuvs_lw_sat_mul(rows, cuvs_lw_sat_add(5u * g, 4u));
    return sort_term > prune_term ? sort_term : prune_term;
}

/* lw_ub = ceil(L_shape x margin / 1000). */
static inline uint64_t
cuvs_lw_ub_bytes(uint64_t shape_bytes, uint32_t margin_permille)
{
    uint64_t scaled = cuvs_lw_sat_mul(shape_bytes, margin_permille);
    if (scaled >= CUVS_LW_SATURATED) return CUVS_LW_SATURATED;
    return scaled / 1000u + (scaled % 1000u != 0);
}

/* LW-8 margin rule (C3; v6 3.5 3a/3b): margin_permille =
 * ceil(1000 x 1.10 x max_i(lw_peak_i / L_shape_i)), lower bound 1000. Exact
 * integer form max_i ceil(1100 x lw_peak_i / L_shape_i). Returns 0 for no
 * points or a zero L_shape (invalid input); saturates at UINT32_MAX. The
 * result is the value the one-line commit writes into
 * CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT (startup refuses above the knob
 * maximum). */
static inline uint32_t
cuvs_lw_margin_from_lw8(const uint64_t *lw_peak, const uint64_t *shape, size_t n)
{
    unsigned __int128 best = CUVS_MANAGED_LW_MARGIN_PERMILLE_MIN;
    size_t i;
    if (n == 0) return 0;
    for (i = 0; i < n; i++)
    {
        unsigned __int128 q;
        if (shape[i] == 0) return 0;
        q = ((unsigned __int128)lw_peak[i] * 1100u + shape[i] - 1) / shape[i];
        if (q > best) best = q;
    }
    return best > UINT32_MAX ? UINT32_MAX : (uint32_t)best;
}

/* C3 large_workspace shape class: lw_checked inside the LW-8 grid (dim
 * 128..768, igd 128..256, n <= 999,000), else lw_extrapolated, for which the
 * margin is not known to cover lw_peak: the [admit] shape_class gets the
 * "+lw_extrapolated" suffix and the daemon logs a WARN. */
#define CUVS_LW_CHECKED_MAX_ROWS 999000
static inline int
cuvs_lw_shape_checked(int64_t n, uint32_t dim, int igd)
{
    uint32_t g = igd > 0 ? (uint32_t)igd : CUVS_LW_DEFAULT_IGD;
    return n >= 0 && n <= CUVS_LW_CHECKED_MAX_ROWS && dim >= 128 && dim <= 768 &&
           g >= 128 && g <= 256;
}

typedef struct CuvsLwAdmitInput {
    uint32_t policy;        /* CuvsLwOverflow */
    int      overcommit;    /* KB-5 on = 1 */
    uint64_t n_m;           /* in-pool admission value N_m (I3-A needed) */
    uint64_t reserved;      /* pool reserved bytes, pre-eviction snapshot */
    uint64_t pool_used;     /* pool used bytes, pre-eviction snapshot */
    uint64_t phys_free;     /* cudaMemGetInfo free + async_reclaimable (FACT-45) */
    uint64_t lw_ub;         /* this build's estimate */
    uint64_t lw_pending;    /* admitted managed builds' lw_ub on this GPU */
    uint64_t host_guard;    /* min(MemAvailable/2, cgroup remaining/2); 0 on failure */
} CuvsLwAdmitInput;

typedef struct CuvsLwAdmitResult {
    uint64_t phys_need;
    uint64_t spill_est;
    CuvsAdmissionReason reason;   /* NONE, AR6 or AR7 */
} CuvsLwAdmitResult;

static inline CuvsLwAdmitResult
cuvs_lw_admit(const CuvsLwAdmitInput *in)
{
    CuvsLwAdmitResult r = {0, 0, CUVS_ADMISSION_NONE};
    uint64_t pool_slack, growth;
    if (in->policy != CUVS_LW_OVERFLOW_MANAGED)
        return r;
    pool_slack = in->reserved > in->pool_used ? in->reserved - in->pool_used : 0;
    growth = in->n_m > pool_slack ? in->n_m - pool_slack : 0;
    r.phys_need = cuvs_lw_sat_add(cuvs_lw_sat_add(growth, in->lw_ub), in->lw_pending);
    r.spill_est = r.phys_need > in->phys_free ? r.phys_need - in->phys_free : 0;
    if (!in->overcommit) {
        if (r.spill_est > 0) r.reason = CUVS_ADMISSION_AR6;
    } else if (r.spill_est > in->host_guard) {
        r.reason = CUVS_ADMISSION_AR7;
    }
    return r;
}

/* ---- KB-4/5/6/24 strict parsers: 0 on success, -1 leaves *out untouched ---- */

static inline int
cuvs_lw_parse_uint(const char *s, unsigned long lo, unsigned long hi, unsigned long *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || *s < '0' || *s > '9') return -1;   /* no sign, space or empty */
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || !end || *end != '\0' || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

static inline int
cuvs_lw_parse_overflow(const char *s, uint32_t *out)
{
    if (s && strcmp(s, "error") == 0) { *out = CUVS_LW_OVERFLOW_ERROR; return 0; }
    if (s && strcmp(s, "managed") == 0) { *out = CUVS_LW_OVERFLOW_MANAGED; return 0; }
    return -1;
}

static inline int
cuvs_lw_parse_on_off(const char *s, int *out)
{
    if (s && strcmp(s, "on") == 0) { *out = 1; return 0; }
    if (s && strcmp(s, "off") == 0) { *out = 0; return 0; }
    return -1;
}

static inline int
cuvs_lw_parse_margin(const char *s, uint32_t *out)
{
    unsigned long v;
    if (cuvs_lw_parse_uint(s, CUVS_MANAGED_LW_MARGIN_PERMILLE_MIN,
                           CUVS_MANAGED_LW_MARGIN_PERMILLE_MAX, &v) != 0)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

/* ADR-089 --rmm-build-headroom-permille, 0..100000 (0 = no headroom). */
static inline int
cuvs_lw_parse_build_headroom(const char *s, uint32_t *out)
{
    unsigned long v;
    if (cuvs_lw_parse_uint(s, 0, CUVS_RMM_BUILD_HEADROOM_PERMILLE_MAX, &v) != 0) return -1;
    *out = (uint32_t)v;
    return 0;
}

static inline int
cuvs_lw_parse_oom_score_adj(const char *s, int *out)
{
    unsigned long v;
    if (cuvs_lw_parse_uint(s, 0, CUVS_OOM_SCORE_ADJ_MAX, &v) != 0) return -1;
    *out = (int)v;
    return 0;
}

static inline const char *
cuvs_lw_overflow_name(uint32_t policy)
{
    return policy == CUVS_LW_OVERFLOW_MANAGED ? "managed" : "error";
}

/* ---- KB-24 oom_score_adj: decide before touching /proc ---- */

typedef enum CuvsOomAdjAction {
    CUVS_OOM_ADJ_SKIP_ZERO = 0,  /* setting 0: never open the file */
    CUVS_OOM_ADJ_KEEP,           /* current >= setting: never lower */
    CUVS_OOM_ADJ_WRITE           /* current < setting: raise */
} CuvsOomAdjAction;

static inline CuvsOomAdjAction
cuvs_oom_adj_action(int setting, int current)
{
    if (setting == 0) return CUVS_OOM_ADJ_SKIP_ZERO;
    if (current >= setting) return CUVS_OOM_ADJ_KEEP;
    return CUVS_OOM_ADJ_WRITE;
}

/* Startup log / status text (C8). err is errno of a failed write, else 0. */
static inline void
cuvs_oom_adj_describe(CuvsOomAdjAction action, int setting, int current, int err,
                      int linux_host, char *buf, size_t len)
{
    if (!linux_host)
        snprintf(buf, len, "oom_score_adj not applied: not Linux");
    else if (action == CUVS_OOM_ADJ_SKIP_ZERO)
        snprintf(buf, len, "oom_score_adj not applied: value 0");
    else if (action == CUVS_OOM_ADJ_KEEP)
        snprintf(buf, len, "oom_score_adj kept current %d", current);
    else if (err == EACCES || err == EPERM)
        snprintf(buf, len, "oom_score_adj not applied: permission denied");
    else if (err)
        snprintf(buf, len, "oom_score_adj not applied: %s", strerror(err));
    else
        snprintf(buf, len, "oom_score_adj=%d applied", setting);
}

/* ---- completion NOTICE (C8 (c)) and spill flag ([build_peak] spill) ---- */

static inline uint64_t
cuvs_lw_mib(uint64_t bytes)
{
    uint64_t mib = bytes / (1024u * 1024u);
    return mib > CUVS_ADMISSION_SCALAR_MAX ? CUVS_ADMISSION_SCALAR_MAX : mib;
}

/* spill = 1 when policy managed and the physical free observed at the
 * lw_peak update was below lw_peak (approximation defined by C8). */
static inline int
cuvs_lw_spill_flag(uint32_t policy, uint64_t lw_peak, uint64_t free_at_peak)
{
    return policy == CUVS_LW_OVERFLOW_MANAGED && lw_peak > 0 && free_at_peak < lw_peak;
}

/* Returns 1 and writes the requester NOTICE when the build spilled. */
static inline int
cuvs_lw_spill_notice(int spill, uint64_t lw_peak, uint64_t phys_free,
                     char *buf, size_t len)
{
    if (!spill) { if (len) buf[0] = '\0'; return 0; }
    snprintf(buf, len,
             "pg_cuvs: build used managed memory spill (large_workspace peak %llu MiB, "
             "physical free %llu MiB); the build may be slower than usual",
             (unsigned long long)cuvs_lw_mib(lw_peak),
             (unsigned long long)cuvs_lw_mib(phys_free));
    return 1;
}

/* AR6/AR7 err_out fields in the shared grammar: AR6 n l y u, AR7 n l y g
 * (C5 subsets, MiB, saturated). Returns snprintf length or -1 for others. */
static inline int
cuvs_lw_reject_fields(const CuvsAdmissionWorkspaceInput *in, CuvsAdmissionReason reason,
                      char *buf, size_t len)
{
    unsigned long long n = cuvs_lw_mib(in->needed), l = cuvs_lw_mib(in->lw_ub);
    unsigned long long y = cuvs_lw_mib(in->lw_pending);
    if (reason == CUVS_ADMISSION_AR6)
        return snprintf(buf, len, "AR6 n=%llu l=%llu y=%llu u=%llu", n, l, y,
                        (unsigned long long)cuvs_lw_mib(in->phys_free));
    if (reason == CUVS_ADMISSION_AR7)
        return snprintf(buf, len, "AR7 n=%llu l=%llu y=%llu g=%llu", n, l, y,
                        (unsigned long long)cuvs_lw_mib(in->host_guard));
    return -1;
}

#endif /* CUVS_LW_POLICY_H */
