/* Structural reachability of a live HNSW index (src/cuvs_cover_reach.h). Pure C, no PostgreSQL.
 * Pages are built by hand in the pgvector 0.8.7 layout; the expected answer comes from a plain
 * breadth first search written independently of the header. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* A counting allocator under the header: reallocation is modelled as copy-then-free, the worst case, so the peak is
 * what a real allocator may hold. The accounted peak of the walk must cover it, and nothing may leak. */
static size_t t_cur, t_peak;
static void *t_malloc(size_t n)
{
	size_t *p = malloc(n + 16);
	p[0] = n; t_cur += n; if (t_cur > t_peak) t_peak = t_cur;
	return (char *) p + 16;
}
static void *t_calloc(size_t n, size_t m) { void *p = t_malloc(n * m); memset(p, 0, n * m); return p; }
static void t_free(void *q)
{
	if (q == NULL) return;
	t_cur -= ((size_t *) ((char *) q - 16))[0];
	free((char *) q - 16);
}
#define CUVS_REACH_MALLOC(n) t_malloc(n)
#define CUVS_REACH_CALLOC(n, sz) t_calloc(n, sz)
#define CUVS_REACH_FREE(p) t_free(p)
#define CUVS_REACH_BITS_INITIAL 64u
#include "cuvs_cover_reach.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return rng_state; }

static void put16(uint8_t *p, size_t off, uint16_t v) { memcpy(p + off, &v, 2); }
static void put32(uint8_t *p, size_t off, uint32_t v) { memcpy(p + off, &v, 4); }
static void put_tid(uint8_t *p, size_t off, uint32_t blk, uint16_t o)
{
	put16(p, off, (uint16_t) (blk >> 16)); put16(p, off + 2, (uint16_t) (blk & 0xFFFF)); put16(p, off + 4, o);
}

/* ---- an in-memory index ---- */
typedef struct Index Index;
typedef void (*DamageFn)(Index *);
struct Index {
	uint8_t (*pages)[CUVS_REACH_PAGE_SIZE];
	uint32_t npages;		/* pages that exist */
	uint32_t visible;		/* what nblocks() reports */
	uint32_t delay_calls, delay_abort_after;
	uint32_t emit_calls, emit_abort_after;
	uint32_t total_reads;
	int after_start;		/* set by the first read of a run: nblocks() then reports the relation as grown */
	uint32_t reads, damage_at_read, damage_page, damage_off, damage_len;	/* a page changes while the walk runs */
	uint8_t damage_val;
	DamageFn damage_fn;			/* arbitrary change at the damage read */
	uint32_t fail_block, fail_from_read, read_count;	/* reads that fail: one block, or every read from the Nth on */
	uint64_t emitted_tids[1 << 13];
	uint32_t n_emitted_tids;
};

static uint32_t src_nblocks(void *c) { Index *ix = c; return ix->after_start ? ix->npages : ix->visible; }
static int src_read(void *c, uint32_t blk, uint8_t *dst)
{
	Index *ix = c;
	if (blk >= ix->npages) return 1;
	ix->read_count++;
	if ((ix->fail_block && blk == ix->fail_block) || (ix->fail_from_read && ix->read_count >= ix->fail_from_read)) return 1;
	if (blk == 0) ix->after_start = 1;
	ix->total_reads++;
	if (ix->damage_at_read && ++ix->reads == ix->damage_at_read)
	{
		if (ix->damage_fn) ix->damage_fn(ix);
		else memset(&ix->pages[ix->damage_page][ix->damage_off], ix->damage_val, ix->damage_len ? ix->damage_len : 1);
	}
	memcpy(dst, ix->pages[blk], CUVS_REACH_PAGE_SIZE);
	return 0;
}
static int src_delay(void *c)
{
	Index *ix = c;
	ix->delay_calls++;
	return ix->delay_abort_after && ix->delay_calls >= ix->delay_abort_after;
}
static int src_emit(void *c, uint32_t hb, uint16_t ho)
{
	Index *ix = c;
	ix->emit_calls++;
	if (ix->n_emitted_tids < (1u << 13))
		ix->emitted_tids[ix->n_emitted_tids++] = ((uint64_t) hb << 16) | ho;
	return ix->emit_abort_after && ix->emit_calls >= ix->emit_abort_after;
}

static CuvsReachStatus RUN(const CuvsReachSource *s, uint64_t limit, CuvsReachResult *r)
{
	CuvsReachStatus st;
	((Index *) s->ctx)->after_start = 0;
	((Index *) s->ctx)->read_count = 0;
	t_peak = 0;
	st = cuvs_reach_run(s, limit, 0, r);
	CHECK(t_cur == 0);			/* nothing leaks, whatever the status */
	CHECK(t_peak == r->mem_bytes);		/* the accounted peak is what the allocator held, to the byte */
	return st;
}

/* A source whose big arrays are tracked: RAM requests and spill requests are counted separately, and a request can
 * be refused. This models the PostgreSQL side (RAM = palloc, spill = an unlinked mapped file). */
static size_t sp_ram_cur, sp_ram_peak, sp_spill_cur, sp_spill_peak, sp_spill_calls;
static void *sp_alloc(void *c, size_t bytes, int spill)
{
	size_t *p;
	(void) c;
	p = calloc(1, bytes + 16);
	p[0] = bytes;
	if (spill) { sp_spill_cur += bytes; sp_spill_calls++; if (sp_spill_cur > sp_spill_peak) sp_spill_peak = sp_spill_cur; }
	else { sp_ram_cur += bytes; if (sp_ram_cur > sp_ram_peak) sp_ram_peak = sp_ram_cur; }
	return (char *) p + 16;
}
static void sp_free(void *c, void *q, size_t bytes, int spill)
{
	(void) c; (void) bytes;
	if (spill) sp_spill_cur -= ((size_t *) ((char *) q - 16))[0]; else sp_ram_cur -= ((size_t *) ((char *) q - 16))[0];
	free((char *) q - 16);
}
static CuvsReachStatus RUN_SPILL(const CuvsReachSource *s0, uint64_t ram, uint64_t spill, CuvsReachResult *r)
{
	CuvsReachSource s = *s0;
	CuvsReachStatus st;
	s.big_alloc = sp_alloc;
	s.big_free = sp_free;
	((Index *) s.ctx)->after_start = 0;
	((Index *) s.ctx)->read_count = 0;
	((Index *) s.ctx)->total_reads = 0;
	t_peak = 0; sp_ram_peak = sp_spill_peak = sp_spill_calls = 0;
	st = cuvs_reach_run(&s, ram, spill, r);
	CHECK(t_cur == 0 && sp_ram_cur == 0 && sp_spill_cur == 0);		/* nothing leaks */
	CHECK(sp_ram_peak + t_peak == r->mem_bytes);			/* RAM actually held (page scratch via the macros) */
	CHECK(sp_spill_peak == r->spilled_bytes);
	CHECK(r->mem_bytes <= ram);
	CHECK(r->spilled_bytes <= spill);
	return st;
}

