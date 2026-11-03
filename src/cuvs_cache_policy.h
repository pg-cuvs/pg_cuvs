#ifndef CUVS_CACHE_POLICY_H
#define CUVS_CACHE_POLICY_H

/*
 * I3-B cache policy: who may be evicted, by whom, and how much background
 * reclaim frees (v6 r18 C1 EV-1..EV-7, decision 2026-11-03-uc1-cache-eviction).
 *
 * Every function here is pure (no globals, no clock, no locks, no I/O except
 * cuvs_entry_sidecar_kind, which only stats files). The daemon owns the state
 * and calls these under g_index_mutex; CPU unit tests drive them with a fake
 * clock. Admission math (needed bytes, AR0..AR7 thresholds) is I3-A's and is
 * NOT computed here: callers pass the shortfall to free.
 */

#include <stddef.h>
#include <stdint.h>

#include "cuvs_admission_contract.h"

/* ---- Requester kinds (C1 RQ table). Every eviction caller names one. ---- */
typedef enum CuvsRequester {
    CUVS_RQ_BUILD_CAGRA = 0,
    CUVS_RQ_BUILD_SHARD,
    CUVS_RQ_BUILD_IVFPQ,
    CUVS_RQ_BUILD_FLAT,
    CUVS_RQ_LOAD,          /* search-side reload and pg_cuvs_load */
    CUVS_RQ_LOAD_SHARD,    /* sharded reload */
    CUVS_RQ_LOAD_STARTUP,  /* startup_load_indexes: never evicts */
    CUVS_RQ_LOAD_WARMUP,   /* snapshot warmup: eligible-only LRU, no soft */
    CUVS_RQ_RECLAIM,       /* background reclaim (logging only; not a pick input) */
    CUVS_RQ_MANUAL,        /* pg_cuvs_unload (logging only) */
    CUVS_RQ__COUNT
} CuvsRequester;

const char *cuvs_requester_name(CuvsRequester rq);
int  cuvs_requester_is_build(CuvsRequester rq);

/* ---- Build eviction scope (KB-7 --build-evict-scope) ---- */
typedef enum CuvsEvictScopeKind {
    CUVS_SCOPE_NONE = 0,   /* default: builds never evict residents */
    CUVS_SCOPE_IDLE,       /* eligible and idle >= idle_min minutes */
    CUVS_SCOPE_LRU         /* EV-1 only (pin and in-use still protected) */
} CuvsEvictScopeKind;

typedef struct CuvsEvictScope {
    CuvsEvictScopeKind kind;
    uint32_t           idle_min;
} CuvsEvictScope;

/* "none" | "lru" | "idle:<minutes>" (0..100000). Returns 0 or -1. */
int cuvs_parse_evict_scope(const char *s, CuvsEvictScope *out);
/* Writes "none", "lru" or "idle:<m>". */
void cuvs_format_evict_scope(const CuvsEvictScope *s, char *buf, size_t cap);

/* ---- Eviction candidates and reasons ---- */
typedef enum CuvsEvictReason {
    CUVS_EVICT_ADMIT_BUILD = 0, /* build scope (and REINDEX self-replace) */
    CUVS_EVICT_RELOAD,          /* load demand, victim was eligible */
    CUVS_EVICT_SOFT,            /* load demand, victim was still protected */
    CUVS_EVICT_RECLAIM,         /* background reclaim */
    CUVS_EVICT_MANUAL_UNLOAD,
    CUVS_EVICT_SLOT,            /* registry slot cap */
    CUVS_EVICT__COUNT
} CuvsEvictReason;

const char *cuvs_evict_reason_name(CuvsEvictReason r);

typedef struct CuvsEvictCand {
    uint32_t db_oid;
    uint32_t index_oid;
    int64_t  last_use;   /* epoch s of last search; 0 = never searched */
    int64_t  loaded_at;  /* epoch s of registration */
    uint32_t hits;       /* searches since last load */
    int      pinned;
    int      in_use;     /* in-flight operations; also loading/evicting */
    uint64_t bytes;      /* resident bytes freed by evicting it */
} CuvsEvictCand;

