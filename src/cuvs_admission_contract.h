#ifndef CUVS_ADMISSION_CONTRACT_H
#define CUVS_ADMISSION_CONTRACT_H

#include <stdint.h>

/* I3 shared contract: UC-2 r18. This is a text payload, not a wire struct.
 * The I3-A integration must bind CUVS_PROTO_VERSION to this version before
 * deploying the new producer and consumer together (ADR-081).
 */
#define CUVS_ADMISSION_IPC_VERSION 5u
#define CUVS_NOTICE_OUT_BYTES 1024u /* msg/DETAIL pairs, includes NUL */
#define CUVS_NOTICE_MAX_PAIRS 3u
#define CUVS_ADMISSION_ERR_OUT_BYTES 512u /* includes terminating NUL */
#define CUVS_ADMISSION_MAX_CANDIDATES 3u
#define CUVS_ADMISSION_SCALAR_MAX UINT32_C(99999999)
#define CUVS_ADMISSION_SHARD_MAX 999u
#define CUVS_ADMISSION_IDLE_NEVER (-1)
#define CUVS_ADMISSION_WAIT_QUERIES 0
#define CUVS_ADMISSION_WAIT_UNPIN (-1)

/* Enum numbers are local discriminants, not CuvsReplyHeader.status values.
 * The wire reason is the spelling AR0..AR8, OOM, or ATT. NONE is not sent.
 * AR8 follows AR7 so AR0..AR8 stay one contiguous range; OOM and ATT moved
 * up by one, which no wire or stored value depends on.
 */
typedef enum CuvsAdmissionReason {
    CUVS_ADMISSION_NONE = -1,
    CUVS_ADMISSION_AR0 = 0,
    CUVS_ADMISSION_AR1,
    CUVS_ADMISSION_AR2,
    CUVS_ADMISSION_AR3,
    CUVS_ADMISSION_AR4,
    CUVS_ADMISSION_AR5,
    CUVS_ADMISSION_AR6,
    CUVS_ADMISSION_AR7,
    CUVS_ADMISSION_AR8,
    CUVS_ADMISSION_OOM,
    CUVS_ADMISSION_ATT
} CuvsAdmissionReason;

#define CUVS_ADMISSION_SQLSTATE_LIMIT "54000"     /* AR0 */
#define CUVS_ADMISSION_SQLSTATE_MEMORY "53200"    /* AR1..5, AR7, AR8, OOM, ATT */
#define CUVS_ADMISSION_SQLSTATE_WORKSPACE "53400" /* AR6 */

/* Field keys: byte-valued metrics are encoded in MiB, saturated at
 * CUVS_ADMISSION_SCALAR_MAX. OIDs keep their full uint32 range. A paired
 * value is MiB/count. Absent fields are absent, not a measured zero.
 * Per-operation required subsets remain in UC-2/C5.
 */
#define CUVS_ADMISSION_KEY_NEEDED "n"
#define CUVS_ADMISSION_KEY_FREE "f"
#define CUVS_ADMISSION_KEY_QUOTA "q"
#define CUVS_ADMISSION_KEY_PENDING "p"
#define CUVS_ADMISSION_KEY_RESIDENT "h"
#define CUVS_ADMISSION_KEY_EVICTABLE "e"
#define CUVS_ADMISSION_KEY_PINNED "k"
#define CUVS_ADMISSION_KEY_WORKSPACE "l"
#define CUVS_ADMISSION_KEY_LW_PENDING "y"
#define CUVS_ADMISSION_KEY_PHYSICAL_FREE "u"
#define CUVS_ADMISSION_KEY_HOST_GUARD "g"
#define CUVS_ADMISSION_KEY_SELF_RESIDENT "s"
#define CUVS_ADMISSION_KEY_WAIT "w"
#define CUVS_ADMISSION_KEY_RECLAIM "r"
#define CUVS_ADMISSION_KEY_CANDIDATE "t"
#define CUVS_ADMISSION_KEY_SHARD "i"
#define CUVS_ADMISSION_KEY_HEADROOM "d"   /* AR8 build headroom */
#define CUVS_ADMISSION_KEY_POOL_USED "o"  /* AR8 pool used */

/* Text grammar (only the reason-specific subset is emitted):
 * ARx n=MiB f=MiB q=MiB p=MiB h=MiB/count e=MiB/count k=MiB/count
 *     l=MiB y=MiB u=MiB g=MiB s=MiB r=0|1
 *     [t=db_oid:index_oid:MiB:idle_s] (at most three) [w=0|-1] [i=0..999]
 *     [d=MiB] [o=MiB]
 * AR8 sends n q p d o: n = N_m, p = pending (build plus lw reservations),
 * d = headroom, o = pool used before eviction.
 * Candidates are unpinned, not in use, oldest last_use first. A fourth
 * candidate is an error, never silent truncation. idle_s=-1 means never.
 * w is not an estimated countdown: -1 requires unpin; 0 awaits queries.
 * The daemon alone computes r: headroom mode AND eligible amount > 0 AND
 * free+eligible >= need AND need <= Fcap AND post-eviction resident >= floor.
 * Eligibility includes EV-1, EV-2 and the sidecar exclusion. The backend
 * must not recompute F. A false or missing r never authorizes a wait hint.
 */