static CuvsReachSource source_of(Index *ix)
{
	CuvsReachSource s = {ix, src_nblocks, src_read, src_delay, src_emit, NULL, NULL};
	return s;
}

/* page builder: items grow down from pd_special, line pointers up from the header */
static void page_init(uint8_t *page)
{
	memset(page, 0, CUVS_REACH_PAGE_SIZE);
	put16(page, 12, CUVS_REACH_PAGE_HEADER);
	put16(page, 14, CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE);
	put16(page, 16, CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE);
	put32(page, CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, CUVS_REACH_NO_BLOCK);
	put16(page, CUVS_REACH_PAGE_ID_AT, CUVS_REACH_PGV_PAGE_ID);
}

static int page_add(uint8_t *page, const uint8_t *tuple, uint32_t len)
{
	uint16_t lower, upper;
	uint32_t alen = (len + 7u) & ~7u, lp;
	int off;
	memcpy(&lower, page + 12, 2); memcpy(&upper, page + 14, 2);
	if ((uint32_t) upper < alen + lower + 4u) return 0;
	upper = (uint16_t) (upper - alen);
	memcpy(page + upper, tuple, len);
	lp = (uint32_t) upper | (1u << 15) | (len << 17);
	put32(page, lower, lp);
	off = (lower - CUVS_REACH_PAGE_HEADER) / 4 + 1;
	lower = (uint16_t) (lower + 4);
	put16(page, 12, lower); put16(page, 14, upper);
	return off;
}

/* a graph: node i has `out[i]` neighbors (node indexes); node->(block, offset) is decided by the builder */
#define MAXN 700
#define MAXDEG 12
typedef struct Graph {
	int n;
	int deg[MAXN];
	int adj[MAXN][MAXDEG];
	int deleted[MAXN];
	int extra[MAXN];	/* additional heap TIDs of the element (0..3) */
	int entry;
} Graph;

typedef struct Layout {
	uint32_t blk[MAXN];
	int eoff[MAXN];		/* element offset */
	int noff[MAXN];		/* neighbor tuple offset */
	uint32_t hblk[MAXN];
	uint16_t hoff[MAXN];
	int hextra[MAXN];
} Layout;

/* per_page elements share a page with their neighbor tuples; `permute` shuffles the block numbers along the chain */
static void build_index(Index *ix, const Graph *g, Layout *lay, int per_page, int permute)
{
	int nb = (g->n + per_page - 1) / per_page, i, b;
	uint32_t *order = malloc(sizeof(uint32_t) * (nb + 1));
	uint8_t tuple[512];
	ix->npages = (uint32_t) nb + 1;
	ix->visible = ix->npages;
	ix->pages = calloc(ix->npages, CUVS_REACH_PAGE_SIZE);
	for (b = 0; b < nb; b++) order[b] = (uint32_t) b + 1;
	if (permute)
		for (b = nb - 1; b > 0; b--) { int j = (int) (rnd() % (uint64_t) (b + 1)); uint32_t t = order[b]; order[b] = order[j]; order[j] = t; }
	/* chain: HEAD (block 1) must start the chain, so keep order[0] == 1 */
	if (permute) {
		for (b = 1; b < nb; b++) if (order[b] == 1) { order[b] = order[0]; order[0] = 1; break; }
	}
	for (b = 0; b < nb; b++) {
		page_init(ix->pages[order[b]]);
		put32(ix->pages[order[b]], CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, b + 1 < nb ? order[b + 1] : CUVS_REACH_NO_BLOCK);
	}
	/* first pass: place tuples to know offsets; neighbors are filled in a second pass */
	for (i = 0; i < g->n; i++) {
		uint8_t *pg = ix->pages[order[i / per_page]];
		memset(tuple, 0, sizeof(tuple));
		tuple[0] = CUVS_REACH_ELEMENT_TYPE;
		tuple[2] = (uint8_t) g->deleted[i];
		lay->blk[i] = order[i / per_page];
		lay->hblk[i] = 5000u + (uint32_t) i;
		lay->hoff[i] = (uint16_t) (1 + i % 200);
		put_tid(tuple, 4, lay->hblk[i], lay->hoff[i]);	/* heaptids[0]; the rest stay invalid (offset 0) unless the element has more */
		lay->hextra[i] = g->extra[i];
		{ int x; for (x = 1; x <= g->extra[i]; x++) put_tid(tuple, 4 + 6 * x, 70000u + (uint32_t) i * 4u + (uint32_t) x, (uint16_t) (x + 1)); }
		lay->eoff[i] = page_add(pg, tuple, 72 + 16);
		memset(tuple, 0, sizeof(tuple));
		tuple[0] = CUVS_REACH_NEIGHBOR_TYPE;
		lay->noff[i] = page_add(pg, tuple, 4 + 6 * MAXDEG);	/* room for MAXDEG neighbors; count set below */
		if (lay->eoff[i] == 0 || lay->noff[i] == 0) { fprintf(stderr, "builder: page full\n"); exit(2); }
	}
	for (i = 0; i < g->n; i++) {
		uint8_t *pg = ix->pages[lay->blk[i]];
		uint32_t lp, at;
		int k;
		memcpy(&lp, pg + CUVS_REACH_PAGE_HEADER + 4 * (lay->eoff[i] - 1), 4);
		at = lp & 0x7FFF;
		put_tid(pg, at + 64, lay->blk[i], (uint16_t) lay->noff[i]);	/* neighbortid: same page */
		memcpy(&lp, pg + CUVS_REACH_PAGE_HEADER + 4 * (lay->noff[i] - 1), 4);
		at = lp & 0x7FFF;
		put16(pg, at + 2, (uint16_t) g->deg[i]);
		for (k = 0; k < g->deg[i]; k++)
			put_tid(pg, at + 4 + 6 * k, lay->blk[g->adj[i][k]], (uint16_t) lay->eoff[g->adj[i][k]]);
	}
	/* metapage */
	put32(ix->pages[0], 24, CUVS_REACH_PGV_MAGIC);
	put32(ix->pages[0], 28, 1);
	put32(ix->pages[0], CUVS_REACH_META_ENTRY_BLK, lay->blk[g->entry]);
	put16(ix->pages[0], CUVS_REACH_META_ENTRY_OFF, (uint16_t) lay->eoff[g->entry]);
	put16(ix->pages[0], 12, 52); put16(ix->pages[0], 14, 8184); put16(ix->pages[0], 16, 8184);
	free(order);
}

