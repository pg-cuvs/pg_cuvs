#ifndef CUVS_COVER_REACH_H
#define CUVS_COVER_REACH_H
/* structural reachability of a LIVE pgvector HNSW index.
 *
 * Question answered: which live elements can the search NOT reach from the entry point by following
 * neighbor tuples of every level? Those are the rows a recertification must put into the ledger base
 * (the unreachable set U). Pure C with no PostgreSQL headers: pages are 8192-byte images obtained
 * through a callback (the product wrapper copies a shared buffer under a short share lock, and
 * releases it before the next read, so nothing here ever holds a lock).
 *
 * The index is being modified while this runs, so every number read from a page is treated as
 * hostile: every block number is checked against the relation size read ONCE at the start (blocks
 * appended later hold rows newer than the ledger's T_start, which the ledger covers), every item is
 * checked for position and length, loops in the page chain end the walk, and the function never
 * reads outside the 8192-byte image it was given.
 *
 * Memory is bounded by the caller's limit and accounted so that the peak covers what is really held, including
 * the moment a growing bitmap exists twice. What it holds: 6 bytes per block (bit offset and slot count), and four
 * bitmaps (live, seen, current and next frontier) of one bit per line-pointer slot of the chain's pages. The search
 * runs in rounds over frontier bitmaps instead of a queue: a queue grows to about the number of rows (measured:
 * 12 bytes per row), the bitmaps need one bit per slot and round. A round visits the blocks in block order, so the
 * page reads are ordered too. The old research version used 512 bytes per block for the bitmaps and needed 4.3 GB
 * for 100M rows.
 *
 * The large arrays (block table, bitmaps) can be spilled: when the RAM limit would be exceeded and the source
 * offers big_alloc, they are requested with spill = 1 (the PostgreSQL side maps an unlinked temp file) up to
 * spill_limit bytes. Only when that is refused or exhausted does the walk stop with CUVS_REACH_OVER_LIMIT, no
 * result, and needed_bytes says roughly how much it would have needed.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The allocator can be replaced before this header is included. Inside PostgreSQL it must be palloc based:
 * the delay hook can raise an error (query cancel) and the memory has to go away with the memory context. */
#ifndef CUVS_REACH_MALLOC
#define CUVS_REACH_MALLOC(n) malloc(n)
#define CUVS_REACH_CALLOC(n, sz) calloc(n, sz)
#define CUVS_REACH_FREE(p) free(p)
#endif

#define CUVS_REACH_PAGE_SIZE 8192u
#define CUVS_REACH_PAGE_HEADER 24u
#define CUVS_REACH_SPECIAL_SIZE 8u
#define CUVS_REACH_PGV_MAGIC 0xA953A953u
#define CUVS_REACH_META_ENTRY_BLK 40u		/* HnswMetaPageData.entryBlkno (page offset) */
#define CUVS_REACH_META_ENTRY_OFF 44u		/* HnswMetaPageData.entryOffno */
#define CUVS_REACH_HEAD_BLKNO 1u		/* HNSW_HEAD_BLKNO: first element page */
#define CUVS_REACH_ELEMENT_TYPE 1u
#define CUVS_REACH_NEIGHBOR_TYPE 2u
#define CUVS_REACH_HEAPTIDS 10u
#define CUVS_REACH_ELEMENT_FIXED 72u		/* offsetof(HnswElementTupleData, data) */
#define CUVS_REACH_NEIGHBOR_FIXED 4u		/* offsetof(HnswNeighborTupleData, indextids) */
#define CUVS_REACH_TID_SIZE 6u
#define CUVS_REACH_NO_BLOCK 0xFFFFFFFFu
#define CUVS_REACH_PGV_PAGE_ID 0xFF90u		/* HNSW_PAGE_ID: last two bytes of the special area of every graph page */
#define CUVS_REACH_PAGE_ID_AT (CUVS_REACH_PAGE_SIZE - 2u)
#ifndef CUVS_REACH_BITS_INITIAL
#define CUVS_REACH_BITS_INITIAL 4096u		/* bitmap capacity in bits before the first growth; the unit test shrinks it */
#endif
#define CUVS_REACH_DELAY_EVERY 256u		/* pages or nodes between two calls of the delay hook */
#ifndef CUVS_REACH_TYPICAL_SLOTS
#define CUVS_REACH_TYPICAL_SLOTS 40u		/* line pointers on a graph page when none has been read yet: 32 dimensions have about 40, 768 about 4, small ones over 100 */
#endif