typedef struct CuvsAdmissionCandidate {
    uint32_t db_oid;
    uint32_t index_oid;
    uint32_t mib;
    int32_t idle_s;
} CuvsAdmissionCandidate;

typedef struct CuvsAdmissionPayload {
    char text[CUVS_ADMISSION_ERR_OUT_BYTES];
} CuvsAdmissionPayload;

/* Admission estimate (v6 r18 C3, ADR-086). Pure, header-only, C and C++.
 *
 *   final  = n * (4 * ld + 4 * gd),  ld = 4 * ceil(dim / 4)
 *   needed = final + corpus_h2d + ceil(final * rho / 1000) + margin
 *
 * Every byte quantity saturates at CUVS_ADMISSION_BYTES_SATURATED, which no
 * real budget admits; invalid inputs (n < 0, dim <= 0, rho above
 * CUVS_ADMISSION_RHO_PERMILLE_MAX) return it too, so they fail closed.
 * graph_degree <= 0 means the cuVS default 64, as estimate_vram_bytes().
 * Use final for resident accounting and needed for admission, reservation,
 * release and GPU choice. Under C3 the measured rho already contains the
 * corpus H2D transfer and the safety factor, so a caller that follows C3 as
 * written passes corpus_h2d = 0 and margin = 0; the scratch term scales final
 * only, never corpus_h2d or margin.
 */
#define CUVS_ADMISSION_BYTES_SATURATED (UINT64_MAX / 2u)
#define CUVS_ADMISSION_RHO_PERMILLE_MAX 100000u
#define CUVS_ADMISSION_DEFAULT_GRAPH_DEGREE 64

static inline uint64_t
cuvs_admission_sat_add(uint64_t a, uint64_t b)
{
    if (a >= CUVS_ADMISSION_BYTES_SATURATED ||
        b >= CUVS_ADMISSION_BYTES_SATURATED - a)
        return CUVS_ADMISSION_BYTES_SATURATED;
    return a + b;
}

static inline uint64_t
cuvs_admission_sat_mul(uint64_t a, uint64_t b)
{
    if (a != 0 && b > CUVS_ADMISSION_BYTES_SATURATED / a)
        return CUVS_ADMISSION_BYTES_SATURATED;
    return a * b;
}

static inline uint64_t
cuvs_admission_estimate_final_bytes(int64_t n, int32_t dim, int32_t graph_degree)
{
    uint64_t ld, gd, row;

    if (n < 0 || dim <= 0)
        return CUVS_ADMISSION_BYTES_SATURATED;
    ld = 4u * (((uint64_t)dim + 3u) / 4u);
    gd = graph_degree > 0 ? (uint64_t)graph_degree
                          : (uint64_t)CUVS_ADMISSION_DEFAULT_GRAPH_DEGREE;
    row = 4u * ld + 4u * gd;
    return cuvs_admission_sat_mul((uint64_t)n, row);
}

static inline uint64_t
cuvs_admission_needed_bytes(uint64_t final_bytes, uint64_t corpus_h2d,
                            uint32_t scratch_rho_permille, uint64_t margin)
{
    uint64_t scratch, needed;

    if (scratch_rho_permille > CUVS_ADMISSION_RHO_PERMILLE_MAX ||
        final_bytes >= CUVS_ADMISSION_BYTES_SATURATED)
        return CUVS_ADMISSION_BYTES_SATURATED;
    /* ceil(final * rho / 1000) without forming final * rho. */
    scratch = cuvs_admission_sat_add(
        cuvs_admission_sat_mul(final_bytes / 1000u, scratch_rho_permille),
        ((final_bytes % 1000u) * scratch_rho_permille + 999u) / 1000u);
    needed = cuvs_admission_sat_add(final_bytes, corpus_h2d);
    needed = cuvs_admission_sat_add(needed, scratch);
    return cuvs_admission_sat_add(needed, margin);
}

/* AR8 pool-capacity premise (ADR-089). Pure, header-only, C and C++.
 *
 *   pending   = g_pending_rmm_build_vram + g_lw_pending (admitted builds)
 *   pool_used = RMM pool used bytes before eviction (resident now)
 *   headroom  = permille == 0 ? 0 : max(256 MiB, ceil(N_m * permille / 1000))
 *   AR8       = quota > 0 && pending + pool_used + N_m + headroom > quota
 *
 * lw_ub is not a term: the managed large_workspace is not pool memory (it
 * stays in AR6/AR7). Sums saturate (CUVS_ADMISSION_BYTES_SATURATED), so an
 * overflow refuses. A permille above the maximum saturates too.
 */