/* independent oracle: plain BFS over node indexes */
static void oracle(const Graph *g, const int *in_scope, int *reached)
{
	int queue[MAXN], qh = 0, qt = 0, i, k;
	for (i = 0; i < g->n; i++) reached[i] = 0;
	if (in_scope[g->entry] && !g->deleted[g->entry]) { reached[g->entry] = 1; queue[qt++] = g->entry; }
	while (qh < qt) {
		int u = queue[qh++];
		for (k = 0; k < g->deg[u]; k++) {
			int v = g->adj[u][k];
			if (in_scope[v] && !g->deleted[v] && !reached[v]) { reached[v] = 1; queue[qt++] = v; }
		}
	}
}

static void random_graph(Graph *g, int n, int maxdeg, int del_percent)
{
	int i, k;
	memset(g, 0, sizeof(*g));
	g->n = n;
	for (i = 0; i < n; i++) {
		g->deg[i] = (int) (rnd() % (uint64_t) (maxdeg + 1));
		for (k = 0; k < g->deg[i]; k++) g->adj[i][k] = (int) (rnd() % (uint64_t) n);
		g->deleted[i] = (int) (rnd() % 100) < del_percent;
		g->extra[i] = (rnd() % 3 == 0) ? (int) (rnd() % 4) : 0;
	}
	g->entry = (int) (rnd() % (uint64_t) n);
}

static void free_index(Index *ix) { free(ix->pages); }

static int tid_expected(const Layout *lay, const uint64_t *got, uint32_t ngot, int i)
{
	/* 1 if every heap TID of the element was handed out, 0 if none was; -1 if only some (a bug) */
	uint32_t k;
	int x, found = 0, want = 1 + lay->hextra[i];
	for (x = 0; x < want; x++) {
		uint64_t t = x == 0 ? (((uint64_t) lay->hblk[i] << 16) | lay->hoff[i])
				    : (((uint64_t) (70000u + (uint32_t) i * 4u + (uint32_t) x) << 16) | (uint64_t) (x + 1));
		for (k = 0; k < ngot; k++) if (got[k] == t) { found++; break; }
	}
	return found == want ? 1 : found == 0 ? 0 : -1;
}

/* make block 1 look like it gained a third line pointer that points at a copy of its element: its slot would be the
 * first slot of block 2 if the width recorded in pass A were not applied */
static void grow_block_1(Index *ix)
{
	uint8_t *pg = ix->pages[1];
	uint16_t lower = cuvs_reach_u16(pg, 12);
	uint32_t lp0 = cuvs_reach_u32(pg, CUVS_REACH_PAGE_HEADER);
	/* the first line pointer's tuple becomes the second line pointer's tuple too */
	put32(pg, lower, lp0);
	put16(pg, 12, (uint16_t) (lower + 4));
}

/* A relation that exists only as a generator: nblocks pages in a chain, each with 100 line pointers that all point at one
 * 4-byte neighbor tuple. It makes the memory arithmetic visible at a scale where the fixed 8 kB of scratch no longer dominates. */
typedef struct Synth { uint32_t nblocks; } Synth;
static uint32_t synth_nblocks(void *c) { return ((Synth *) c)->nblocks; }
static int synth_read(void *c, uint32_t blk, uint8_t *dst)
{
	Synth *sy = c;
	memset(dst, 0, CUVS_REACH_PAGE_SIZE);
	if (blk == 0) { put32(dst, 24, CUVS_REACH_PGV_MAGIC); put16(dst, 12, 52); put16(dst, 14, 8184); put16(dst, 16, 8184); return 0; }
	{
		int i;
		put16(dst, 12, 24 + 4 * 100); put16(dst, 14, 8000); put16(dst, 16, 8184);
		dst[8000] = CUVS_REACH_NEIGHBOR_TYPE;
		for (i = 0; i < 100; i++)
			put32(dst, 24 + 4 * i, 8000u | (1u << 15) | (4u << 17));
	}
	put32(dst, 8184, blk + 1 < sy->nblocks ? blk + 1 : CUVS_REACH_NO_BLOCK);
	put16(dst, CUVS_REACH_PAGE_ID_AT, CUVS_REACH_PGV_PAGE_ID);
	return 0;
}

