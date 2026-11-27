#ifndef CUVS_COVER_LEDGER_H
#define CUVS_COVER_LEDGER_H
/* cover ledger pages. Pure C: a page is an 8192-byte image.
 *
 * A ledger page lives OUTSIDE the graph chain: pgvector never reaches it. It is a standard page with an
 * 8-byte special area shaped like HnswPageOpaqueData but with page_id 0xC0E1, then a 40-byte header (LH)
 * (40 bytes, with the relation size seen at the last key) and an array of 24-byte keys:
 *
 *   [PageHeader 24][LH 32][key 24] * count   <pd_lower> ... <pd_upper == pd_special> [nextblkno u32][kind u16][page_id u16]
 *
 * The LH crc covers the LH fields before it (bytes 0..31). Every key carries a CRC-16 of its own (CCITT, polynomial 0x1021,
 * start 0xFFFF) over its 24 bytes with the crc field zero, so that a single flipped bit in a heap TID, a transaction id or the
 * flags is found when the key is read: the header crc cannot cover the keys, it changes with every append. The on-page key is
 *
 *   [ordinal : 6 bytes][crc16 : 2][creator_fullxid : 8][heap_block : 4][heap_offset : 2][flags : 2]
 *
 * (an ordinal needs 48 bits: 2.8e14 keys). The position check (ordinal == first_ordinal + index) stays.
 *
 * Kinds: 1 = keys appended by inserts since the certificate; 2 = base keys written by a certification.
 * Every page of one chain carries the generation of the descriptor that owns the chain, so a reader that
 * lands on a page recycled by a later recertification notices.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "cuvs_cover_format.h"

#define CUVS_LEDGER_MAGIC 0x4C45444Cu		/* "LEDL" */
#define CUVS_LEDGER_VERSION 1u
#define CUVS_LEDGER_PAGE_ID 0xC0E1u
#define CUVS_LEDGER_LH_SIZE 40u
#define CUVS_LEDGER_KEY_SIZE 24u
#define CUVS_LEDGER_KIND_KEYS 1u
#define CUVS_LEDGER_KIND_BASE 2u
#define CUVS_LEDGER_KEYS_AT (CUVS_COVER_PAGE_HEADER + CUVS_LEDGER_LH_SIZE)
#define CUVS_LEDGER_CAPACITY ((CUVS_COVER_PAGE_SIZE - CUVS_COVER_PAGE_HEADER - CUVS_LEDGER_LH_SIZE - CUVS_COVER_SPECIAL_SIZE) / CUVS_LEDGER_KEY_SIZE)
#define CUVS_LEDGER_KEY_UNKNOWN_CREATOR 0x0001u	/* key.flags: the creating transaction is not known */
#define CUVS_LEDGER_NO_BLOCK 0xFFFFFFFFu

typedef struct CuvsLedgerKey {
	uint64_t ordinal;
	uint64_t creator_fullxid;
	uint32_t heap_block;
	uint16_t heap_offset;
	uint16_t flags;
} CuvsLedgerKey;

#define CUVS_LEDGER_ORDINAL_MASK 0x0000FFFFFFFFFFFFull

static const uint16_t cuvs_ledger_crc16_table[256] = {
	0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
	0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
	0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
	0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
	0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
	0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
	0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
	0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
	0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
	0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
	0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
	0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
	0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
	0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
	0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
	0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
	0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
	0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
	0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
	0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
	0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
	0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
	0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
	0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
	0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
	0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
	0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
	0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
	0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
	0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
	0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
	0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
};

static inline uint16_t
cuvs_ledger_crc16(const uint8_t *p, size_t n)
{
	uint16_t c = 0xFFFFu;

	while (n--)
		c = (uint16_t) ((c << 8) ^ cuvs_ledger_crc16_table[(uint8_t) ((c >> 8) ^ *p++)]);
	return c;
}