typedef enum {
	CUVS_REACH_OK = 0,
	CUVS_REACH_NOT_HNSW,		/* block 0 or block 1 is not a pgvector hnsw page */
	CUVS_REACH_OVER_LIMIT,		/* memory limit reached: no result */
	CUVS_REACH_NO_MEMORY,		/* allocation failed: no result */
	CUVS_REACH_DAMAGED,		/* the page chain cannot be followed to its end (bad header, unreadable page, loop,
					 * link outside the relation): rows behind the break were not seen, so no result.
					 * A caller must never certify on this. */
	CUVS_REACH_ABORTED		/* the delay hook or the emit callback asked to stop */
} CuvsReachStatus;

typedef struct CuvsReachSource {
	void *ctx;
	uint32_t (*nblocks)(void *ctx);
	/* copy block `blk` into dst (CUVS_REACH_PAGE_SIZE bytes); 0 on success, nonzero if it cannot be read */
	int (*read_block)(void *ctx, uint32_t blk, uint8_t *dst);
	/* interrupts and vacuum cost delay; returning nonzero aborts. May be NULL. */
	int (*delay)(void *ctx);
	/* called for each heap TID of each unreached live element; nonzero aborts. May be NULL. */
	int (*emit)(void *ctx, uint32_t heap_blk, uint16_t heap_off);
	/* Large zeroed arrays. spill == 0: RAM; spill != 0: memory that need not be RAM (a mapped temp file). Return NULL
	 * when it cannot be had. Both may be NULL: libc RAM through the CUVS_REACH_* macros, no spill. */
	void *(*big_alloc)(void *ctx, size_t bytes, int spill);
	void (*big_free)(void *ctx, void *p, size_t bytes, int spill);
} CuvsReachSource;

typedef struct CuvsReachResult {
	CuvsReachStatus status;
	uint32_t nblocks;		/* relation size read at the start; nothing beyond it is looked at */
	uint64_t elements;		/* element tuples seen in the chain (live or deleted) */
	uint64_t live;			/* element tuples with deleted == 0 */
	uint64_t reached;		/* live elements reachable from the entry point */
	uint64_t unreached;		/* live - reached */
	uint64_t emitted;		/* heap TIDs passed to emit */
	uint64_t mem_bytes;		/* peak accounted RAM */
	uint64_t spilled_bytes;		/* peak bytes placed in spill memory */
	uint64_t needed_bytes;		/* OVER_LIMIT: an estimate of the RAM + spill the walk needs; else 0 */
} CuvsReachResult;

/* ---- page image helpers: every access is range checked ---- */