typedef struct CuvsEvictParams {
    int64_t  t_min_s;    /* KB-8 --evict-min-idle-s */
    int64_t  bonus_s;    /* KB-9 --evict-hit-bonus-s */
    uint32_t hits_cap;   /* KB-10 --evict-hit-cap */
} CuvsEvictParams;

/* EV-2: P(e) = t_min + min(hits, cap) * bonus (saturating). */
int64_t cuvs_evict_protect_s(uint32_t hits, int64_t t_min_s,
                             int64_t bonus_s, uint32_t hits_cap);
/* EV-1 protection: pinned or in use. */
int cuvs_evict_protected(const CuvsEvictCand *c);
/* EV-1 + EV-2: not protected and (last_use == 0 or now - last_use >= P). */
int cuvs_evict_eligible(const CuvsEvictCand *c, int64_t now,
                        const CuvsEvictParams *p);
/* EV-2 order key: max(last_use, loaded_at). */
int64_t cuvs_evict_ord(const CuvsEvictCand *c);
/* qsort-style comparator on ord ascending, then index_oid, then db_oid. */
int cuvs_evict_cmp(const CuvsEvictCand *a, const CuvsEvictCand *b);

typedef enum CuvsEvictVerdict {
    CUVS_EVICT_OK = 0,       /* victims (possibly none) cover the shortfall */
    CUVS_EVICT_AR5,          /* allowed set short, EV-1-only (or pin/in-use
                              * released, for soft load) would fit */
    CUVS_EVICT_SHORT         /* short even then: caller reports AR3 or AR4 */
} CuvsEvictVerdict;

typedef struct CuvsEvictPick {
    CuvsEvictVerdict verdict;
    int      n_victims;
    uint64_t freed;          /* bytes the victims free */
    uint64_t allowed_total;  /* sum over the requester's allowed set */
    uint64_t lru_total;      /* sum over EV-1-only set (lru_free) */
    uint64_t elig_total;     /* sum over eligible set (elig_free) */
    uint64_t pin_total;      /* bytes pinned */
    uint32_t n_pinned;
    uint64_t busy_total;     /* bytes in use (unpinned) */
    int32_t  wait;           /* AR5 w: -1 requires unpin, 0 awaits queries */
} CuvsEvictPick;

typedef struct CuvsEvictRequest {
    CuvsRequester  rq;
    CuvsEvictScope scope;      /* BUILD_* only */
    int            soft_evict; /* KB-25 --load-soft-evict (LOAD, LOAD_SHARD) */
    uint64_t       need;       /* bytes that must be freed (shortfall) */
    int64_t        now;
} CuvsEvictRequest;

/*
 * Choose victims for one admission. On CUVS_EVICT_OK, victims[0..n) are
 * indexes into cands in eviction order with their reason. On a reject no
 * victim is returned (nothing is evicted before the sum is known: ADR-086).
 * `need == 0` is OK with no victim. victims/reasons need room for n entries.
 */
void cuvs_evict_pick(const CuvsEvictCand *cands, int n,
                     const CuvsEvictParams *p, const CuvsEvictRequest *req,
                     int *victims, CuvsEvictReason *reasons,
                     CuvsEvictPick *out);

/* ---- REINDEX self-replace (UC-1 2a, KB-26) ---- */
typedef enum CuvsSelfReplace {
    CUVS_SELF_NOT_NEEDED = 0, /* fits without counting the old copy */
    CUVS_SELF_REPLACE,        /* fits only by counting it: unload after reserve */
    CUVS_SELF_UNAVAILABLE     /* no old copy, pinned, or knob off: count 0 */
} CuvsSelfReplace;

