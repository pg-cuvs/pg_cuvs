/*
 * I3-B cache policy (pure). See cuvs_cache_policy.h for the contract.
 */
#include "cuvs_cache_policy.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cuvs_util.h"   /* cuvs_crc32 */

/* ---------------------------------------------------------------- names */

const char *
cuvs_requester_name(CuvsRequester rq)
{
    switch (rq)
    {
        case CUVS_RQ_BUILD_CAGRA:  return "BUILD_CAGRA";
        case CUVS_RQ_BUILD_SHARD:  return "BUILD_SHARD";
        case CUVS_RQ_BUILD_IVFPQ:  return "BUILD_IVFPQ";
        case CUVS_RQ_BUILD_FLAT:   return "BUILD_FLAT";
        case CUVS_RQ_LOAD:         return "LOAD";
        case CUVS_RQ_LOAD_SHARD:   return "LOAD_SHARD";
        case CUVS_RQ_LOAD_STARTUP: return "LOAD_STARTUP";
        case CUVS_RQ_LOAD_WARMUP:  return "LOAD_WARMUP";
        case CUVS_RQ_RECLAIM:      return "RECLAIM";
        case CUVS_RQ_MANUAL:       return "MANUAL";
        default:                   return "?";
    }
}

int
cuvs_requester_is_build(CuvsRequester rq)
{
    return rq == CUVS_RQ_BUILD_CAGRA || rq == CUVS_RQ_BUILD_SHARD ||
           rq == CUVS_RQ_BUILD_IVFPQ || rq == CUVS_RQ_BUILD_FLAT;
}

const char *
cuvs_evict_reason_name(CuvsEvictReason r)
{
    switch (r)
    {
        case CUVS_EVICT_ADMIT_BUILD:   return "admit_build";
        case CUVS_EVICT_RELOAD:        return "reload";
        case CUVS_EVICT_SOFT:          return "soft_evict";
        case CUVS_EVICT_RECLAIM:       return "reclaim";
        case CUVS_EVICT_MANUAL_UNLOAD: return "manual_unload";
        case CUVS_EVICT_SLOT:          return "slot";
        default:                       return "?";
    }
}

/* ------------------------------------------------------------ parsing */