static inline uint16_t
cuvs_reach_u16(const uint8_t *p, size_t off)
{
	uint16_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

static inline uint32_t
cuvs_reach_u32(const uint8_t *p, size_t off)
{
	uint32_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

/* Number of line pointers if the page header is plausible, else -1. */
static inline int
cuvs_reach_maxoff(const uint8_t *page)
{
	uint32_t lower = cuvs_reach_u16(page, 12), upper = cuvs_reach_u16(page, 14), special = cuvs_reach_u16(page, 16);

	if (special != CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE || lower < CUVS_REACH_PAGE_HEADER || lower > upper || upper > special)
		return -1;
	return (int) ((lower - CUVS_REACH_PAGE_HEADER) / 4u);
}

/* The item at offset `off` (1-based) if it is a normal item of at least `minlen` bytes that lies inside the
 * page, else NULL. *len receives its length. */
static inline const uint8_t *
cuvs_reach_item(const uint8_t *page, int maxoff, unsigned off, uint32_t minlen, uint32_t *len)
{
	uint32_t lp;
	uint32_t at, n, flags;

	if (off < 1 || (int) off > maxoff)
		return NULL;
	lp = cuvs_reach_u32(page, CUVS_REACH_PAGE_HEADER + 4u * (off - 1u));
	at = lp & 0x7FFFu;
	flags = (lp >> 15) & 3u;
	n = lp >> 17;
	if (flags != 1u || n < minlen || at + n > CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE)
		return NULL;
	if (len != NULL)
		*len = n;
	return page + at;
}

/* ---- the computation ---- */

typedef struct CuvsReachBuf {
	void *p;
	size_t bytes;
	int spilled;
} CuvsReachBuf;

typedef struct CuvsReachState {
	const CuvsReachSource *src;
	uint64_t limit, used, peak;		/* RAM */
	uint64_t spill_limit, spilled, spilled_peak;
	uint64_t want;				/* what a refused request (or a projection) says is needed */
	uint32_t nblocks, chain_seen;
	CuvsReachBuf b_base, b_width, b_live, b_seen, b_cur, b_nxt;
	uint32_t *base;			/* bit offset of block b in the bitmaps, or NO_BLOCK if not in the chain */
	uint16_t *width;		/* line pointers of block b (valid where base != NO_BLOCK) */
	uint8_t *live, *seen, *cur, *nxt;	/* cur and nxt (frontiers) exist only during the search */
	uint64_t bits, bits_cap;	/* bits used / capacity of each bitmap */
	uint8_t *page;			/* scratch image */
	CuvsReachStatus fail;
} CuvsReachState;

static inline int
cuvs_reach_account(CuvsReachState *s, uint64_t add)
{
	if (s->used + add > s->limit) {
		s->fail = CUVS_REACH_OVER_LIMIT;
		return 0;
	}
	s->used += add;
	if (s->used > s->peak)
		s->peak = s->used;
	return 1;
}

static inline void
cuvs_reach_release(CuvsReachState *s, uint64_t bytes)
{
	s->used -= bytes;
}

/* A zeroed array of `bytes`: RAM while the limit allows, spill memory when it does not, else OVER_LIMIT. */
static inline int
cuvs_reach_buf_alloc(CuvsReachState *s, CuvsReachBuf *b, size_t bytes)
{
	uint64_t ram_free = s->limit > s->used ? s->limit - s->used : 0;
	void *p;

	if (bytes == 0)
		bytes = 1;
	if (bytes <= ram_free) {
		p = s->src->big_alloc != NULL ? s->src->big_alloc(s->src->ctx, bytes, 0) : CUVS_REACH_CALLOC(bytes, 1);
		if (p == NULL) {
			s->fail = CUVS_REACH_NO_MEMORY;
			return 0;
		}
		s->used += bytes;
		if (s->used > s->peak)
			s->peak = s->used;
		b->spilled = 0;
	} else if (s->src->big_alloc != NULL && s->spilled + bytes <= s->spill_limit &&
		   (p = s->src->big_alloc(s->src->ctx, bytes, 1)) != NULL) {
		s->spilled += bytes;
		if (s->spilled > s->spilled_peak)
			s->spilled_peak = s->spilled;
		b->spilled = 1;
	} else {
		if (s->used + s->spilled + bytes > s->want)
			s->want = s->used + s->spilled + bytes;
		s->fail = CUVS_REACH_OVER_LIMIT;
		return 0;
	}
	b->p = p;
	b->bytes = bytes;
	return 1;
}

static inline void
cuvs_reach_buf_free(CuvsReachState *s, CuvsReachBuf *b)
{
	if (b->p == NULL)
		return;
	if (b->spilled)
		s->spilled -= b->bytes;
	else
		s->used -= b->bytes;
	if (s->src->big_free != NULL)
		s->src->big_free(s->src->ctx, b->p, b->bytes, b->spilled);
	else
		CUVS_REACH_FREE(b->p);
	b->p = NULL;
	b->bytes = 0;
}

static inline int
cuvs_reach_bit(const uint8_t *bm, uint64_t i)
{
	return (bm[i >> 3] >> (i & 7u)) & 1;
}

static inline void
cuvs_reach_setbit(uint8_t *bm, uint64_t i)
{
	bm[i >> 3] |= (uint8_t) (1u << (i & 7u));
}

/* Bit index of (blk, off) if that slot exists in the chain's pages, else UINT64_MAX. */
static inline uint64_t
cuvs_reach_slot(const CuvsReachState *s, uint32_t blk, unsigned off)
{
	if (blk >= s->nblocks || s->base[blk] == CUVS_REACH_NO_BLOCK || off < 1 || off > s->width[blk])
		return UINT64_MAX;
	return (uint64_t) s->base[blk] + off - 1u;
}

/* Grow both bitmaps (zero filled) to hold `need` bits. */
/* Grow one bitmap (zero filled) to bytes_new. The new array exists while the old one is copied, so both are allocated
 * (and accounted) at that moment. */
static inline int
cuvs_reach_grow_one(CuvsReachState *s, CuvsReachBuf *b, uint8_t **ptr, size_t bytes_new)
{
	CuvsReachBuf nb;

	memset(&nb, 0, sizeof(nb));
	if (!cuvs_reach_buf_alloc(s, &nb, bytes_new))
		return 0;
	if (b->p != NULL)
		memcpy(nb.p, b->p, b->bytes < bytes_new ? b->bytes : bytes_new);
	cuvs_reach_buf_free(s, b);
	*b = nb;
	*ptr = (uint8_t *) nb.p;
	return 1;
}

static inline int
cuvs_reach_grow_bitmaps(CuvsReachState *s, uint64_t need)
{
	uint64_t cap, bytes_new;

	if (need <= s->bits_cap)
		return 1;
	cap = s->bits_cap ? s->bits_cap : CUVS_REACH_BITS_INITIAL;
	while (cap < need)
		cap *= 2;
	if (cap > 0xFFFFFFFEull)
		cap = 0xFFFFFFFEull;
	if (need > cap) {
		s->fail = CUVS_REACH_OVER_LIMIT;
		return 0;
	}
	bytes_new = (cap + 7u) / 8u;
	if (!cuvs_reach_grow_one(s, &s->b_live, &s->live, (size_t) bytes_new) ||
	    !cuvs_reach_grow_one(s, &s->b_seen, &s->seen, (size_t) bytes_new)) {
		if (s->fail == CUVS_REACH_OVER_LIMIT && s->chain_seen > 0) {
			/* project the whole need from the part of the chain read so far: the fixed part and four bitmaps of the
			 * final size */
			uint64_t est_bits = (uint64_t) ((double) need * (double) s->nblocks / (double) s->chain_seen);
			uint64_t projected = (uint64_t) s->nblocks * 6u + CUVS_REACH_PAGE_SIZE + 4u * ((est_bits + 7u) / 8u);

			if (projected > s->want)
				s->want = projected;
		}
		return 0;
	}
	s->bits_cap = cap;
	return 1;
}

/* Refused before a single chain page was read: all that is known is the size of the relation. Without this the figure would be
 * the one array that was refused (measured: six times too low). The page scratch, block table and widths are exact; the
 * four bitmaps assume CUVS_REACH_TYPICAL_SLOTS line pointers per page. */
static inline void
cuvs_reach_project_first(CuvsReachState *s)
{
	uint64_t projected;

	if (s->fail != CUVS_REACH_OVER_LIMIT)
		return;
	projected = (uint64_t) s->nblocks * (sizeof(uint32_t) + sizeof(uint16_t)) + CUVS_REACH_PAGE_SIZE +
		4u * (((uint64_t) s->nblocks * CUVS_REACH_TYPICAL_SLOTS + 7u) / 8u);
	if (projected > s->want)
		s->want = projected;
}

static inline void
cuvs_reach_free(CuvsReachState *s)
{
	cuvs_reach_buf_free(s, &s->b_base);
	cuvs_reach_buf_free(s, &s->b_width);
	cuvs_reach_buf_free(s, &s->b_live);
	cuvs_reach_buf_free(s, &s->b_seen);
	cuvs_reach_buf_free(s, &s->b_cur);
	cuvs_reach_buf_free(s, &s->b_nxt);
	CUVS_REACH_FREE(s->page);
}

static inline CuvsReachStatus
cuvs_reach_run(const CuvsReachSource *src, uint64_t mem_limit, uint64_t spill_limit, CuvsReachResult *out)
{
	CuvsReachState s;
	uint32_t blk, hops = 0, entry_blk, nchain = 0;
	unsigned entry_off;
	uint64_t entry_slot;
	uint64_t pages_done = 0, nodes_done = 0;

	memset(out, 0, sizeof(*out));
	memset(&s, 0, sizeof(s));
	s.src = src;
	s.limit = mem_limit;
	s.spill_limit = spill_limit;
	s.fail = CUVS_REACH_OK;
	s.nblocks = src->nblocks(src->ctx);
	out->nblocks = s.nblocks;
	if (s.nblocks < 2u) {
		out->status = CUVS_REACH_NOT_HNSW;
		return out->status;
	}
	/* account first, then allocate: a limit that is too small must not be exceeded even for the fixed part. The
	 * block table is the first thing a walk needs, so a relation too big for the limits (and no spill) fails here
	 * without reading a page. */
	if (!cuvs_reach_account(&s, CUVS_REACH_PAGE_SIZE)) {
		cuvs_reach_project_first(&s);
		goto done;
	}
	s.page = CUVS_REACH_MALLOC(CUVS_REACH_PAGE_SIZE);
	if (s.page == NULL) {
		s.fail = CUVS_REACH_NO_MEMORY;
		goto done;
	}
	if (!cuvs_reach_buf_alloc(&s, &s.b_base, (size_t) s.nblocks * sizeof(uint32_t)) ||
	    !cuvs_reach_buf_alloc(&s, &s.b_width, (size_t) s.nblocks * sizeof(uint16_t))) {
		cuvs_reach_project_first(&s);
		goto done;
	}
	s.base = (uint32_t *) s.b_base.p;
	s.width = (uint16_t *) s.b_width.p;
	memset(s.base, 0xFF, (size_t) s.nblocks * sizeof(uint32_t));

	/* metapage: the entry point */
	if (src->read_block(src->ctx, 0, s.page) != 0 || cuvs_reach_u32(s.page, 24) != CUVS_REACH_PGV_MAGIC) {
		s.fail = CUVS_REACH_NOT_HNSW;
		goto done;
	}
	entry_blk = cuvs_reach_u32(s.page, CUVS_REACH_META_ENTRY_BLK);
	entry_off = cuvs_reach_u16(s.page, CUVS_REACH_META_ENTRY_OFF);

	/* pass A: walk the page chain from block 1; give every block a bit range, mark live elements */
	for (blk = CUVS_REACH_HEAD_BLKNO; blk != CUVS_REACH_NO_BLOCK; ) {
		int maxoff;
		unsigned off;
		uint32_t next;

		if (blk == 0 || blk >= s.nblocks) {
			/* a page appended after the size was read ends the walk (its rows are newer than the ledger's start);
			 * a link to the metapage or to a block that does not exist even now is damage */
			if (blk != 0 && blk < src->nblocks(src->ctx))
				break;
			s.fail = CUVS_REACH_DAMAGED;
			goto done;
		}
		if (s.base[blk] != CUVS_REACH_NO_BLOCK || hops++ > s.nblocks) {	/* loop in the chain */
			s.fail = CUVS_REACH_DAMAGED;
			goto done;
		}
		if (src->delay != NULL && (pages_done++ % CUVS_REACH_DELAY_EVERY) == 0 && src->delay(src->ctx) != 0) {
			s.fail = CUVS_REACH_ABORTED;
			goto done;
		}
		if (src->read_block(src->ctx, blk, s.page) != 0) {
			s.fail = blk == CUVS_REACH_HEAD_BLKNO ? CUVS_REACH_NOT_HNSW : CUVS_REACH_DAMAGED;
			goto done;
		}
		maxoff = cuvs_reach_maxoff(s.page);
		if (maxoff < 0) {
			s.fail = blk == CUVS_REACH_HEAD_BLKNO ? CUVS_REACH_NOT_HNSW : CUVS_REACH_DAMAGED;
			goto done;
		}
		if (s.bits + (uint64_t) maxoff > 0xFFFFFFFEull) {
			s.fail = CUVS_REACH_OVER_LIMIT;
			goto done;
		}
		if (!cuvs_reach_grow_bitmaps(&s, s.bits + (uint64_t) maxoff))
			goto done;
		s.base[blk] = (uint32_t) s.bits;
		s.width[blk] = (uint16_t) maxoff;
		for (off = 1; off <= (unsigned) maxoff; off++) {
			uint32_t len;
			const uint8_t *e = cuvs_reach_item(s.page, maxoff, off, 1u, &len);

			/* A graph page holds element tuples and neighbor tuples and nothing else, every line pointer in use. An item
			 * that is not one of them cannot be skipped: if it was an element, a live row would be missing from the
			 * count of what the graph cannot reach, and that count is what the guarantee is made of. */
			if (e == NULL || (e[0] == CUVS_REACH_ELEMENT_TYPE ? len < CUVS_REACH_ELEMENT_FIXED :
					  e[0] == CUVS_REACH_NEIGHBOR_TYPE ? len < CUVS_REACH_NEIGHBOR_FIXED : 1)) {
				s.fail = CUVS_REACH_DAMAGED;
				goto done;
			}
			if (e[0] == CUVS_REACH_ELEMENT_TYPE) {
				out->elements++;
				if (e[2] == 0) {
					cuvs_reach_setbit(s.live, s.bits + off - 1u);
					out->live++;
				}
			}
		}
		s.bits += (uint64_t) maxoff;
		nchain++;
		s.chain_seen = nchain;
		next = cuvs_reach_u32(s.page, CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE);
		blk = next;
	}
	if (nchain == 0) {
		s.fail = CUVS_REACH_NOT_HNSW;
		goto done;
	}
	/* Every page of the relation that the chain did not reach must be something that is not graph: a ledger page, an extension
	 * that was never initialised (zeros). A graph page the chain skipped means the chain was cut, and the rows behind the cut
	 * were not seen. The size is the one read at the start, so a page an insert appended meanwhile is not looked at. */
	{
		uint32_t b;
		uint64_t skipped_done = 0;

		for (b = CUVS_REACH_HEAD_BLKNO + 1u; b < s.nblocks; b++) {
			if (s.base[b] != CUVS_REACH_NO_BLOCK)
				continue;
			if (src->delay != NULL && (skipped_done++ % CUVS_REACH_DELAY_EVERY) == 0 && src->delay(src->ctx) != 0) {
				s.fail = CUVS_REACH_ABORTED;
				goto done;
			}
			if (src->read_block(src->ctx, b, s.page) != 0) {
				s.fail = CUVS_REACH_DAMAGED;
				goto done;
			}
			if (cuvs_reach_u16(s.page, CUVS_REACH_PAGE_ID_AT) == CUVS_REACH_PGV_PAGE_ID && cuvs_reach_maxoff(s.page) != 0) {
				s.fail = CUVS_REACH_DAMAGED;
				goto done;
			}
		}
	}

	/* pass B: breadth first search from the entry point over the neighbor tuples of every level, one round per
	 * BFS level. cur holds the elements to expand in this round, nxt the ones found in it. */
	entry_slot = cuvs_reach_slot(&s, entry_blk, entry_off);
	{
		uint64_t frontier_bytes = (s.bits + 7u) / 8u;
		uint64_t cur_count = 0;

		if (!cuvs_reach_buf_alloc(&s, &s.b_cur, (size_t) frontier_bytes) ||
		    !cuvs_reach_buf_alloc(&s, &s.b_nxt, (size_t) frontier_bytes))
			goto done;
		s.cur = (uint8_t *) s.b_cur.p;
		s.nxt = (uint8_t *) s.b_nxt.p;
		if (entry_slot != UINT64_MAX && cuvs_reach_bit(s.live, entry_slot)) {
			cuvs_reach_setbit(s.seen, entry_slot);
			cuvs_reach_setbit(s.cur, entry_slot);
			out->reached = 1;
			cur_count = 1;
		}
		while (cur_count > 0) {
			uint64_t next_count = 0;
			uint32_t b;

			for (b = CUVS_REACH_HEAD_BLKNO; b < s.nblocks; b++) {
				unsigned no;
				int have_page = 0, maxoff = -1;

				if (s.base[b] == CUVS_REACH_NO_BLOCK)
					continue;
				for (no = 1; no <= s.width[b]; no++) {
					uint64_t slot = (uint64_t) s.base[b] + no - 1u;
					uint32_t eb, len, ntid, k;
					unsigned eo;
					int nmaxoff;
					const uint8_t *item;
					uint8_t eparts[CUVS_REACH_NEIGHBOR_FIXED + 6u * 2048u];

					if (!cuvs_reach_bit(s.cur, slot))
						continue;
					if (!have_page) {
						if (src->delay != NULL && (nodes_done++ % CUVS_REACH_DELAY_EVERY) == 0 && src->delay(src->ctx) != 0) {
							s.fail = CUVS_REACH_ABORTED;
							goto done;
						}
						if (src->read_block(src->ctx, b, s.page) != 0 || (maxoff = cuvs_reach_maxoff(s.page)) < 0)
							break;
						have_page = 1;
					}
					item = cuvs_reach_item(s.page, maxoff, no, CUVS_REACH_ELEMENT_FIXED, &len);
					if (item == NULL || item[0] != CUVS_REACH_ELEMENT_TYPE)
						continue;
					/* neighbortid sits right after the ten heap TIDs: offset 4 + 60 */
					eb = ((uint32_t) cuvs_reach_u16(item, 64) << 16) | cuvs_reach_u16(item, 66);
					eo = cuvs_reach_u16(item, 68);
					if (eb >= s.nblocks)
						continue;
					if (eb == b) {
						/* the neighbor tuple usually shares the page: read it from the image we already have */
						item = cuvs_reach_item(s.page, maxoff, eo, CUVS_REACH_NEIGHBOR_FIXED, &len);
						if (item == NULL || item[0] != CUVS_REACH_NEIGHBOR_TYPE)
							continue;
						ntid = cuvs_reach_u16(item, 2);
						if (ntid > (len - CUVS_REACH_NEIGHBOR_FIXED) / CUVS_REACH_TID_SIZE)
							ntid = (len - CUVS_REACH_NEIGHBOR_FIXED) / CUVS_REACH_TID_SIZE;
						memcpy(eparts, item, CUVS_REACH_NEIGHBOR_FIXED + (size_t) ntid * CUVS_REACH_TID_SIZE);
					} else {
						/* a second image: the element page buffer stays valid for the rest of this block */
						uint8_t other[CUVS_REACH_PAGE_SIZE];

						if (src->read_block(src->ctx, eb, other) != 0 || (nmaxoff = cuvs_reach_maxoff(other)) < 0)
							continue;
						item = cuvs_reach_item(other, nmaxoff, eo, CUVS_REACH_NEIGHBOR_FIXED, &len);
						if (item == NULL || item[0] != CUVS_REACH_NEIGHBOR_TYPE)
							continue;
						ntid = cuvs_reach_u16(item, 2);
						if (ntid > (len - CUVS_REACH_NEIGHBOR_FIXED) / CUVS_REACH_TID_SIZE)
							ntid = (len - CUVS_REACH_NEIGHBOR_FIXED) / CUVS_REACH_TID_SIZE;
						memcpy(eparts, item, CUVS_REACH_NEIGHBOR_FIXED + (size_t) ntid * CUVS_REACH_TID_SIZE);
					}
					for (k = 0; k < ntid; k++) {
						size_t at = CUVS_REACH_NEIGHBOR_FIXED + (size_t) k * CUVS_REACH_TID_SIZE;
						uint32_t tb = ((uint32_t) cuvs_reach_u16(eparts, at) << 16) | cuvs_reach_u16(eparts, at + 2);
						unsigned to = cuvs_reach_u16(eparts, at + 4);
						uint64_t ts = cuvs_reach_slot(&s, tb, to);

						if (ts == UINT64_MAX || !cuvs_reach_bit(s.live, ts) || cuvs_reach_bit(s.seen, ts))
							continue;
						cuvs_reach_setbit(s.seen, ts);
						cuvs_reach_setbit(s.nxt, ts);
						out->reached++;
						next_count++;
					}
				}
			}
			/* the next round expands what this one found; the old frontier is cleared for reuse */
			{
				uint8_t *t = s.cur;

				/* only the working pointers change places; b_cur and b_nxt keep describing the two arrays for freeing */
				s.cur = s.nxt;
				s.nxt = t;
				memset(s.nxt, 0, (size_t) frontier_bytes);
			}
			cur_count = next_count;
		}
	}
	out->unreached = out->live - out->reached;

	/* pass C: hand out the heap TIDs of the unreached live elements, in chain order */
	if (src->emit != NULL && out->unreached > 0) {
		uint32_t visited = 0;

		hops = 0;
		pages_done = 0;
		for (blk = CUVS_REACH_HEAD_BLKNO; blk != CUVS_REACH_NO_BLOCK && blk < s.nblocks; ) {
			int maxoff;
			unsigned off;

			/* the chain must be the one pass A saw, every page readable: a missing page means missing keys */
			if (hops++ > s.nblocks || s.base[blk] == CUVS_REACH_NO_BLOCK) {
				s.fail = CUVS_REACH_DAMAGED;
				goto done;
			}
			if (src->delay != NULL && (pages_done++ % CUVS_REACH_DELAY_EVERY) == 0 && src->delay(src->ctx) != 0) {
				s.fail = CUVS_REACH_ABORTED;
				goto done;
			}
			if (src->read_block(src->ctx, blk, s.page) != 0 || (maxoff = cuvs_reach_maxoff(s.page)) < 0) {
				s.fail = CUVS_REACH_DAMAGED;
				goto done;
			}
			visited++;
			for (off = 1; off <= (unsigned) maxoff && off <= s.width[blk]; off++) {
				uint64_t slot = (uint64_t) s.base[blk] + off - 1u;
				uint32_t len, t;
				const uint8_t *e;

				if (!cuvs_reach_bit(s.live, slot) || cuvs_reach_bit(s.seen, slot))
					continue;
				e = cuvs_reach_item(s.page, maxoff, off, CUVS_REACH_ELEMENT_FIXED, &len);
				if (e == NULL || e[0] != CUVS_REACH_ELEMENT_TYPE) {
					/* pass A saw a live element here and tuples are only ever added: this is damage, and skipping it would
					 * leave an unreached row out of the keys */
					s.fail = CUVS_REACH_DAMAGED;
					goto done;
				}
				for (t = 0; t < CUVS_REACH_HEAPTIDS; t++) {
					size_t at = 4u + (size_t) t * CUVS_REACH_TID_SIZE;
					uint32_t hb = ((uint32_t) cuvs_reach_u16(e, at) << 16) | cuvs_reach_u16(e, at + 2);
					uint16_t ho = cuvs_reach_u16(e, at + 4);

					if (ho == 0)	/* invalid ItemPointer */
						continue;
					if (src->emit(src->ctx, hb, ho) != 0) {
						s.fail = CUVS_REACH_ABORTED;
						goto done;
					}
					out->emitted++;
				}
			}
			blk = cuvs_reach_u32(s.page, CUVS_REACH_PAGE_SIZE - CUVS_REACH_SPECIAL_SIZE);
		}
		if (visited != nchain) {
			s.fail = CUVS_REACH_DAMAGED;
			goto done;
		}
	}

done:
	out->mem_bytes = s.peak;
	out->spilled_bytes = s.spilled_peak;
	out->status = s.fail;
	if (s.fail == CUVS_REACH_OVER_LIMIT)
		out->needed_bytes = s.want;
	cuvs_reach_free(&s);
	if (out->status != CUVS_REACH_OK) {
		out->reached = out->unreached = out->emitted = 0;
	}
	return out->status;
}

#endif