/* shortfall: bytes still missing without the old copy (0 = fits). */
CuvsSelfReplace cuvs_self_replace_decide(uint64_t shortfall, int has_old,
                                         int old_pinned, uint64_t old_bytes,
                                         int knob_on);

/* REINDEX self-replace NOTICE and DETAIL (decision UC-1 2a wording).
 * label: the index as the backend will name it (quoted qualified name, or
 * "index <oid>" in the daemon). need: the build's admission bytes; free_bytes:
 * room for it before the old copy was counted. Return 0, -1 if truncated. */
int cuvs_self_replace_notice(char *msg, size_t mcap, char *detail, size_t dcap,
                             const char *label, uint64_t need, uint64_t free_bytes);
/* The rebuild failed after the old copy was unloaded: say so and that the
 * next query reloads the old copy. */
int cuvs_self_replace_fail_notice(char *msg, size_t mcap, char *detail,
                                  size_t dcap, const char *label);

/* ---- Reclaim (EV-5): modes, free target F and its four fences ---- */
typedef enum CuvsReclaimMode {
    CUVS_RECLAIM_OFF = 0,
    CUVS_RECLAIM_USAGE,
    CUVS_RECLAIM_HEADROOM
} CuvsReclaimMode;

int  cuvs_parse_reclaim_mode(const char *s, CuvsReclaimMode *out);
const char *cuvs_reclaim_mode_name(CuvsReclaimMode m);

#define CUVS_ADMIT_LOG_CAP 128

/* One admission verdict of a BUILD_* requester. reason NONE = admitted. */
typedef struct CuvsAdmitRecord {
    uint32_t db_oid;
    uint32_t index_oid;
    uint64_t needed;
    int      reason;   /* CuvsAdmissionReason */
    int      gpu;
    int64_t  at;
} CuvsAdmitRecord;

typedef struct CuvsAdmitLog {
    CuvsAdmitRecord rec[CUVS_ADMIT_LOG_CAP];
    int             n;
} CuvsAdmitLog;

/* Append; an admission (reason NONE) also drops earlier rejects of the same
 * (db, index) so a build that got in no longer drives F (fence F-3). When
 * full, the oldest record is dropped. */
void cuvs_admit_log_add(CuvsAdmitLog *log, const CuvsAdmitRecord *r);
/* DROP INDEX: forget that index's records at once (conch 2026-11-03). */
void cuvs_admit_log_forget(CuvsAdmitLog *log, uint32_t db_oid,
                           uint32_t index_oid);
/* Drop records older than now - window_s. */
void cuvs_admit_log_expire(CuvsAdmitLog *log, int64_t now, int64_t window_s);

/* Reasons whose shortfall VRAM can resolve: AR1..AR6 (fence F-1 excludes
 * AR0 and AR7). */
int cuvs_reclaim_reason_counts(int reason);

typedef enum CuvsFreeSrc {
    CUVS_FSRC_NONE = 0,
    CUVS_FSRC_FIXED,
    CUVS_FSRC_BUILD
} CuvsFreeSrc;

typedef struct CuvsFreeTarget {
    uint64_t    f_bytes;
    uint64_t    f_cap;      /* budget_eff * cap_pct / 100 */
    CuvsFreeSrc src;
    uint32_t    src_db;
    uint32_t    src_idx;
} CuvsFreeTarget;

/* F = min(cap, fixed ? fixed : max eligible needed in window on gpu). */
void cuvs_reclaim_free_target(const CuvsAdmitLog *log, int gpu, int64_t now,
                              int64_t window_s, uint64_t budget_eff,
                              uint32_t cap_pct, uint64_t fixed,
                              CuvsFreeTarget *out);
/* "none", "fixed", "build:<db>:<idx>" */
void cuvs_format_free_src(const CuvsFreeTarget *t, char *buf, size_t cap);

typedef enum CuvsReclaimTrigger {
    CUVS_TRIG_NONE = 0,
    CUVS_TRIG_USAGE,
    CUVS_TRIG_HEADROOM,
    CUVS_TRIG_BOTH
} CuvsReclaimTrigger;