/* The 24 bytes a key occupies on a page. */
static inline void
cuvs_ledger_key_encode(uint8_t *dst, const CuvsLedgerKey *k)
{
	uint64_t w0 = k->ordinal & CUVS_LEDGER_ORDINAL_MASK;
	uint16_t crc;

	memcpy(dst, &w0, 8);
	memcpy(dst + 8, &k->creator_fullxid, 8);
	memcpy(dst + 16, &k->heap_block, 4);
	memcpy(dst + 20, &k->heap_offset, 2);
	memcpy(dst + 22, &k->flags, 2);
	crc = cuvs_ledger_crc16(dst, CUVS_LEDGER_KEY_SIZE);
	w0 |= (uint64_t) crc << 48;
	memcpy(dst, &w0, 8);
}

/* 0 if the 24 bytes at `src` are a key with a matching crc, else nonzero (and *k is not set). */
static inline int
cuvs_ledger_key_decode(const uint8_t *src, CuvsLedgerKey *k)
{
	uint8_t b[CUVS_LEDGER_KEY_SIZE];
	uint64_t w0;
	uint16_t crc;

	memcpy(b, src, sizeof(b));
	memcpy(&w0, b, 8);
	crc = (uint16_t) (w0 >> 48);
	w0 &= CUVS_LEDGER_ORDINAL_MASK;
	memcpy(b, &w0, 8);
	if (cuvs_ledger_crc16(b, sizeof(b)) != crc)
		return 1;
	k->ordinal = w0;
	memcpy(&k->creator_fullxid, b + 8, 8);
	memcpy(&k->heap_block, b + 16, 4);
	memcpy(&k->heap_offset, b + 20, 2);
	memcpy(&k->flags, b + 22, 2);
	return 0;
}

typedef struct CuvsLedgerHeader {
	uint32_t magic;
	uint16_t version;
	uint16_t kind;
	uint64_t generation;
	uint64_t first_ordinal;
	uint32_t count;
	uint32_t graph_nblocks_seen;	/* relation size when the last key was written */
	uint32_t crc32c;
	uint32_t reserved;
} CuvsLedgerHeader;

_Static_assert(sizeof(CuvsLedgerKey) == CUVS_LEDGER_KEY_SIZE, "ledger key size");
_Static_assert(sizeof(CuvsLedgerHeader) == CUVS_LEDGER_LH_SIZE, "ledger header size");
_Static_assert(offsetof(CuvsLedgerHeader, generation) == 8, "LH generation offset");
_Static_assert(offsetof(CuvsLedgerHeader, first_ordinal) == 16, "LH first_ordinal offset");
_Static_assert(offsetof(CuvsLedgerHeader, count) == 24, "LH count offset");
_Static_assert(offsetof(CuvsLedgerHeader, graph_nblocks_seen) == 28, "LH seen offset");
_Static_assert(offsetof(CuvsLedgerHeader, crc32c) == 32, "LH crc offset");
_Static_assert(CUVS_LEDGER_CAPACITY == 338, "338 keys per page");

typedef enum {
	CUVS_LEDGER_OK = 0,
	CUVS_LEDGER_BAD_PAGE,		/* page header, special area or page_id is not a ledger page */
	CUVS_LEDGER_BAD_HEADER,		/* magic, version, kind, count or crc */
	CUVS_LEDGER_BAD_LAYOUT,		/* pd_lower does not match count */
	CUVS_LEDGER_FULL,
	CUVS_LEDGER_BAD_ORDINAL		/* key ordinal is not first_ordinal + count */
} CuvsLedgerStatus;

static inline uint32_t
cuvs_ledger_header_crc(const CuvsLedgerHeader *h)
{
	return cuvs_cover_crc32c(h, offsetof(CuvsLedgerHeader, crc32c));
}

static inline void
cuvs_ledger_write_header(uint8_t *page, const CuvsLedgerHeader *h)
{
	CuvsLedgerHeader w = *h;

	w.crc32c = cuvs_ledger_header_crc(&w);
	memcpy(page + CUVS_COVER_PAGE_HEADER, &w, sizeof(w));
	cuvs_cover_put16(page, CUVS_COVER_OFF_LOWER, (uint16_t) (CUVS_LEDGER_KEYS_AT + CUVS_LEDGER_KEY_SIZE * w.count));
}