int main(void)
{
	/* ---- a small hand-made graph: 0 -> 1 -> 2, 3 is live but nobody points to it, 4 is deleted ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
		memset(&g, 0, sizeof(g)); memset(&ix, 0, sizeof(ix));
		g.n = 5; g.entry = 0;
		g.deg[0] = 1; g.adj[0][0] = 1; g.deg[1] = 2; g.adj[1][0] = 2; g.adj[1][1] = 4; g.deg[2] = 1; g.adj[2][0] = 0;
		g.deg[3] = 1; g.adj[3][0] = 0; g.deleted[4] = 1;
		build_index(&ix, &g, &lay, 2, 0);
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK);
		CHECK(r.elements == 5 && r.live == 4 && r.reached == 3 && r.unreached == 1);
		CHECK(r.emitted == 1 && ix.n_emitted_tids == 1 && tid_expected(&lay, ix.emitted_tids, ix.n_emitted_tids, 3));
		CHECK(r.nblocks == ix.npages && r.mem_bytes > 0 && r.mem_bytes <= (1 << 20));
		free_index(&ix);
	}

	/* ---- random graphs against the oracle: every page shape, chain order, deletion rate ---- */
	{
		int trial;
		uint32_t mismatches = 0;
		for (trial = 0; trial < 1500; trial++) {
			Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
			int reached[MAXN], scope[MAXN], i, per_page = 1 + (int) (rnd() % 14), live = 0, want_reached = 0, want_emitted = 0;
			memset(&ix, 0, sizeof(ix));
			random_graph(&g, 1 + (int) (rnd() % 300), 1 + (int) (rnd() % MAXDEG), (int) (rnd() % 40));
			build_index(&ix, &g, &lay, per_page, (int) (rnd() & 1));
			for (i = 0; i < g.n; i++) scope[i] = 1;
			oracle(&g, scope, reached);
			for (i = 0; i < g.n; i++) { live += !g.deleted[i]; want_reached += reached[i]; if (!g.deleted[i] && !reached[i]) want_emitted += 1 + g.extra[i]; }
			s = source_of(&ix);
			if (RUN(&s, 1 << 24, &r) != CUVS_REACH_OK || r.elements != (uint64_t) g.n || r.live != (uint64_t) live ||
			    r.reached != (uint64_t) want_reached || r.unreached != (uint64_t) (live - want_reached) || r.emitted != (uint64_t) want_emitted)
				mismatches++;
			for (i = 0; i < g.n; i++) {
				int want = !g.deleted[i] && !reached[i];
				if (want != tid_expected(&lay, ix.emitted_tids, ix.n_emitted_tids, i)) mismatches++;
			}
			free_index(&ix);
		}
		CHECK(mismatches == 0);
	}

	/* ---- the relation size read at the start is a hard bound: later blocks do not exist for the walk ---- */
	{
		int trial;
		uint32_t mismatches = 0;
		for (trial = 0; trial < 400; trial++) {
			Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
			int reached[MAXN], scope[MAXN], i, per_page = 1 + (int) (rnd() % 6), want_reached = 0, live = 0;
			memset(&ix, 0, sizeof(ix));
			random_graph(&g, 20 + (int) (rnd() % 200), 1 + (int) (rnd() % MAXDEG), (int) (rnd() % 20));
			build_index(&ix, &g, &lay, per_page, 0);	/* block order = chain order: blocks >= visible are cut */
			ix.visible = 2 + (uint32_t) (rnd() % (uint64_t) (ix.npages - 1));
			for (i = 0; i < g.n; i++) { scope[i] = lay.blk[i] < ix.visible; live += scope[i] && !g.deleted[i]; }
			if (!scope[g.entry]) g.entry = 0, put32(ix.pages[0], CUVS_REACH_META_ENTRY_BLK, lay.blk[0]), put16(ix.pages[0], CUVS_REACH_META_ENTRY_OFF, (uint16_t) lay.eoff[0]);
			oracle(&g, scope, reached);
			for (i = 0; i < g.n; i++) want_reached += reached[i];
			s = source_of(&ix);
			if (RUN(&s, 1 << 24, &r) != CUVS_REACH_OK || r.live != (uint64_t) live || r.reached != (uint64_t) want_reached)
				mismatches++;
			for (i = 0; i < g.n; i++)
				if ((scope[i] && !g.deleted[i] && !reached[i]) != tid_expected(&lay, ix.emitted_tids, ix.n_emitted_tids, i)) mismatches++;
			free_index(&ix);
		}
		CHECK(mismatches == 0);
	}

	/* ---- memory limit: no result and an exact status when the limit is too small; the accounted peak never exceeds it ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r, big; CuvsReachSource s;
		uint64_t lim;
		memset(&ix, 0, sizeof(ix));
		random_graph(&g, 300, 8, 5);
		build_index(&ix, &g, &lay, 6, 0);
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 24, &big) == CUVS_REACH_OK && big.mem_bytes > 0);
		for (lim = 0; lim < big.mem_bytes; lim += (big.mem_bytes / 17) + 1) {
			CHECK(RUN(&s, lim, &r) == CUVS_REACH_OVER_LIMIT);
			CHECK(r.reached == 0 && r.unreached == 0 && r.emitted == 0 && r.mem_bytes <= lim);
		}
		CHECK(RUN(&s, big.mem_bytes, &r) == CUVS_REACH_OK && r.mem_bytes == big.mem_bytes);
		CHECK(RUN(&s, big.mem_bytes - 1, &r) == CUVS_REACH_OVER_LIMIT);
		free_index(&ix);
	}

	/* ---- not an hnsw index ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
		memset(&ix, 0, sizeof(ix));
		random_graph(&g, 30, 4, 0);
		build_index(&ix, &g, &lay, 5, 0);
		s = source_of(&ix);
		put32(ix.pages[0], 24, 0);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_NOT_HNSW);
		put32(ix.pages[0], 24, CUVS_REACH_PGV_MAGIC);
		ix.visible = 1;
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_NOT_HNSW);
		ix.visible = ix.npages;
		put16(ix.pages[1], 16, 100);	/* block 1 is not an hnsw page */
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_NOT_HNSW);
		free_index(&ix);
	}

	/* ---- hook aborts ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
		memset(&ix, 0, sizeof(ix));
		random_graph(&g, 300, 6, 0);
		g.deg[g.entry] = 0;	/* force unreached nodes */
		build_index(&ix, &g, &lay, 2, 0);
		s = source_of(&ix);
		ix.delay_abort_after = 1;
		CHECK(RUN(&s, 1 << 24, &r) == CUVS_REACH_ABORTED && ix.delay_calls == 1);
		ix.delay_abort_after = 0; ix.emit_abort_after = 3; ix.emit_calls = 0;
		CHECK(RUN(&s, 1 << 24, &r) == CUVS_REACH_ABORTED && ix.emit_calls == 3 && r.emitted == 0);
		free_index(&ix);
	}

	/* ---- hostile pages, one defect each; the result must stay within what the sane graph allows ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
		uint32_t at, lp, k;
		memset(&ix, 0, sizeof(ix));
		memset(&g, 0, sizeof(g));
		g.n = 4; g.entry = 0;
		g.deg[0] = 2; g.adj[0][0] = 1; g.adj[0][1] = 2; g.deg[1] = 1; g.adj[1][0] = 3;
		build_index(&ix, &g, &lay, 2, 0);
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 4);

		/* chain loop: block 2 points back to block 1 */
		/* every one of these leaves rows behind the break unseen, so there is no result (a caller must not certify) */
		put32(ix.pages[2], CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, 1);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED && r.reached == 0 && r.unreached == 0);
		put32(ix.pages[2], CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, 2);	/* self loop */
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
		put32(ix.pages[2], CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, 0x7FFFFFFFu);	/* far outside */
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
		put16(ix.pages[0], 12, CUVS_REACH_PAGE_HEADER); put16(ix.pages[0], 14, CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE);	/* an empty page header */
		put16(ix.pages[0], 16, CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE);
		put32(ix.pages[0], CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, CUVS_REACH_NO_BLOCK);	/* the metapage would end the chain quietly */
		put32(ix.pages[2], CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, 0);	/* back to the metapage */
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
		put32(ix.pages[2], CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, CUVS_REACH_NO_BLOCK);

		/* entry point outside the relation / on a slot that is not an element */
		put32(ix.pages[0], CUVS_REACH_META_ENTRY_BLK, 99999);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 0 && r.unreached == 4);
		put32(ix.pages[0], CUVS_REACH_META_ENTRY_BLK, lay.blk[0]);
		put16(ix.pages[0], CUVS_REACH_META_ENTRY_OFF, 60000);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 0);
		put16(ix.pages[0], CUVS_REACH_META_ENTRY_OFF, (uint16_t) lay.eoff[0]);

		/* an item whose length reaches past the page, then past the special area, then too short */
		memcpy(&lp, ix.pages[lay.blk[1]] + CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[1] - 1), 4);
		/* element 1 would be skipped as "no element at all": not live, and whatever is unreachable behind it would be missing
		 * from the keys. A graph page holds nothing but element and neighbor tuples, so this is damage, not a skip. */
		for (k = 0; k < 4; k++) {
			uint32_t bad = k == 3 ? ((lp & ~(3u << 15)) | (2u << 15))	/* flags: not a normal item */
					      : (lp & 0x1FFFFu) | ((k == 0 ? 0x7FFFu : k == 1 ? 8000u : 10u) << 17);
			put32(ix.pages[lay.blk[1]], CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[1] - 1), bad);
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED && r.reached == 0 && r.unreached == 0 && r.emitted == 0);
		}
		/* the tuple type byte is neither 1 nor 2; the length of an element tuple below its fixed part; a line pointer that is
		 * unused (flags 0) or dead (3) */
		put32(ix.pages[lay.blk[1]], CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[1] - 1), lp);
		{
			uint32_t at1 = lp & 0x7FFF, k2;
			uint8_t keep = ix.pages[lay.blk[1]][at1];
			ix.pages[lay.blk[1]][at1] = 9;
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
			ix.pages[lay.blk[1]][at1] = keep;
			put32(ix.pages[lay.blk[1]], CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[1] - 1), (lp & 0x1FFFFu) | (3u << 17));	/* length 3 */
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
			for (k2 = 0; k2 < 2; k2++) {
				put32(ix.pages[lay.blk[1]], CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[1] - 1), (lp & ~(3u << 15)) | ((k2 ? 3u : 0u) << 15));
				CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
			}
			/* a neighbor tuple shorter than its fixed part */
			put32(ix.pages[lay.blk[1]], CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[1] - 1), lp);
			{
				uint32_t nlp;
				memcpy(&nlp, ix.pages[lay.blk[0]] + CUVS_REACH_PAGE_HEADER + 4 * (lay.noff[0] - 1), 4);
				put32(ix.pages[lay.blk[0]], CUVS_REACH_PAGE_HEADER + 4 * (lay.noff[0] - 1), (nlp & 0x1FFFFu) | (3u << 17));
				CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
				put32(ix.pages[lay.blk[0]], CUVS_REACH_PAGE_HEADER + 4 * (lay.noff[0] - 1), nlp);
			}
		}
		put32(ix.pages[lay.blk[1]], CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[1] - 1), lp);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 4);

		/* neighbor count far above what the item can hold; neighbortid to a bad block */
		memcpy(&lp, ix.pages[lay.blk[0]] + CUVS_REACH_PAGE_HEADER + 4 * (lay.noff[0] - 1), 4);
		at = lp & 0x7FFF;
		put16(ix.pages[lay.blk[0]], at + 2, 0xFFFF);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached <= 4);
		put16(ix.pages[lay.blk[0]], at + 2, 2);
		memcpy(&lp, ix.pages[lay.blk[0]] + CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[0] - 1), 4);
		at = lp & 0x7FFF;
		put_tid(ix.pages[lay.blk[0]], at + 64, 0xFFFFFFF0u, 1);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 1);
		put_tid(ix.pages[lay.blk[0]], at + 64, lay.blk[0], (uint16_t) lay.noff[0]);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 4);

		/* header fields that no sane page has: upper below lower with every line pointer valid, upper beyond the special area,
		 * and a chain page that is all zeros (no page id left to be noticed by the check on the pages the chain skipped) */
		{
			uint16_t keep_upper = cuvs_reach_u16(ix.pages[lay.blk[2]], 14);
			uint8_t saved_page[CUVS_REACH_PAGE_SIZE];

			put16(ix.pages[lay.blk[2]], 14, (uint16_t) (cuvs_reach_u16(ix.pages[lay.blk[2]], 12) - 1u));	/* upper below lower */
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
			put16(ix.pages[lay.blk[2]], 14, CUVS_REACH_PAGE_SIZE - 2);
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
			put16(ix.pages[lay.blk[2]], 14, keep_upper);
			memcpy(saved_page, ix.pages[lay.blk[2]], sizeof(saved_page));
			memset(ix.pages[lay.blk[2]], 0, CUVS_REACH_PAGE_SIZE);
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
			memcpy(ix.pages[lay.blk[2]], saved_page, sizeof(saved_page));
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 4);
		}

		/* header fields that no sane page has */
		put16(ix.pages[lay.blk[2]], 12, 8000); put16(ix.pages[lay.blk[2]], 14, 100);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);	/* the chain breaks at the bad page: rows behind it were not seen */
		free_index(&ix);
	}

	/* ---- the chain is cut: a page's next pointer says "end", but graph pages with elements follow in the relation. Everything
	 * behind the cut is missing from the count, so it is damage; ledger pages and never-initialised pages that the chain does
	 * not reach are not ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
		uint8_t saved[CUVS_REACH_PAGE_SIZE];
		memset(&ix, 0, sizeof(ix));
		memset(&g, 0, sizeof(g));
		g.n = 6; g.entry = 0;	/* one element per block: blocks 1..6 */
		g.deg[0] = 1; g.adj[0][0] = 1; g.deg[1] = 1; g.adj[1][0] = 2;
		build_index(&ix, &g, &lay, 1, 0);
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.live == 6 && r.reached == 3 && r.unreached == 3);
		put32(ix.pages[3], CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, CUVS_REACH_NO_BLOCK);	/* cut after block 3 */
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED && r.reached == 0 && r.unreached == 0 && r.emitted == 0);
		put32(ix.pages[3], CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, 4);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.live == 6);
		/* the same for a cut that leaves exactly one page behind it, which is the last page of the relation */
		memcpy(saved, ix.pages[5], sizeof(saved));
		put32(ix.pages[5], CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, CUVS_REACH_NO_BLOCK);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
		memcpy(ix.pages[5], saved, sizeof(saved));
		/* the last page is a ledger page (page_id 0xC0E1) or a zero page: not graph, not damage; with the chain cut before it */
		memcpy(saved, ix.pages[6], sizeof(saved));
		put32(ix.pages[5], CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE, CUVS_REACH_NO_BLOCK);
		put16(ix.pages[6], CUVS_REACH_PAGE_ID_AT, 0xC0E1);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.live == 5);
		memset(ix.pages[6], 0, CUVS_REACH_PAGE_SIZE);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.live == 5);
		/* an unvisited graph page with no items is not a loss (an extension that was initialised and then abandoned) */
		memcpy(ix.pages[6], saved, sizeof(saved));
		put16(ix.pages[6], 12, CUVS_REACH_PAGE_HEADER);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.live == 5);
		/* an unvisited page that cannot be read at all */
		ix.fail_block = 6;
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
		ix.fail_block = 0;
		/* a page appended after the size was read is not looked at, whatever it holds */
		memcpy(ix.pages[6], saved, sizeof(saved));
		ix.visible = 6;
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.live == 5);
		free_index(&ix);
	}

	/* ---- the index changes while the walk runs: a neighbor list or an element can turn into something else between
	 * the chain pass and the search; the search must not follow what it can no longer recognise ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
		uint32_t lp, at, nread;
		memset(&ix, 0, sizeof(ix));
		memset(&g, 0, sizeof(g));
		g.n = 4; g.entry = 0;	/* 0 -> 1 -> 3, 2 is live and unreachable */
		g.deg[0] = 1; g.adj[0][0] = 1; g.deg[1] = 1; g.adj[1][0] = 3;
		build_index(&ix, &g, &lay, 2, 0);
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 3 && r.unreached == 1);
		/* reads: block 0, then the chain (2 pages), then the search; damage the third search read or earlier */
		nread = 1 + 2;
		memcpy(&lp, ix.pages[lay.blk[1]] + CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[1] - 1), 4);
		at = lp & 0x7FFF;
		ix.damage_at_read = nread + 1; ix.reads = 0;	/* before the first search read: the element type of node 1 becomes 9 */
		ix.damage_page = lay.blk[1]; ix.damage_off = at; ix.damage_val = 9;
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 2);
		ix.pages[lay.blk[1]][at] = CUVS_REACH_ELEMENT_TYPE;
		memcpy(&lp, ix.pages[lay.blk[0]] + CUVS_REACH_PAGE_HEADER + 4 * (lay.noff[0] - 1), 4);
		at = lp & 0x7FFF;
		ix.damage_at_read = nread + 1; ix.reads = 0;	/* the neighbor tuple type of node 0 becomes 9 */
		ix.damage_page = lay.blk[0]; ix.damage_off = at; ix.damage_val = 9;
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 1);
		ix.pages[lay.blk[0]][at] = CUVS_REACH_NEIGHBOR_TYPE;
		ix.damage_at_read = 0;
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 3);

		/* a neighbor tuple that exists but lies beyond the size read at the start is not followed */
		memset(&g, 0, sizeof(g));
		g.n = 4; g.entry = 0;
		g.deg[0] = 0; g.deg[2] = 1; g.adj[2][0] = 1;	/* node 2 (block 2) lists node 1 (block 1) */
		free_index(&ix);
		memset(&ix, 0, sizeof(ix));
		build_index(&ix, &g, &lay, 2, 0);
		memcpy(&lp, ix.pages[lay.blk[0]] + CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[0] - 1), 4);
		at = lp & 0x7FFF;
		put_tid(ix.pages[lay.blk[0]], at + 64, lay.blk[2], (uint16_t) lay.noff[2]);	/* node 0 borrows node 2's list */
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 2);	/* all blocks visible: 0 -> 1 */
		ix.visible = 2;	/* block 2 exists but was appended after the start */
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 1);
		free_index(&ix);
	}

	/* ---- memory does not depend on how many elements one search reaches or how long the paths are (no queue), and
	 * the page reads are bounded: a path of 300 elements has 300 rounds, each touching one block ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult path, entry_only; CuvsReachSource s;
		int i;
		memset(&ix, 0, sizeof(ix));
		memset(&g, 0, sizeof(g));
		g.n = 300; g.entry = 0;
		for (i = 0; i < 299; i++) { g.deg[i] = 1; g.adj[i][0] = i + 1; }
		build_index(&ix, &g, &lay, 5, 0);
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 24, &path) == CUVS_REACH_OK && path.reached == 300);
		CHECK(ix.total_reads <= 1 + 60 + 3 * 300);	/* meta + chain + at most three page reads per round */
		free_index(&ix);
		memset(&ix, 0, sizeof(ix));
		g.deg[0] = 0;
		build_index(&ix, &g, &lay, 5, 0);
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 24, &entry_only) == CUVS_REACH_OK && entry_only.reached == 1);
		CHECK(path.mem_bytes == entry_only.mem_bytes);
		free_index(&ix);
	}

	/* ---- a block with many frontier elements is read once per round, not once per element ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
		int i;
		memset(&ix, 0, sizeof(ix));
		memset(&g, 0, sizeof(g));
		g.n = 200; g.entry = 0;
		g.deg[0] = MAXDEG;
		for (i = 0; i < MAXDEG; i++) g.adj[0][i] = 1 + i;
		for (i = 1; i <= MAXDEG; i++) { g.deg[i] = 1; g.adj[i][0] = 13 + (i - 1) * 15; }	/* 12 elements fan out to 12 more, spread over the pages */
		build_index(&ix, &g, &lay, 20, 0);	/* 10 blocks of 20 elements */
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 24, &r) == CUVS_REACH_OK && r.reached == 1 + MAXDEG + MAXDEG);
		CHECK(ix.total_reads <= 1 + 10 + 3 * 10);	/* meta + chain + at most one element page per block and round and a few neighbor pages */
		free_index(&ix);
	}

	/* ---- a chain that cannot be followed to its end gives no result: rows behind the break were not seen, and a
	 * caller that certified on OK would certify an index it did not look at (review H2, the midchain repro) ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
		memset(&ix, 0, sizeof(ix));
		memset(&g, 0, sizeof(g));
		g.n = 3; g.entry = 0;	/* one element per block: blocks 1, 2, 3; 0 -> 1, element 2 is live and unreachable */
		g.deg[0] = 1; g.adj[0][0] = 1;
		build_index(&ix, &g, &lay, 1, 0);
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.live == 3 && r.reached == 2 && r.unreached == 1);
		/* the header of block 2 is zeroed */
		{
			uint8_t saved[CUVS_REACH_PAGE_HEADER];
			memcpy(saved, ix.pages[2], sizeof(saved));
			memset(ix.pages[2], 0, CUVS_REACH_PAGE_HEADER);
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED && r.reached == 0 && r.unreached == 0 && r.emitted == 0);
			memcpy(ix.pages[2], saved, sizeof(saved));
		}
		/* block 2 cannot be read, from the start and from a given read on */
		ix.fail_block = 2;
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
		ix.fail_block = 0;
		/* the first element page cannot be read or is not an hnsw page: that is "not hnsw", not damage */
		ix.fail_block = 1;
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_NOT_HNSW);
		ix.fail_block = 0;
		/* pass C meets a page it can no longer read (the walk and the search succeeded): the keys of that page would be
		 * missing, so no result. Reads of a clean run: meta 1 + chain 3 + search 2 (one block per round, the neighbor tuple shares the page) + pass C 3 */
		{
			uint32_t total;
			ix.read_count = 0;
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK);
			total = ix.read_count;
			CHECK(total == 9);
			ix.fail_from_read = total;	/* the last pass C page */
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
			ix.fail_from_read = 0;
		}
		/* the chain is rewired between the passes so that pass C sees fewer pages than pass A */
		{
			uint32_t special_at = CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE;
			ix.damage_at_read = 1 + 3 + 2 + 1;	/* meta + chain + search + first pass C page */
			ix.reads = 0; ix.damage_page = 1; ix.damage_off = special_at; ix.damage_len = 4; ix.damage_val = 0xFF;
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
			ix.damage_at_read = 0; ix.damage_len = 0;
			/* restore block 1's link */
			put32(ix.pages[1], special_at, 2);
		}
		/* pass C meets an unreached live element that is no element any more: pass A saw it and tuples are only ever added, so it
		 * is damage; skipping it would leave that row out of the keys */
		{
			uint32_t lp2, at2;
			memcpy(&lp2, ix.pages[lay.blk[2]] + CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[2] - 1), 4);
			at2 = lp2 & 0x7FFF;
			ix.damage_at_read = 9; ix.reads = 0;	/* the third pass C page */
			ix.damage_page = lay.blk[2]; ix.damage_off = at2; ix.damage_val = 9;
			CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED && r.emitted == 0);
			ix.damage_at_read = 0;
			ix.pages[lay.blk[2]][at2] = CUVS_REACH_ELEMENT_TYPE;
		}
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK);
		free_index(&ix);
	}

	/* ---- the delay hook is consulted every 256 pages of the chain, every 256 block visits of the search and every
	 * 256 pages of the emit pass; any of them can stop the walk ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
		uint32_t k;
		memset(&ix, 0, sizeof(ix));
		memset(&g, 0, sizeof(g));
		g.n = 600; g.entry = 0;	/* 600 blocks, the entry reaches nothing: chain 3 calls, search 1 call, emit 3 calls */
		build_index(&ix, &g, &lay, 1, 0);
		s = source_of(&ix);
		for (k = 1; k <= 7; k++) {
			ix.delay_calls = 0; ix.delay_abort_after = k;
			CHECK(RUN(&s, 1 << 24, &r) == CUVS_REACH_ABORTED && ix.delay_calls == k);
		}
		ix.delay_calls = 0; ix.delay_abort_after = 8;
		CHECK(RUN(&s, 1 << 24, &r) == CUVS_REACH_OK && ix.delay_calls == 7);
		free_index(&ix);

		/* the pass over the pages the chain skipped asks too: 300 of them (ledger pages, say) are two calls, after the chain's
		 * one and before the search's one and the emit pass's one */
		memset(&ix, 0, sizeof(ix));
		memset(&g, 0, sizeof(g));
		g.n = 3; g.entry = 0;
		build_index(&ix, &g, &lay, 1, 0);
		ix.pages = realloc(ix.pages, (size_t) (ix.npages + 300) * CUVS_REACH_PAGE_SIZE);
		memset(ix.pages[ix.npages], 0, (size_t) 300 * CUVS_REACH_PAGE_SIZE);
		ix.npages += 300; ix.visible = ix.npages;
		s = source_of(&ix);
		for (k = 1; k <= 5; k++) {
			ix.delay_calls = 0; ix.delay_abort_after = k;
			CHECK(RUN(&s, 1 << 24, &r) == CUVS_REACH_ABORTED && ix.delay_calls == k);
		}
		ix.delay_calls = 0; ix.delay_abort_after = 6;
		CHECK(RUN(&s, 1 << 24, &r) == CUVS_REACH_OK && ix.delay_calls == 5);
		free_index(&ix);
	}

	/* ---- a page that grows between the chain pass and the emit pass: line pointers beyond the width recorded in pass A
	 * belong to no bitmap slot of this page and must be ignored (they would be read as another block's bits) ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
		memset(&ix, 0, sizeof(ix));
		memset(&g, 0, sizeof(g));
		g.n = 2; g.entry = 0; g.extra[1] = 2;	/* elements 0 and 1 in blocks 1 and 2; 1 is unreachable */
		build_index(&ix, &g, &lay, 1, 0);
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.unreached == 1 && r.emitted == 3);
		ix.damage_fn = grow_block_1;
		ix.damage_at_read = 1 + 2 + 1 + 1;	/* meta + chain + search (entry block) + the first pass C page, block 1 */
		ix.reads = 0;
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.unreached == 1 && r.emitted == 3);	/* the invented line pointer is ignored */
		ix.damage_fn = NULL; ix.damage_at_read = 0;
		free_index(&ix);
	}

	/* ---- an element whose neighbor tuple lies on ANOTHER page (the build puts them together; inserts need not) ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
		uint32_t lp, at;
		memset(&ix, 0, sizeof(ix));
		memset(&g, 0, sizeof(g));
		g.n = 4; g.entry = 0;	/* 0 -> 1; node 3 (block 2) lists node 2 (block 2) */
		g.deg[0] = 1; g.adj[0][0] = 1; g.deg[3] = 1; g.adj[3][0] = 2;
		build_index(&ix, &g, &lay, 2, 0);
		memcpy(&lp, ix.pages[lay.blk[1]] + CUVS_REACH_PAGE_HEADER + 4 * (lay.eoff[1] - 1), 4);
		at = lp & 0x7FFF;
		put_tid(ix.pages[lay.blk[1]], at + 64, lay.blk[3], (uint16_t) lay.noff[3]);	/* element 1 borrows node 3's list on block 2 */
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 3);	/* 0, 1, and 2 through the other page */
		memcpy(&lp, ix.pages[lay.blk[3]] + CUVS_REACH_PAGE_HEADER + 4 * (lay.noff[3] - 1), 4);
		/* the tuple turns into something else after the chain pass (reads: meta 1, chain 2, then block 1 and block 2 of the search) */
		ix.damage_at_read = 6; ix.reads = 0; ix.damage_page = lay.blk[3]; ix.damage_off = lp & 0x7FFF; ix.damage_val = 9;
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_OK && r.reached == 2);	/* not followed, and not damage: the safe direction */
		ix.damage_at_read = 0;
		ix.pages[lay.blk[3]][lp & 0x7FFF] = 9;	/* that tuple is no neighbor tuple: a graph page has nothing else */
		CHECK(RUN(&s, 1 << 20, &r) == CUVS_REACH_DAMAGED);
		free_index(&ix);
	}

	/* ---- spill: the large arrays go to spill memory when RAM would be exceeded, with the same answer; only when spill is
	 * refused or used up is there no result, and then the result says what it needed ---- */
	{
		Graph g; Layout lay; Index ix; CuvsReachResult unlimited, r; CuvsReachSource s;
		uint64_t ram_floor;
		memset(&ix, 0, sizeof(ix));
		random_graph(&g, 300, 8, 10);
		build_index(&ix, &g, &lay, 3, 1);
		s = source_of(&ix);
		CHECK(RUN(&s, 1 << 26, &unlimited) == CUVS_REACH_OK);
		/* RAM for the page only: the block table and every bitmap spill */
		ram_floor = CUVS_REACH_PAGE_SIZE;
		CHECK(RUN_SPILL(&s, ram_floor, 1 << 26, &r) == CUVS_REACH_OK);
		CHECK(r.spilled_bytes > 0 && sp_spill_calls > 0);
		CHECK(r.live == unlimited.live && r.reached == unlimited.reached && r.unreached == unlimited.unreached && r.elements == unlimited.elements);
		CHECK(r.mem_bytes <= ram_floor);
		/* RAM enough for everything: nothing spills */
		CHECK(RUN_SPILL(&s, 1 << 26, 1 << 26, &r) == CUVS_REACH_OK && r.spilled_bytes == 0 && sp_spill_calls == 0);
		/* spill too small: no result, and the estimate is within a factor of two of the real need */
		CHECK(RUN_SPILL(&s, ram_floor, 100, &r) == CUVS_REACH_OVER_LIMIT);
		CHECK(r.reached == 0 && r.unreached == 0 && r.needed_bytes > 0);
		{
			uint64_t need = unlimited.mem_bytes;
			CHECK(r.needed_bytes * 2 >= need && r.needed_bytes <= need * 2);
		}
		/* spill disabled (limit 0) behaves like the old limit */
		CHECK(RUN_SPILL(&s, ram_floor, 0, &r) == CUVS_REACH_OVER_LIMIT);
		/* the same answer for every split of the budget, for several graphs (the number of search rounds differs, and the
		 * two frontier arrays change places once per round) */
		{
			int variant;
			free_index(&ix);
			for (variant = 0; variant < 5; variant++) {
				CuvsReachResult base;
				uint64_t ram;
				memset(&ix, 0, sizeof(ix));
				random_graph(&g, 40 + variant * 30, 2 + variant, 10);
				build_index(&ix, &g, &lay, 3, variant & 1);
				s = source_of(&ix);
				CHECK(RUN(&s, 1 << 26, &base) == CUVS_REACH_OK);
				for (ram = CUVS_REACH_PAGE_SIZE; ram < base.mem_bytes + 256; ram += 13) {
					CuvsReachResult x;
					CHECK(RUN_SPILL(&s, ram, 1 << 26, &x) == CUVS_REACH_OK && x.reached == base.reached && x.unreached == base.unreached);
				}
				free_index(&ix);
			}
			memset(&ix, 0, sizeof(ix));
			random_graph(&g, 300, 8, 10);
			build_index(&ix, &g, &lay, 3, 1);
			s = source_of(&ix);
		}
		/* a relation whose block table alone does not fit is refused before any page is read (no wasted full read) */
		free_index(&ix);
		memset(&ix, 0, sizeof(ix));
		memset(&g, 0, sizeof(g));
		g.n = 600; g.entry = 0;
		build_index(&ix, &g, &lay, 1, 0);
		s = source_of(&ix);
		CHECK(RUN(&s, 6u * ix.npages + CUVS_REACH_PAGE_SIZE - 1, &r) == CUVS_REACH_OVER_LIMIT && ix.total_reads == 0);
		/* refused before a page was read: the projection from the size of the relation alone (block table and widths, the page
		 * scratch, four bitmaps of the typical 40 line pointers per page) */
		CHECK(r.needed_bytes == (uint64_t) ix.npages * 6u + CUVS_REACH_PAGE_SIZE + 4u * (((uint64_t) ix.npages * 40u + 7u) / 8u));
		CHECK(RUN(&s, CUVS_REACH_PAGE_SIZE - 1, &r) == CUVS_REACH_OVER_LIMIT && ix.total_reads == 0);	/* not even the page scratch */
		CHECK(r.needed_bytes == (uint64_t) ix.npages * 6u + CUVS_REACH_PAGE_SIZE + 4u * (((uint64_t) ix.npages * 40u + 7u) / 8u));
		free_index(&ix);
	}

	/* ---- the estimate of what an over-limit walk needs is a projection from the part of the chain already read: at 100000
	 * blocks of 100 slots the real need is the fixed part (0.6 MB) plus four bitmaps of 1.25 MB, and a request-sized
	 * figure (the one array that was refused) would be a third of the real peak ---- */
	{
		Synth sy = {100000};
		CuvsReachSource src = {&sy, synth_nblocks, synth_read, NULL, NULL, NULL, NULL};
		CuvsReachResult r, big;
		CHECK(cuvs_reach_run(&src, 1ull << 30, 0, &big) == CUVS_REACH_OK && big.live == 0);
		CHECK(cuvs_reach_run(&src, 2u << 20, 0, &r) == CUVS_REACH_OVER_LIMIT);
		CHECK(r.needed_bytes * 2 >= big.mem_bytes && r.needed_bytes <= big.mem_bytes * 2);	/* within a factor of two */
		CHECK(t_cur == 0);
	}

	/* ---- fuzz: valid graphs with random byte damage, and pure noise behind a valid metapage; the point is that a
	 * sanitizer build stays silent and the counters stay consistent ---- */
	{
		int trial;
		for (trial = 0; trial < 3000; trial++) {
			Graph g; Layout lay; Index ix; CuvsReachResult r; CuvsReachSource s;
			uint32_t flips = 1 + (uint32_t) (rnd() % 12), f;
			memset(&ix, 0, sizeof(ix));
			random_graph(&g, 5 + (int) (rnd() % 60), 1 + (int) (rnd() % MAXDEG), (int) (rnd() % 30));
			build_index(&ix, &g, &lay, 1 + (int) (rnd() % 8), (int) (rnd() & 1));
			for (f = 0; f < flips; f++) {
				uint32_t pg = (uint32_t) (rnd() % ix.npages), at = (uint32_t) (rnd() % CUVS_REACH_PAGE_SIZE);
				ix.pages[pg][at] = (uint8_t) rnd();
			}
			if (trial % 7 == 0) {	/* noise after the first page */
				uint32_t pg, at;
				for (pg = 2; pg < ix.npages; pg++)
					for (at = 0; at < CUVS_REACH_PAGE_SIZE; at++) ix.pages[pg][at] = (uint8_t) rnd();
			}
			s = source_of(&ix);
			if (RUN(&s, 1 << 22, &r) == CUVS_REACH_OK) {
				CHECK(r.reached <= r.live && r.live <= r.elements && r.unreached == r.live - r.reached);
				CHECK(r.emitted <= r.unreached * CUVS_REACH_HEAPTIDS);
			}
			free_index(&ix);
		}
	}

	if (failures) {
		fprintf(stderr, "test_cover_reach: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cover_reach: all checks passed\n");
	return 0;
}