const char *cuvs_reclaim_trigger_name(CuvsReclaimTrigger t);

typedef struct CuvsReclaimGoal {
    CuvsReclaimMode mode;
    uint64_t budget_eff;  /* min(logical budget, quota) */
    uint32_t hi_pct;      /* KB-12; 100 disables the usage lines */
    uint32_t lo_pct;      /* KB-13 */
    uint32_t floor_pct;   /* KB-23, F reclaim only */
    uint64_t f_bytes;     /* from cuvs_reclaim_free_target */
} CuvsReclaimGoal;

/* Trigger at this resident level (start of a tick). */
CuvsReclaimTrigger cuvs_reclaim_trigger(const CuvsReclaimGoal *g,
                                        uint64_t resident);
/*
 * While the loop holds `active` (the trigger computed at tick start), does
 * it still need to evict one more victim at `resident`? The usage side runs
 * down to lo (no floor); the headroom side runs until budget - resident >= F
 * and never below the floor.
 */
int cuvs_reclaim_need_more(const CuvsReclaimGoal *g, CuvsReclaimTrigger active,
                           uint64_t resident);
/* Should the loop evict this victim of `bytes` at `resident`? The usage side
 * takes it whenever it still needs more; the headroom side only if the
 * resident level stays >= floor afterwards (fence F-4: never below). */
int cuvs_reclaim_take(const CuvsReclaimGoal *g, CuvsReclaimTrigger active,
                      uint64_t resident, uint64_t bytes);

/* Reclaim order: indexes of eligible candidates (EV-1, EV-2), ord ascending.
 * Sidecar exclusion is NOT applied here (the loop checks files under the
 * lock right before each save). Returns the count written to order. */
int cuvs_reclaim_plan(const CuvsEvictCand *cands, int n,
                      const CuvsEvictParams *p, int64_t now, int *order);

/* Daemon-only `r` flag for err_out (contract header): headroom mode, some
 * eligible bytes, free + eligible >= need, need <= F cap, and the resident
 * level after evicting the shortfall stays >= floor. */
int cuvs_reclaim_wait_hint(CuvsReclaimMode mode, uint64_t free_bytes,
                           uint64_t eligible_bytes, uint64_t need,
                           uint64_t f_cap, uint64_t resident,
                           uint64_t floor_bytes);

/* ---- Reclaim parameters (C6 KB-7..KB-27) ---- */
typedef struct CuvsReclaimParams {
    /* runtime-settable (pg_cuvs_reclaim_set allow-list) */
    CuvsReclaimMode mode;      /* KB-19 */
    uint64_t f_fixed;          /* KB-20 */
    int64_t  t_min_s;          /* KB-8 */
    int64_t  bonus_s;          /* KB-9 */
    uint32_t hits_cap;         /* KB-10 */
    uint32_t hi_pct;           /* KB-12 */
    uint32_t lo_pct;           /* KB-13 */
    int64_t  tick_s;           /* KB-14 */
    /* restart-only */
    CuvsEvictScope scope;      /* KB-7 */
    uint32_t pin_max_pct;      /* KB-15 */
    uint32_t f_cap_pct;        /* KB-21 */
    uint32_t window_min;       /* KB-22 */
    uint32_t floor_pct;        /* KB-23 */
    int      soft_evict;       /* KB-25 */
    int      self_replace;     /* KB-26 */
    int      usage_file;       /* KB-27 */
    int      peak_meter;       /* KB-17 --rmm-peak-meter (restart) */
} CuvsReclaimParams;