/* Initialise an empty ledger page. `next` is the next page of the chain or CUVS_LEDGER_NO_BLOCK. */
static inline void
cuvs_ledger_page_init(uint8_t *page, uint16_t kind, uint64_t generation, uint64_t first_ordinal, uint32_t next, uint32_t seen)
{
	CuvsLedgerHeader h;
	const size_t special = CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE;

	memset(page, 0, CUVS_COVER_PAGE_SIZE);
	cuvs_cover_put16(page, CUVS_COVER_OFF_UPPER, (uint16_t) special);
	cuvs_cover_put16(page, CUVS_COVER_OFF_SPECIAL, (uint16_t) special);
	memcpy(page + special, &next, sizeof(next));
	cuvs_cover_put16(page, special + 4u, kind);
	cuvs_cover_put16(page, special + 6u, CUVS_LEDGER_PAGE_ID);
	memset(&h, 0, sizeof(h));
	h.magic = CUVS_LEDGER_MAGIC;
	h.version = CUVS_LEDGER_VERSION;
	h.kind = kind;
	h.generation = generation;
	h.first_ordinal = first_ordinal;
	h.graph_nblocks_seen = seen;
	cuvs_ledger_write_header(page, &h);
}

/* Check a page and return its header. Nothing is read outside the image. */
static inline CuvsLedgerStatus
cuvs_ledger_validate(const uint8_t *page, CuvsLedgerHeader *out)
{
	const uint32_t special = CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE;
	CuvsLedgerHeader h;
	uint32_t lower, upper;

	if (cuvs_cover_get16(page, CUVS_COVER_OFF_SPECIAL) != special || cuvs_cover_get16(page, special + 6u) != CUVS_LEDGER_PAGE_ID)
		return CUVS_LEDGER_BAD_PAGE;
	lower = cuvs_cover_get16(page, CUVS_COVER_OFF_LOWER);
	upper = cuvs_cover_get16(page, CUVS_COVER_OFF_UPPER);
	if (upper != special || lower < CUVS_LEDGER_KEYS_AT || lower > special)
		return CUVS_LEDGER_BAD_PAGE;
	memcpy(&h, page + CUVS_COVER_PAGE_HEADER, sizeof(h));
	if (h.magic != CUVS_LEDGER_MAGIC || h.version == 0 || h.version > CUVS_LEDGER_VERSION ||
	    (h.kind != CUVS_LEDGER_KIND_KEYS && h.kind != CUVS_LEDGER_KIND_BASE) ||
	    h.count > CUVS_LEDGER_CAPACITY || h.crc32c != cuvs_ledger_header_crc(&h) ||
	    cuvs_cover_get16(page, special + 4u) != h.kind)
		return CUVS_LEDGER_BAD_HEADER;
	if (lower != CUVS_LEDGER_KEYS_AT + CUVS_LEDGER_KEY_SIZE * h.count)
		return CUVS_LEDGER_BAD_LAYOUT;
	if (out != NULL)
		*out = h;
	return CUVS_LEDGER_OK;
}

static inline uint32_t
cuvs_ledger_next(const uint8_t *page)
{
	return cuvs_cover_get32(page, CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE);
}

static inline void
cuvs_ledger_set_next(uint8_t *page, uint32_t next)
{
	memcpy(page + CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE, &next, sizeof(next));
}

/* Append one key and record the relation size seen with it. The page must validate; the key's ordinal must continue
 * the page. */
static inline CuvsLedgerStatus
cuvs_ledger_append_seen(uint8_t *page, const CuvsLedgerKey *k, uint32_t seen)
{
	CuvsLedgerHeader h;
	CuvsLedgerStatus st = cuvs_ledger_validate(page, &h);

	if (st != CUVS_LEDGER_OK)
		return st;
	if (h.count >= CUVS_LEDGER_CAPACITY)
		return CUVS_LEDGER_FULL;
	if (k->ordinal != h.first_ordinal + h.count || k->ordinal > CUVS_LEDGER_ORDINAL_MASK)
		return CUVS_LEDGER_BAD_ORDINAL;
	cuvs_ledger_key_encode(page + CUVS_LEDGER_KEYS_AT + (size_t) CUVS_LEDGER_KEY_SIZE * h.count, k);
	h.count++;
	h.graph_nblocks_seen = seen;
	cuvs_ledger_write_header(page, &h);
	return CUVS_LEDGER_OK;
}