#define CUVS_RMM_BUILD_HEADROOM_PERMILLE_DEFAULT 150u
#define CUVS_RMM_BUILD_HEADROOM_PERMILLE_MAX 100000u
#define CUVS_RMM_BUILD_HEADROOM_FLOOR_BYTES (UINT64_C(256) << 20)

static inline uint64_t
cuvs_admission_build_headroom(uint64_t needed, uint32_t permille)
{
    uint64_t h;

    if (permille == 0)
        return 0;
    if (permille > CUVS_RMM_BUILD_HEADROOM_PERMILLE_MAX ||
        needed >= CUVS_ADMISSION_BYTES_SATURATED)
        return CUVS_ADMISSION_BYTES_SATURATED;
    /* ceil(needed * permille / 1000) without forming the product. */
    h = cuvs_admission_sat_add(
        cuvs_admission_sat_mul(needed / 1000u, permille),
        ((needed % 1000u) * permille + 999u) / 1000u);
    return h > CUVS_RMM_BUILD_HEADROOM_FLOOR_BYTES ? h
                                                   : CUVS_RMM_BUILD_HEADROOM_FLOOR_BYTES;
}

/* 1 when AR8 refuses; quota 0 (pool disabled) never refuses. */
static inline int
cuvs_admission_pool_refuses(uint64_t quota, uint64_t pending, uint64_t pool_used,
                            uint64_t needed, uint64_t headroom)
{
    if (quota == 0)
        return 0;
    return cuvs_admission_sat_add(
               cuvs_admission_sat_add(cuvs_admission_sat_add(pending, pool_used), needed),
               headroom) > quota;
}

/* Bytes the pool is short by under the AR8 sum (0 when it fits or quota is
 * 0): pending + pool_used + N_m + headroom - quota, saturating. Nonzero
 * exactly when cuvs_admission_pool_refuses() is 1.
 *
 * Admission order (owner decision 2026-11-09, ADR-089 amended): a pool
 * shortfall is first offered to the build's eviction scope (make_room)
 * together with the logical-budget and physical shortfalls. Eviction frees
 * resident pool bytes only, so a shortfall above pool_used is refused as AR8
 * before any eviction; a scope that forbids victims a wider set would allow
 * is AR5; AR8 also refuses when no permissible eviction covers the pool
 * shortfall, or when the pool is still short after eviction. */
static inline uint64_t
cuvs_admission_pool_shortfall(uint64_t quota, uint64_t pending, uint64_t pool_used,
                              uint64_t needed, uint64_t headroom)
{
    uint64_t sum;

    if (quota == 0)
        return 0;
    sum = cuvs_admission_sat_add(
        cuvs_admission_sat_add(cuvs_admission_sat_add(pending, pool_used), needed),
        headroom);
    return sum > quota ? sum - quota : 0;
}

/* I4 admission hook: the out-of-pool large_workspace check (AR6/AR7, v6 1.4
 * "AR6, AR7 판정식"). The daemon calls it once per CAGRA build admission,
 * after AR2 and before any scope eviction, with pre-eviction snapshots; it is
 * not re-run after eviction. Returns CUVS_ADMISSION_NONE, AR6 or AR7 only.
 * Defined in cuvs_util.c. The I3-A tip has no managed policy, so its
 * definition returns CUVS_ADMISSION_NONE; I4 owns and replaces that body.
 * Fields are append-only (I3-A, then I3-B, I3-C, I4).
 */
typedef struct CuvsAdmissionWorkspaceInput {
    uint64_t needed;     /* N_m from cuvs_admission_needed_bytes() */
    uint64_t reserved;   /* pool reserved bytes on the device */
    uint64_t pool_used;  /* pool used bytes on the device */
    uint64_t phys_free;  /* cudaMemGetInfo free + async reclaimable */
    uint64_t lw_ub;      /* this build's large_workspace upper bound */
    uint64_t lw_pending; /* sum of lw_ub of admitted managed builds */
    uint64_t host_guard; /* host RAM allowance for managed spill */
    uint32_t managed;    /* 1 when the large_workspace policy is managed */
    uint32_t overcommit; /* 1 when managed overcommit is on */
} CuvsAdmissionWorkspaceInput;

#ifdef __cplusplus
extern "C" {
#endif
CuvsAdmissionReason
cuvs_admission_workspace_check(const CuvsAdmissionWorkspaceInput *in);
#ifdef __cplusplus
}
#endif

#endif /* CUVS_ADMISSION_CONTRACT_H */