#define CUVS_EVICT_MIN_IDLE_S_DEFAULT     600
#define CUVS_EVICT_HIT_BONUS_S_DEFAULT    30
#define CUVS_EVICT_HIT_CAP_DEFAULT        20
#define CUVS_RECLAIM_HIGH_PCT_DEFAULT     85
#define CUVS_RECLAIM_LOW_PCT_DEFAULT      75
#define CUVS_RECLAIM_TICK_S_DEFAULT       10
#define CUVS_PIN_MAX_PCT_DEFAULT          50
#define CUVS_RECLAIM_FREE_MAX_PCT_DEFAULT 50
#define CUVS_RECLAIM_FREE_WINDOW_MIN_DEFAULT 30
#define CUVS_RECLAIM_FLOOR_PCT_DEFAULT    10

void cuvs_reclaim_params_default(CuvsReclaimParams *p);

/* Startup flag value for `name` (flag spelling without "--"). Returns 0, or
 * -1 with err filled (value out of range, empty, negative, unknown name). */
int cuvs_reclaim_flag_set(CuvsReclaimParams *p, const char *name,
                          const char *value, char *err, size_t errcap);
/* Cross-field check after all flags: lo <= hi. */
int cuvs_reclaim_params_check(const CuvsReclaimParams *p, char *err,
                              size_t errcap);

/* Runtime change (pg_cuvs_reclaim_set). Only the allow-list (KB-8, 9, 10,
 * 12, 13, 14, 19, 20) is accepted; on any error p is left unchanged. */
int cuvs_reclaim_runtime_set(CuvsReclaimParams *p, const char *name,
                             const char *value, char *err, size_t errcap);
int cuvs_reclaim_name_is_runtime(const char *name);
/* Is `name` (without "--") one of the I3-B daemon flags? */
int cuvs_reclaim_flag_known(const char *name);

/* ---- Usage stats file (UC-1 4c) ---- */
#define CUVS_USAGE_STATS_MAX 65536

typedef struct CuvsUsageStat {
    uint32_t db_oid;
    uint32_t index_oid;
    int64_t  last_use;
    uint32_t hits;
    int      pinned;
} CuvsUsageStat;

/* Text body with a crc trailer. Returns bytes written (excluding NUL) or -1
 * if cap is too small. */
long cuvs_usage_stats_format(const CuvsUsageStat *s, int n, char *buf,
                             size_t cap);
/* Strict parse: any defect (empty, truncated, bad header or version, count
 * above max, bad line, crc mismatch, trailing bytes) returns -1 with a short
 * reason and *n_out = 0. Never returns a partial list. */
int cuvs_usage_stats_parse(const char *buf, size_t len, CuvsUsageStat *out,
                           int max, int *n_out, char *reason,
                           size_t reasoncap);

typedef struct CuvsLoadKey {
    uint32_t db_oid;
    uint32_t index_oid;
} CuvsLoadKey;

/*
 * Startup load order within one pass: keys present in stats first, hot first
 * (last_use desc, hits desc, (db, idx) asc), then the rest by (db, idx) asc.
 * With n_stats < 0 (no or corrupt file) the order is (db, idx) asc. perm
 * receives indexes into keys. Independent of the input order of keys.
 */
void cuvs_startup_order(const CuvsLoadKey *keys, int n,
                        const CuvsUsageStat *stats, int n_stats, int *perm);

/* ---- Sidecar exclusion (track-split decision 4, UC-1 7b) ---- */
#define CUVS_SIDECAR_NONE      0
#define CUVS_SIDECAR_DELTA     1
#define CUVS_SIDECAR_TOMBSTONE 2
#define CUVS_SIDECAR_BOTH      3

/* Existence of <dir>/<db>_<idx>.delta and .tombstone (size 0 counts). */
int cuvs_entry_sidecar_kind(const char *dir, uint32_t db_oid,
                            uint32_t index_oid);
const char *cuvs_sidecar_kind_name(int kind);
/* Background reclaim skips: sidecar present AND CAGRA single AND the
 * in-memory graph differs from the files (file_current == 0). */
int cuvs_reclaim_skip_sidecar(int sidecar_kind, int is_cagra_single,
                              int file_current);

#endif /* CUVS_CACHE_POLICY_H */