/* Raise the relation size the page remembers by `pages`: blocks that something other than the graph is about to add (a recertification extends the
 * index for its new chain), so that the growth monitor of the next insert does not read them as rows written without keys. 0 ("unknown") stays 0. */
static inline CuvsLedgerStatus
cuvs_ledger_raise_seen(uint8_t *page, uint32_t pages)
{
	CuvsLedgerHeader h;
	CuvsLedgerStatus st = cuvs_ledger_validate(page, &h);

	if (st != CUVS_LEDGER_OK)
		return st;
	if (h.graph_nblocks_seen != 0)
	{
		h.graph_nblocks_seen += pages;
		cuvs_ledger_write_header(page, &h);
	}
	return CUVS_LEDGER_OK;
}

/* Append one key leaving the recorded relation size as it is. */
static inline CuvsLedgerStatus
cuvs_ledger_append(uint8_t *page, const CuvsLedgerKey *k)
{
	CuvsLedgerHeader h;
	CuvsLedgerStatus st = cuvs_ledger_validate(page, &h);

	if (st != CUVS_LEDGER_OK)
		return st;
	return cuvs_ledger_append_seen(page, k, h.graph_nblocks_seen);
}

/* Key `i` of a validated page (i < count), else nonzero. */
static inline int
cuvs_ledger_key_at(const uint8_t *page, uint32_t i, CuvsLedgerKey *out)
{
	CuvsLedgerHeader h;

	/* validate already limits count to the capacity; the second bound lets the compiler see that the copy stays in the page */
	if (cuvs_ledger_validate(page, &h) != CUVS_LEDGER_OK || i >= h.count || i >= CUVS_LEDGER_CAPACITY)
		return 1;
	return cuvs_ledger_key_decode(page + CUVS_LEDGER_KEYS_AT + (size_t) CUVS_LEDGER_KEY_SIZE * i, out);
}

typedef enum {
	CUVS_LEDGER_WALK_OK = 0,
	CUVS_LEDGER_WALK_UNREADABLE,	/* a page could not be read */
	CUVS_LEDGER_WALK_INVALID,	/* a page is not a valid ledger page */
	CUVS_LEDGER_WALK_STALE,		/* generation differs: the chain was replaced under the reader (retry) */
	CUVS_LEDGER_WALK_GAP,		/* first_ordinal does not continue the previous page */
	CUVS_LEDGER_WALK_TOO_LONG,	/* more pages than the caller allows (a loop, or a damaged next pointer) */
	CUVS_LEDGER_WALK_ABORTED,	/* the key callback asked to stop */
	CUVS_LEDGER_WALK_BAD_KEY,	/* a key whose crc does not match: a flipped bit in a heap TID or the like */
	CUVS_LEDGER_WALK_TAIL_MISSING	/* the chain ended without passing the page the descriptor names as its tail (it was cut) */
} CuvsLedgerWalkStatus;

/* Walk a chain from `head`, calling key_cb for every key in order. A page must validate, carry `generation`, and
 * continue the previous page's ordinals, and every key must match its crc. `max_pages` bounds the walk.
 * `must_include` is a block the chain has to pass (the tail the descriptor names), or CUVS_LEDGER_NO_BLOCK: a chain whose
 * next pointer was overwritten ends early and would otherwise read as complete. A reader that races with inserts may find
 * the chain longer than its descriptor copy says, never shorter. Fills *total with the keys seen. */