/* Strict unsigned decimal: digits only, no sign, no blanks, <= max. */
static int
parse_u64(const char *s, uint64_t max, uint64_t *out)
{
    uint64_t v = 0;
    if (!s || !*s)
        return -1;
    for (const char *c = s; *c; c++)
    {
        if (*c < '0' || *c > '9')
            return -1;
        unsigned d = (unsigned)(*c - '0');
        if (v > (UINT64_MAX - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    if (v > max)
        return -1;
    *out = v;
    return 0;
}

int
cuvs_parse_evict_scope(const char *s, CuvsEvictScope *out)
{
    uint64_t m;
    if (!s || !out)
        return -1;
    if (strcmp(s, "none") == 0)
    {
        out->kind = CUVS_SCOPE_NONE;
        out->idle_min = 0;
        return 0;
    }
    if (strcmp(s, "lru") == 0)
    {
        out->kind = CUVS_SCOPE_LRU;
        out->idle_min = 0;
        return 0;
    }
    if (strncmp(s, "idle:", 5) == 0 && parse_u64(s + 5, 100000, &m) == 0)
    {
        out->kind = CUVS_SCOPE_IDLE;
        out->idle_min = (uint32_t)m;
        return 0;
    }
    return -1;
}

void
cuvs_format_evict_scope(const CuvsEvictScope *s, char *buf, size_t cap)
{
    if (s->kind == CUVS_SCOPE_IDLE)
        snprintf(buf, cap, "idle:%u", s->idle_min);
    else
        snprintf(buf, cap, "%s", s->kind == CUVS_SCOPE_LRU ? "lru" : "none");
}

int
cuvs_parse_reclaim_mode(const char *s, CuvsReclaimMode *out)
{
    if (!s)
        return -1;
    if (strcmp(s, "off") == 0)           *out = CUVS_RECLAIM_OFF;
    else if (strcmp(s, "usage") == 0)    *out = CUVS_RECLAIM_USAGE;
    else if (strcmp(s, "headroom") == 0) *out = CUVS_RECLAIM_HEADROOM;
    else return -1;
    return 0;
}

const char *
cuvs_reclaim_mode_name(CuvsReclaimMode m)
{
    switch (m)
    {
        case CUVS_RECLAIM_OFF:      return "off";
        case CUVS_RECLAIM_USAGE:    return "usage";
        case CUVS_RECLAIM_HEADROOM: return "headroom";
        default:                    return "?";
    }
}

/* -------------------------------------------------------- EV-1 / EV-2 */

int64_t
cuvs_evict_protect_s(uint32_t hits, int64_t t_min_s, int64_t bonus_s,
                     uint32_t hits_cap)
{
    uint32_t h = hits < hits_cap ? hits : hits_cap;
    if (t_min_s < 0) t_min_s = 0;
    if (bonus_s < 0) bonus_s = 0;
    /* Bounded by flag ranges (86400 + 1000 * 3600), no overflow in int64. */
    return t_min_s + (int64_t)h * bonus_s;
}

int
cuvs_evict_protected(const CuvsEvictCand *c)
{
    return c->pinned || c->in_use > 0;
}

int
cuvs_evict_eligible(const CuvsEvictCand *c, int64_t now,
                    const CuvsEvictParams *p)
{
    if (cuvs_evict_protected(c))
        return 0;
    if (c->last_use == 0)
        return 1;   /* never searched: no minimum retention (UC-1 3b) */
    return now - c->last_use >=
           cuvs_evict_protect_s(c->hits, p->t_min_s, p->bonus_s, p->hits_cap);
}

int64_t
cuvs_evict_ord(const CuvsEvictCand *c)
{
    return c->last_use > c->loaded_at ? c->last_use : c->loaded_at;
}

int
cuvs_evict_cmp(const CuvsEvictCand *a, const CuvsEvictCand *b)
{
    int64_t oa = cuvs_evict_ord(a), ob = cuvs_evict_ord(b);
    if (oa != ob)
        return oa < ob ? -1 : 1;
    if (a->index_oid != b->index_oid)
        return a->index_oid < b->index_oid ? -1 : 1;
    if (a->db_oid != b->db_oid)
        return a->db_oid < b->db_oid ? -1 : 1;
    return 0;
}

/* Insertion sort of candidate indexes by cuvs_evict_cmp (n is small: the
 * registry on one GPU). Stable for equal keys. */
static void
sort_by_ord(const CuvsEvictCand *cands, int *idx, int n)
{
    for (int i = 1; i < n; i++)
    {
        int v = idx[i];
        int j = i - 1;
        while (j >= 0 && cuvs_evict_cmp(&cands[idx[j]], &cands[v]) > 0)
        {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = v;
    }
}

static uint64_t
sat_add(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

/* Is candidate c in the build scope (EV-3)? */
static int
in_build_scope(const CuvsEvictCand *c, const CuvsEvictScope *scope,
               int64_t now, const CuvsEvictParams *p)
{
    switch (scope->kind)
    {
        case CUVS_SCOPE_NONE:
            return 0;
        case CUVS_SCOPE_LRU:
            return !cuvs_evict_protected(c);
        case CUVS_SCOPE_IDLE:
            if (!cuvs_evict_eligible(c, now, p))
                return 0;
            return c->last_use == 0 ||
                   now - c->last_use >= (int64_t)scope->idle_min * 60;
    }
    return 0;
}

void
cuvs_evict_pick(const CuvsEvictCand *cands, int n, const CuvsEvictParams *p,
                const CuvsEvictRequest *req, int *victims,
                CuvsEvictReason *reasons, CuvsEvictPick *out)
{
    memset(out, 0, sizeof(*out));
    out->verdict = CUVS_EVICT_OK;

    int *order = n > 0 ? malloc(sizeof(int) * (size_t)n) : NULL;
    int  n_order = 0;

    for (int i = 0; i < n; i++)
    {
        const CuvsEvictCand *c = &cands[i];
        if (c->pinned)
        {
            out->pin_total = sat_add(out->pin_total, c->bytes);
            out->n_pinned++;
        }
        else if (c->in_use > 0)
            out->busy_total = sat_add(out->busy_total, c->bytes);
        else
            out->lru_total = sat_add(out->lru_total, c->bytes);
        if (cuvs_evict_eligible(c, req->now, p))
            out->elig_total = sat_add(out->elig_total, c->bytes);
    }

    if (req->need == 0)
    {
        free(order);
        return;
    }

    /* Allowed set per requester (C1 RQ table). Soft load takes eligible
     * victims first, then protected-by-time ones; both in ord order. */
    int soft = (req->rq == CUVS_RQ_LOAD || req->rq == CUVS_RQ_LOAD_SHARD) &&
               req->soft_evict;
    for (int pass = 0; pass < (soft ? 2 : 1) && order; pass++)
    {
        int start = n_order;
        for (int i = 0; i < n; i++)
        {
            const CuvsEvictCand *c = &cands[i];
            int elig = cuvs_evict_eligible(c, req->now, p);
            int take = 0;
            if (cuvs_requester_is_build(req->rq))
                take = in_build_scope(c, &req->scope, req->now, p);
            else if (req->rq == CUVS_RQ_LOAD_STARTUP)
                take = 0;
            else if (pass == 0)
                take = elig;
            else
                take = !elig && !cuvs_evict_protected(c);
            if (take)
                order[n_order++] = i;
        }
        sort_by_ord(cands, order + start, n_order - start);
    }

    for (int k = 0; k < n_order; k++)
        out->allowed_total = sat_add(out->allowed_total, cands[order[k]].bytes);

    if (out->allowed_total >= req->need)
    {
        uint64_t freed = 0;
        for (int k = 0; k < n_order && freed < req->need; k++)
        {
            const CuvsEvictCand *c = &cands[order[k]];
            CuvsEvictReason why;
            if (cuvs_requester_is_build(req->rq))
                why = CUVS_EVICT_ADMIT_BUILD;
            else if (cuvs_evict_eligible(c, req->now, p))
                why = CUVS_EVICT_RELOAD;
            else
                why = CUVS_EVICT_SOFT;
            victims[out->n_victims] = order[k];
            reasons[out->n_victims] = why;
            out->n_victims++;
            freed = sat_add(freed, c->bytes);
        }
        out->freed = freed;
        free(order);
        return;
    }

    /* Reject: nothing is evicted. AR5 when a wider set would have fit. */
    free(order);
    if (req->rq == CUVS_RQ_LOAD_STARTUP)
    {
        out->verdict = CUVS_EVICT_SHORT; /* startup never reports AR5 */
        return;
    }
    if (soft)
    {
        /* Allowed set already equals the EV-1-only set; AR5 now means "only
         * pinned or busy bytes remain and releasing them would fit". */
        uint64_t with_busy = sat_add(out->lru_total, out->busy_total);
        uint64_t with_all  = sat_add(with_busy, out->pin_total);
        if (with_busy >= req->need)
        {
            out->verdict = CUVS_EVICT_AR5;
            out->wait = CUVS_ADMISSION_WAIT_QUERIES;
        }
        else if (with_all >= req->need)
        {
            out->verdict = CUVS_EVICT_AR5;
            out->wait = CUVS_ADMISSION_WAIT_UNPIN;
        }
        else
            out->verdict = CUVS_EVICT_SHORT;
        return;
    }
    if (out->lru_total >= req->need)
    {
        out->verdict = CUVS_EVICT_AR5;
        out->wait = CUVS_ADMISSION_WAIT_UNPIN;
    }
    else
        out->verdict = CUVS_EVICT_SHORT;
}

/* ------------------------------------------------------- self-replace */

CuvsSelfReplace
cuvs_self_replace_decide(uint64_t shortfall, int has_old, int old_pinned,
                         uint64_t old_bytes, int knob_on)
{
    if (shortfall == 0)
        return CUVS_SELF_NOT_NEEDED;
    if (!knob_on || !has_old || old_pinned)
        return CUVS_SELF_UNAVAILABLE;
    return old_bytes >= shortfall ? CUVS_SELF_REPLACE : CUVS_SELF_UNAVAILABLE;
}

/* ------------------------------------------------------------ reclaim */

void
cuvs_admit_log_add(CuvsAdmitLog *log, const CuvsAdmitRecord *r)
{
    if (r->reason == CUVS_ADMISSION_NONE)
    {
        int w = 0;
        for (int i = 0; i < log->n; i++)
        {
            const CuvsAdmitRecord *x = &log->rec[i];
            if (x->db_oid == r->db_oid && x->index_oid == r->index_oid)
                continue;
            log->rec[w++] = *x;
        }
        log->n = w;
        return;   /* an admission itself never drives F */
    }
    if (log->n == CUVS_ADMIT_LOG_CAP)
    {
        memmove(&log->rec[0], &log->rec[1],
                sizeof(log->rec[0]) * (CUVS_ADMIT_LOG_CAP - 1));
        log->n--;
    }
    log->rec[log->n++] = *r;
}

void
cuvs_admit_log_forget(CuvsAdmitLog *log, uint32_t db_oid, uint32_t index_oid)
{
    int w = 0;
    for (int i = 0; i < log->n; i++)
        if (log->rec[i].db_oid != db_oid || log->rec[i].index_oid != index_oid)
            log->rec[w++] = log->rec[i];
    log->n = w;
}

void
cuvs_admit_log_expire(CuvsAdmitLog *log, int64_t now, int64_t window_s)
{
    int w = 0;
    for (int i = 0; i < log->n; i++)
        if (now - log->rec[i].at <= window_s)
            log->rec[w++] = log->rec[i];
    log->n = w;
}

int
cuvs_reclaim_reason_counts(int reason)
{
    return reason >= CUVS_ADMISSION_AR1 && reason <= CUVS_ADMISSION_AR6;
}

static uint64_t
pct_of(uint64_t base, uint32_t pct)
{
    /* base <= 2^64; split to avoid overflow in base * pct. */
    return (base / 100) * pct + (base % 100) * pct / 100;
}

void
cuvs_reclaim_free_target(const CuvsAdmitLog *log, int gpu, int64_t now,
                         int64_t window_s, uint64_t budget_eff,
                         uint32_t cap_pct, uint64_t fixed, CuvsFreeTarget *out)
{
    memset(out, 0, sizeof(*out));
    out->f_cap = pct_of(budget_eff, cap_pct);
    if (fixed > 0)
    {
        out->src = CUVS_FSRC_FIXED;
        out->f_bytes = fixed < out->f_cap ? fixed : out->f_cap;
        return;
    }
    for (int i = 0; i < log->n; i++)
    {
        const CuvsAdmitRecord *r = &log->rec[i];
        if (r->gpu != gpu || !cuvs_reclaim_reason_counts(r->reason))
            continue;
        if (now - r->at > window_s || r->at > now)
            continue;
        if (out->src == CUVS_FSRC_NONE || r->needed > out->f_bytes)
        {
            out->src = CUVS_FSRC_BUILD;
            out->src_db = r->db_oid;
            out->src_idx = r->index_oid;
            out->f_bytes = r->needed;
        }
    }
    if (out->f_bytes > out->f_cap)
        out->f_bytes = out->f_cap;
}

void
cuvs_format_free_src(const CuvsFreeTarget *t, char *buf, size_t cap)
{
    if (t->src == CUVS_FSRC_FIXED)
        snprintf(buf, cap, "fixed");
    else if (t->src == CUVS_FSRC_BUILD)
        snprintf(buf, cap, "build:%u:%u", t->src_db, t->src_idx);
    else
        snprintf(buf, cap, "none");
}

const char *
cuvs_reclaim_trigger_name(CuvsReclaimTrigger t)
{
    switch (t)
    {
        case CUVS_TRIG_USAGE:    return "usage";
        case CUVS_TRIG_HEADROOM: return "headroom";
        case CUVS_TRIG_BOTH:     return "both";
        default:                 return "none";
    }
}

static int
usage_side(const CuvsReclaimGoal *g, uint64_t resident, int starting)
{
    if (g->mode == CUVS_RECLAIM_OFF || g->hi_pct >= 100 || g->budget_eff == 0)
        return 0;
    if (starting)
        return resident > pct_of(g->budget_eff, g->hi_pct);
    return resident > pct_of(g->budget_eff, g->lo_pct);
}

static int
headroom_side(const CuvsReclaimGoal *g, uint64_t resident)
{
    if (g->mode != CUVS_RECLAIM_HEADROOM || g->f_bytes == 0)
        return 0;
    if (resident <= pct_of(g->budget_eff, g->floor_pct))
        return 0;   /* fence F-4: floor applies to F reclaim only */
    uint64_t free_b = g->budget_eff > resident ? g->budget_eff - resident : 0;
    return free_b < g->f_bytes;
}

CuvsReclaimTrigger
cuvs_reclaim_trigger(const CuvsReclaimGoal *g, uint64_t resident)
{
    int u = usage_side(g, resident, 1);
    int h = headroom_side(g, resident);
    if (u && h) return CUVS_TRIG_BOTH;
    if (u)      return CUVS_TRIG_USAGE;
    if (h)      return CUVS_TRIG_HEADROOM;
    return CUVS_TRIG_NONE;
}

int
cuvs_reclaim_need_more(const CuvsReclaimGoal *g, CuvsReclaimTrigger active,
                       uint64_t resident)
{
    int u = (active == CUVS_TRIG_USAGE || active == CUVS_TRIG_BOTH) &&
            usage_side(g, resident, 0);
    int h = (active == CUVS_TRIG_HEADROOM || active == CUVS_TRIG_BOTH) &&
            headroom_side(g, resident);
    return u || h;
}

int
cuvs_reclaim_take(const CuvsReclaimGoal *g, CuvsReclaimTrigger active,
                  uint64_t resident, uint64_t bytes)
{
    if ((active == CUVS_TRIG_USAGE || active == CUVS_TRIG_BOTH) &&
        usage_side(g, resident, 0))
        return 1;
    if ((active == CUVS_TRIG_HEADROOM || active == CUVS_TRIG_BOTH) &&
        headroom_side(g, resident))
    {
        uint64_t after = bytes > resident ? 0 : resident - bytes;
        return after >= pct_of(g->budget_eff, g->floor_pct);
    }
    return 0;
}

int
cuvs_reclaim_plan(const CuvsEvictCand *cands, int n, const CuvsEvictParams *p,
                  int64_t now, int *order)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        if (cuvs_evict_eligible(&cands[i], now, p))
            order[k++] = i;
    sort_by_ord(cands, order, k);
    return k;
}

int
cuvs_reclaim_wait_hint(CuvsReclaimMode mode, uint64_t free_bytes,
                       uint64_t eligible_bytes, uint64_t need, uint64_t f_cap,
                       uint64_t resident, uint64_t floor_bytes)
{
    if (mode != CUVS_RECLAIM_HEADROOM || eligible_bytes == 0)
        return 0;
    if (sat_add(free_bytes, eligible_bytes) < need || need > f_cap)
        return 0;
    uint64_t evict = need > free_bytes ? need - free_bytes : 0;
    if (evict > resident)
        return 0;
    return resident - evict >= floor_bytes;
}

/* ---------------------------------------------------------- parameters */

void
cuvs_reclaim_params_default(CuvsReclaimParams *p)
{
    memset(p, 0, sizeof(*p));
    p->mode        = CUVS_RECLAIM_HEADROOM;
    p->f_fixed     = 0;
    p->t_min_s     = CUVS_EVICT_MIN_IDLE_S_DEFAULT;
    p->bonus_s     = CUVS_EVICT_HIT_BONUS_S_DEFAULT;
    p->hits_cap    = CUVS_EVICT_HIT_CAP_DEFAULT;
    p->hi_pct      = CUVS_RECLAIM_HIGH_PCT_DEFAULT;
    p->lo_pct      = CUVS_RECLAIM_LOW_PCT_DEFAULT;
    p->tick_s      = CUVS_RECLAIM_TICK_S_DEFAULT;
    p->scope.kind  = CUVS_SCOPE_NONE;
    p->pin_max_pct = CUVS_PIN_MAX_PCT_DEFAULT;
    p->f_cap_pct   = CUVS_RECLAIM_FREE_MAX_PCT_DEFAULT;
    p->window_min  = CUVS_RECLAIM_FREE_WINDOW_MIN_DEFAULT;
    p->floor_pct   = CUVS_RECLAIM_FLOOR_PCT_DEFAULT;
    p->soft_evict  = 1;
    p->self_replace = 1;
    p->usage_file  = 1;
}

static int
parse_onoff(const char *v, int *out)
{
    if (!v) return -1;
    if (strcmp(v, "on") == 0)  { *out = 1; return 0; }
    if (strcmp(v, "off") == 0) { *out = 0; return 0; }
    return -1;
}

#define ERR(...) do { if (err && errcap) snprintf(err, errcap, __VA_ARGS__); return -1; } while (0)

/* Range table for the integer knobs. */
typedef struct KnobRange {
    const char *name;
    uint64_t    lo, hi;
    int         runtime;
} KnobRange;

static const KnobRange k_ranges[] = {
    { "evict-min-idle-s",        0, 86400,      1 },  /* KB-8 */
    { "evict-hit-bonus-s",       0, 3600,       1 },  /* KB-9 */
    { "evict-hit-cap",           0, 1000,       1 },  /* KB-10 */
    { "reclaim-high-pct",        1, 100,        1 },  /* KB-12 */
    { "reclaim-low-pct",         1, 100,        1 },  /* KB-13 */
    { "reclaim-tick-s",          1, 3600,       1 },  /* KB-14 */
    { "pin-max-pct",             0, 100,        0 },  /* KB-15 */
    { "reclaim-free-bytes",      0, UINT64_MAX, 1 },  /* KB-20 */
    { "reclaim-free-max-pct",    1, 100,        0 },  /* KB-21 */
    { "reclaim-free-window-min", 1, 1440,       0 },  /* KB-22 */
    { "reclaim-floor-pct",       0, 100,        0 },  /* KB-23 */
};

static const char *k_runtime_extra[] = { "reclaim-mode" };   /* KB-19 */

int
cuvs_reclaim_name_is_runtime(const char *name)
{
    if (!name)
        return 0;
    for (size_t i = 0; i < sizeof(k_ranges) / sizeof(k_ranges[0]); i++)
        if (k_ranges[i].runtime && strcmp(k_ranges[i].name, name) == 0)
            return 1;
    for (size_t i = 0; i < sizeof(k_runtime_extra) / sizeof(k_runtime_extra[0]); i++)
        if (strcmp(k_runtime_extra[i], name) == 0)
            return 1;
    return 0;
}

int
cuvs_reclaim_flag_known(const char *name)
{
    static const char *str_knobs[] = { "reclaim-mode", "build-evict-scope",
        "load-soft-evict", "reindex-self-replace", "usage-stats-file" };
    if (!name)
        return 0;
    for (size_t i = 0; i < sizeof(str_knobs) / sizeof(str_knobs[0]); i++)
        if (strcmp(str_knobs[i], name) == 0)
            return 1;
    for (size_t i = 0; i < sizeof(k_ranges) / sizeof(k_ranges[0]); i++)
        if (strcmp(k_ranges[i].name, name) == 0)
            return 1;
    return 0;
}

static int
set_one(CuvsReclaimParams *p, const char *name, const char *value,
        char *err, size_t errcap)
{
    if (!name || !value)
        ERR("missing name or value");
    if (strcmp(name, "reclaim-mode") == 0)
    {
        if (cuvs_parse_reclaim_mode(value, &p->mode) != 0)
            ERR("--reclaim-mode must be off, usage or headroom (got '%s')", value);
        return 0;
    }
    if (strcmp(name, "build-evict-scope") == 0)
    {
        if (cuvs_parse_evict_scope(value, &p->scope) != 0)
            ERR("--build-evict-scope must be none, lru or idle:<minutes> (got '%s')", value);
        return 0;
    }
    if (strcmp(name, "load-soft-evict") == 0)
    {
        if (parse_onoff(value, &p->soft_evict) != 0)
            ERR("--load-soft-evict must be on or off (got '%s')", value);
        return 0;
    }
    if (strcmp(name, "reindex-self-replace") == 0)
    {
        if (parse_onoff(value, &p->self_replace) != 0)
            ERR("--reindex-self-replace must be on or off (got '%s')", value);
        return 0;
    }
    if (strcmp(name, "usage-stats-file") == 0)
    {
        if (parse_onoff(value, &p->usage_file) != 0)
            ERR("--usage-stats-file must be on or off (got '%s')", value);
        return 0;
    }
    for (size_t i = 0; i < sizeof(k_ranges) / sizeof(k_ranges[0]); i++)
    {
        const KnobRange *r = &k_ranges[i];
        uint64_t v;
        if (strcmp(r->name, name) != 0)
            continue;
        if (parse_u64(value, r->hi, &v) != 0 || v < r->lo)
            ERR("--%s must be an integer in %" PRIu64 "..%" PRIu64 " (got '%s')",
                name, r->lo, r->hi, value);
        if      (strcmp(name, "evict-min-idle-s") == 0)        p->t_min_s = (int64_t)v;
        else if (strcmp(name, "evict-hit-bonus-s") == 0)       p->bonus_s = (int64_t)v;
        else if (strcmp(name, "evict-hit-cap") == 0)           p->hits_cap = (uint32_t)v;
        else if (strcmp(name, "reclaim-high-pct") == 0)        p->hi_pct = (uint32_t)v;
        else if (strcmp(name, "reclaim-low-pct") == 0)         p->lo_pct = (uint32_t)v;
        else if (strcmp(name, "reclaim-tick-s") == 0)          p->tick_s = (int64_t)v;
        else if (strcmp(name, "pin-max-pct") == 0)             p->pin_max_pct = (uint32_t)v;
        else if (strcmp(name, "reclaim-free-bytes") == 0)      p->f_fixed = v;
        else if (strcmp(name, "reclaim-free-max-pct") == 0)    p->f_cap_pct = (uint32_t)v;
        else if (strcmp(name, "reclaim-free-window-min") == 0) p->window_min = (uint32_t)v;
        else if (strcmp(name, "reclaim-floor-pct") == 0)       p->floor_pct = (uint32_t)v;
        return 0;
    }
    ERR("unknown reclaim parameter '%s'", name);
}

int
cuvs_reclaim_flag_set(CuvsReclaimParams *p, const char *name,
                      const char *value, char *err, size_t errcap)
{
    return set_one(p, name, value, err, errcap);
}

int
cuvs_reclaim_params_check(const CuvsReclaimParams *p, char *err, size_t errcap)
{
    if (p->lo_pct > p->hi_pct)
        ERR("--reclaim-low-pct (%u) must not exceed --reclaim-high-pct (%u)",
            p->lo_pct, p->hi_pct);
    return 0;
}

int
cuvs_reclaim_runtime_set(CuvsReclaimParams *p, const char *name,
                         const char *value, char *err, size_t errcap)
{
    CuvsReclaimParams tmp = *p;
    if (!cuvs_reclaim_name_is_runtime(name))
        ERR("'%s' cannot be changed at runtime (restart-only or unknown)",
            name ? name : "");
    if (set_one(&tmp, name, value, err, errcap) != 0)
        return -1;
    if (cuvs_reclaim_params_check(&tmp, err, errcap) != 0)
        return -1;
    *p = tmp;
    return 0;
}

/* --------------------------------------------------------- usage stats */

#define USAGE_HDR "pg_cuvs-usage-stats v1\n"

long
cuvs_usage_stats_format(const CuvsUsageStat *s, int n, char *buf, size_t cap)
{
    size_t off = 0;
    int    w;

    w = snprintf(buf, cap, USAGE_HDR "count %d\n", n);
    if (w < 0 || (size_t)w >= cap)
        return -1;
    off = (size_t)w;
    for (int i = 0; i < n; i++)
    {
        w = snprintf(buf + off, cap - off, "%u %u %" PRId64 " %u %d\n",
                     s[i].db_oid, s[i].index_oid, s[i].last_use, s[i].hits,
                     s[i].pinned ? 1 : 0);
        if (w < 0 || (size_t)w >= cap - off)
            return -1;
        off += (size_t)w;
    }
    uint32_t crc = cuvs_crc32(buf, off);
    w = snprintf(buf + off, cap - off, "end %08x\n", crc);
    if (w < 0 || (size_t)w >= cap - off)
        return -1;
    off += (size_t)w;
    return (long)off;
}

#define BAD(...) do { if (reason && reasoncap) snprintf(reason, reasoncap, __VA_ARGS__); *n_out = 0; return -1; } while (0)

/* Read one '\n'-terminated line at *pos into line (NUL-terminated). */
static int
next_line(const char *buf, size_t len, size_t *pos, char *line, size_t cap)
{
    size_t start = *pos, i = start;
    while (i < len && buf[i] != '\n')
        i++;
    if (i >= len)
        return -1;              /* no terminator: truncated */
    size_t l = i - start;
    if (l >= cap)
        return -1;
    memcpy(line, buf + start, l);
    line[l] = '\0';
    *pos = i + 1;
    return 0;
}

int
cuvs_usage_stats_parse(const char *buf, size_t len, CuvsUsageStat *out,
                       int max, int *n_out, char *reason, size_t reasoncap)
{
    char   line[128];
    size_t pos = 0;
    long   count;
    char  *endp;

    *n_out = 0;
    if (!buf || len == 0)
        BAD("empty file");
    if (memchr(buf, '\0', len))
        BAD("NUL byte in file");
    if (len < strlen(USAGE_HDR) || memcmp(buf, USAGE_HDR, strlen(USAGE_HDR)) != 0)
        BAD("unknown format or version");
    pos = strlen(USAGE_HDR);
    if (next_line(buf, len, &pos, line, sizeof(line)) != 0 ||
        strncmp(line, "count ", 6) != 0)
        BAD("missing count line");
    errno = 0;
    count = strtol(line + 6, &endp, 10);
    if (errno || *endp != '\0' || count < 0 || line[6] == '\0' ||
        !isdigit((unsigned char)line[6]))
        BAD("bad count");
    if (count > CUVS_USAGE_STATS_MAX || count > max)
        BAD("count %ld above limit", count);

    for (long i = 0; i < count; i++)
    {
        unsigned long db, idx, hits;
        long long     lu;
        int           pin, consumed = 0;
        if (next_line(buf, len, &pos, line, sizeof(line)) != 0)
            BAD("truncated at entry %ld", i);
        if (sscanf(line, "%lu %lu %lld %lu %d%n", &db, &idx, &lu, &hits, &pin,
                   &consumed) != 5 || line[consumed] != '\0' ||
            db > UINT32_MAX || idx > UINT32_MAX || hits > UINT32_MAX ||
            lu < 0 || (pin != 0 && pin != 1) || line[0] == '-')
            BAD("bad entry %ld", i);
        out[i].db_oid = (uint32_t)db;
        out[i].index_oid = (uint32_t)idx;
        out[i].last_use = (int64_t)lu;
        out[i].hits = (uint32_t)hits;
        out[i].pinned = pin;
    }

    size_t body = pos;
    unsigned int crc_file;
    int consumed = 0;
    if (next_line(buf, len, &pos, line, sizeof(line)) != 0 ||
        sscanf(line, "end %8x%n", &crc_file, &consumed) != 1 ||
        line[consumed] != '\0' || consumed != 12)
        BAD("missing or bad trailer");
    if (pos != len)
        BAD("trailing bytes after trailer");
    if (cuvs_crc32(buf, body) != (uint32_t)crc_file)
        BAD("crc mismatch");
    *n_out = (int)count;
    return 0;
}

/* Hot-first comparator over keys that carry their stat (or none). */
typedef struct OrderItem {
    int                  key;
    const CuvsUsageStat *stat;   /* NULL = not in the file */
    uint32_t             db, idx;
} OrderItem;

static int
order_item_cmp(const void *pa, const void *pb)
{
    const OrderItem *a = pa, *b = pb;
    if ((a->stat != NULL) != (b->stat != NULL))
        return a->stat ? -1 : 1;
    if (a->stat)
    {
        if (a->stat->last_use != b->stat->last_use)
            return a->stat->last_use > b->stat->last_use ? -1 : 1;
        if (a->stat->hits != b->stat->hits)
            return a->stat->hits > b->stat->hits ? -1 : 1;
    }
    if (a->db != b->db)
        return a->db < b->db ? -1 : 1;
    if (a->idx != b->idx)
        return a->idx < b->idx ? -1 : 1;
    return a->key - b->key;
}

void
cuvs_startup_order(const CuvsLoadKey *keys, int n, const CuvsUsageStat *stats,
                   int n_stats, int *perm)
{
    if (n <= 0)
        return;
    OrderItem *it = calloc((size_t)n, sizeof(OrderItem));
    if (!it)
    {
        for (int i = 0; i < n; i++)
            perm[i] = i;
        return;
    }
    for (int i = 0; i < n; i++)
    {
        it[i].key = i;
        it[i].db = keys[i].db_oid;
        it[i].idx = keys[i].index_oid;
        it[i].stat = NULL;
        for (int s = 0; s < n_stats; s++)
            if (stats[s].db_oid == keys[i].db_oid &&
                stats[s].index_oid == keys[i].index_oid)
            {
                it[i].stat = &stats[s];
                break;
            }
    }
    qsort(it, (size_t)n, sizeof(OrderItem), order_item_cmp);
    for (int i = 0; i < n; i++)
        perm[i] = it[i].key;
    free(it);
}

/* ------------------------------------------------------------ sidecars */

static int
path_exists(const char *dir, uint32_t db, uint32_t idx, const char *ext)
{
    char        p[1024];
    struct stat st;
    int w = snprintf(p, sizeof(p), "%s/%u_%u.%s", dir, db, idx, ext);
    if (w < 0 || (size_t)w >= sizeof(p))
        return 1;   /* cannot tell: treat as present (fail closed: skip) */
    return stat(p, &st) == 0;
}

int
cuvs_entry_sidecar_kind(const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    int k = 0;
    if (path_exists(dir, db_oid, index_oid, "delta"))
        k |= CUVS_SIDECAR_DELTA;
    if (path_exists(dir, db_oid, index_oid, "tombstone"))
        k |= CUVS_SIDECAR_TOMBSTONE;
    return k;
}

const char *
cuvs_sidecar_kind_name(int kind)
{
    switch (kind)
    {
        case CUVS_SIDECAR_DELTA:     return "delta";
        case CUVS_SIDECAR_TOMBSTONE: return "tombstone";
        case CUVS_SIDECAR_BOTH:      return "both";
        default:                     return "none";
    }
}

int
cuvs_reclaim_skip_sidecar(int sidecar_kind, int is_cagra_single,
                          int file_current)
{
    return sidecar_kind != CUVS_SIDECAR_NONE && is_cagra_single && !file_current;
}