static inline CuvsLedgerWalkStatus
cuvs_ledger_walk(int (*read_block)(void *ctx, uint32_t blk, uint8_t *dst), void *ctx, uint32_t head,
		 uint64_t generation, uint32_t max_pages, uint64_t first_ordinal, uint32_t must_include,
		 int (*key_cb)(void *kctx, const CuvsLedgerKey *k), void *kctx, uint64_t *total)
{
	uint8_t page[CUVS_COVER_PAGE_SIZE];
	uint32_t blk = head, pages = 0;
	uint64_t next_ordinal = first_ordinal;
	int included = must_include == CUVS_LEDGER_NO_BLOCK;

	*total = 0;
	while (blk != CUVS_LEDGER_NO_BLOCK) {
		CuvsLedgerHeader h;
		uint32_t i;

		if (pages++ >= max_pages)
			return CUVS_LEDGER_WALK_TOO_LONG;
		if (read_block(ctx, blk, page) != 0)
			return CUVS_LEDGER_WALK_UNREADABLE;
		if (cuvs_ledger_validate(page, &h) != CUVS_LEDGER_OK)
			return CUVS_LEDGER_WALK_INVALID;
		if (h.generation != generation)
			return CUVS_LEDGER_WALK_STALE;
		if (h.first_ordinal != next_ordinal)
			return CUVS_LEDGER_WALK_GAP;
		for (i = 0; i < h.count; i++) {
			CuvsLedgerKey k;

			if (cuvs_ledger_key_decode(page + CUVS_LEDGER_KEYS_AT + (size_t) CUVS_LEDGER_KEY_SIZE * i, &k) != 0)
				return CUVS_LEDGER_WALK_BAD_KEY;
			if (k.ordinal != h.first_ordinal + i)
				return CUVS_LEDGER_WALK_INVALID;
			if (key_cb != NULL && key_cb(kctx, &k) != 0)
				return CUVS_LEDGER_WALK_ABORTED;
		}
		if (blk == must_include)
			included = 1;
		next_ordinal += h.count;
		*total += h.count;
		blk = cuvs_ledger_next(page);
	}
	return included ? CUVS_LEDGER_WALK_OK : CUVS_LEDGER_WALK_TAIL_MISSING;
}


/*
 * A recertification lists the rows the walk could not reach (BASE keys) and carries over the keys that inserts wrote while the walk ran.
 * A row that was inserted during the walk can be in both: the walk saw its element before its neighbours pointed at it, or the
 * row joined a part of the graph the entry cannot reach. The ledger holds a row once: a carried key whose TID the walk listed is dropped
 * (the listed key already makes the row a candidate). `listed` is ascending and has no repeats; a TID is (block << 16) | offset.
 * keys[0..n) is compacted in place, its order kept, and the number of keys that stay is returned.
 */
static inline uint64_t
cuvs_ledger_tid_code(uint32_t block, uint16_t offset)
{
	return ((uint64_t) block << 16) | offset;
}

static inline size_t
cuvs_ledger_drop_listed(CuvsLedgerKey *keys, size_t n, const uint64_t *listed, size_t nlisted)
{
	size_t		i, w = 0;

	for (i = 0; i < n; i++)
	{
		uint64_t	code = cuvs_ledger_tid_code(keys[i].heap_block, keys[i].heap_offset);
		size_t		lo = 0, hi = nlisted;
		int			found = 0;

		while (lo < hi)
		{
			size_t		mid = lo + (hi - lo) / 2;

			if (listed[mid] == code)
			{
				found = 1;
				break;
			}
			if (listed[mid] < code)
				lo = mid + 1;
			else
				hi = mid;
		}
		if (!found)
			keys[w++] = keys[i];
	}
	return w;
}

/*
 * One copy pass of a recertification: the n keys just read from the old chain (those after the ones an earlier pass already took) are
 * filtered by cuvs_ledger_drop_listed, and the next pass starts after all n of them, kept or not: *advance is n. Advancing by the keys that
 * stayed would read the dropped ones again at the next pass and the keys behind them twice (a key in the ledger twice).
 * Whether a pass can drop anything depends on the timing (the key of a row is written before its element, so a key written after the
 * walk ended cannot be one of the walk's), and a unit test, not a table, fixes the rule.
 */
static inline size_t
cuvs_ledger_carry(CuvsLedgerKey *keys, size_t n, const uint64_t *listed, size_t nlisted, size_t *advance)
{
	*advance = n;
	return cuvs_ledger_drop_listed(keys, n, listed, nlisted);
}

#endif
