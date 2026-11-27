/* F2a: ledger page layout and chain walk (src/cuvs_cover_ledger.h). Pure C, no PostgreSQL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cuvs_cover_ledger.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static CuvsLedgerKey key(uint64_t ordinal)
{
	CuvsLedgerKey k = {ordinal, 0x1000000000000ull + ordinal, (uint32_t) (ordinal * 7), (uint16_t) (ordinal % 250 + 1), (uint16_t) (ordinal & 1)};
	return k;
}

/* a chain of pages in memory: block i is pages[i] */
typedef struct Chain { uint8_t (*pages)[CUVS_COVER_PAGE_SIZE]; uint32_t n; uint32_t reads; } Chain;
static int chain_read(void *c, uint32_t blk, uint8_t *dst)
{
	Chain *ch = c;
	ch->reads++;
	if (blk >= ch->n) return 1;
	memcpy(dst, ch->pages[blk], CUVS_COVER_PAGE_SIZE);
	return 0;
}
typedef struct Sink { uint64_t seen, next; int ordered; } Sink;
static int sink_key(void *c, const CuvsLedgerKey *k)
{
	Sink *s = c;
	if (k->ordinal != s->next) s->ordered = 0;
	s->next = k->ordinal + 1;
	s->seen++;
	return 0;
}
static int stop_key(void *c, const CuvsLedgerKey *k) { (void) c; return k->ordinal == 505; }

static void build_chain(Chain *ch, uint32_t npages, uint32_t keys_per_page, uint64_t generation, uint64_t first)
{
	uint32_t p, i;
	uint64_t ord = first;
	ch->pages = calloc(npages, CUVS_COVER_PAGE_SIZE);
	ch->n = npages;
	for (p = 0; p < npages; p++) {
		cuvs_ledger_page_init(ch->pages[p], CUVS_LEDGER_KIND_KEYS, generation, ord, p + 1 < npages ? p + 1 : CUVS_LEDGER_NO_BLOCK, 0);
		for (i = 0; i < keys_per_page; i++) {
			CuvsLedgerKey k = key(ord++);
			CHECK(cuvs_ledger_append(ch->pages[p], &k) == CUVS_LEDGER_OK);
		}
	}
}

int main(void)
{
	uint8_t page[CUVS_COVER_PAGE_SIZE], work[CUVS_COVER_PAGE_SIZE];
	CuvsLedgerHeader h;
	CuvsLedgerKey k;
	uint32_t i;

	/* layout constants */
	CHECK(CUVS_LEDGER_CAPACITY == 338);
	CHECK(CUVS_LEDGER_KEYS_AT == 64);
	CHECK(CUVS_LEDGER_KEYS_AT + 24 * CUVS_LEDGER_CAPACITY <= 8184);

	/* an empty page validates and has the shape pgvector would ignore */
	cuvs_ledger_page_init(page, CUVS_LEDGER_KIND_KEYS, 77, 1000, CUVS_LEDGER_NO_BLOCK, 0);
	CHECK(cuvs_ledger_validate(page, &h) == CUVS_LEDGER_OK);
	CHECK(h.count == 0 && h.generation == 77 && h.first_ordinal == 1000 && h.kind == CUVS_LEDGER_KIND_KEYS);
	CHECK(cuvs_cover_get16(page, 8190) == 0xC0E1);	/* page_id: not pgvector's */
	CHECK(cuvs_cover_get16(page, 14) == 8184 && cuvs_cover_get16(page, 12) == 64);
	CHECK(cuvs_ledger_next(page) == CUVS_LEDGER_NO_BLOCK);

	/* fill the page: 338 keys, then FULL; pd_lower follows the count */
	for (i = 0; i < CUVS_LEDGER_CAPACITY; i++) {
		k = key(1000 + i);
		CHECK(cuvs_ledger_append(page, &k) == CUVS_LEDGER_OK);
		CHECK(cuvs_cover_get16(page, 12) == 64 + 24 * (i + 1));
	}
	k = key(1000 + CUVS_LEDGER_CAPACITY);
	CHECK(cuvs_ledger_append(page, &k) == CUVS_LEDGER_FULL);
	CHECK(cuvs_ledger_validate(page, &h) == CUVS_LEDGER_OK && h.count == 338);
	CHECK(cuvs_cover_get16(page, 12) == 64 + 24 * 338 && 64 + 24 * 338 <= 8184);
	for (i = 0; i < 338; i += 37) {
		CHECK(cuvs_ledger_key_at(page, i, &k) == 0 && k.ordinal == 1000 + i && k.heap_block == (uint32_t) ((1000 + i) * 7));
	}
	CHECK(cuvs_ledger_key_at(page, 338, &k) != 0);

	/* the ordinal must continue the page */
	cuvs_ledger_page_init(page, CUVS_LEDGER_KIND_KEYS, 1, 50, CUVS_LEDGER_NO_BLOCK, 0);
	k = key(51); CHECK(cuvs_ledger_append(page, &k) == CUVS_LEDGER_BAD_ORDINAL);
	k = key(49); CHECK(cuvs_ledger_append(page, &k) == CUVS_LEDGER_BAD_ORDINAL);
	k = key(50); CHECK(cuvs_ledger_append(page, &k) == CUVS_LEDGER_OK);
	k = key(50); CHECK(cuvs_ledger_append(page, &k) == CUVS_LEDGER_BAD_ORDINAL);
	/* one key on the page: key 0 is readable, key 1 (below the capacity but not written) is not */
	CHECK(cuvs_ledger_key_at(page, 0, &k) == 0 && k.ordinal == 50);
	CHECK(cuvs_ledger_key_at(page, 1, &k) != 0);

	/* the relation size a page remembers goes up by what a recertification adds; unknown (0) stays unknown; nothing else changes, the page stays valid */
	cuvs_ledger_page_init(page, CUVS_LEDGER_KIND_KEYS, 5, 10, 99u, 1000);
	k = key(10); CHECK(cuvs_ledger_append(page, &k) == CUVS_LEDGER_OK);
	CHECK(cuvs_ledger_raise_seen(page, 276) == CUVS_LEDGER_OK);
	CHECK(cuvs_ledger_validate(page, &h) == CUVS_LEDGER_OK && h.graph_nblocks_seen == 1276 && h.count == 1 && h.first_ordinal == 10 && h.generation == 5);
	CHECK(cuvs_ledger_next(page) == 99u);
	CHECK(cuvs_ledger_key_at(page, 0, &k) == 0 && k.ordinal == 10);
	cuvs_ledger_page_init(page, CUVS_LEDGER_KIND_KEYS, 5, 10, CUVS_LEDGER_NO_BLOCK, 0);
	CHECK(cuvs_ledger_raise_seen(page, 276) == CUVS_LEDGER_OK);
	CHECK(cuvs_ledger_validate(page, &h) == CUVS_LEDGER_OK && h.graph_nblocks_seen == 0);
	memset(work, 0, sizeof(work));
	CHECK(cuvs_ledger_raise_seen(work, 1) == CUVS_LEDGER_BAD_PAGE);

	/* a page that is not a ledger page: zero, pgvector element page, hole-dropped image */
	memset(work, 0, sizeof(work));
	CHECK(cuvs_ledger_validate(work, &h) == CUVS_LEDGER_BAD_PAGE);
	cuvs_cover_put16(work, 16, 8184); cuvs_cover_put16(work, 14, 8184); cuvs_cover_put16(work, 12, 24); cuvs_cover_put16(work, 8190, 0xFF90);
	CHECK(cuvs_ledger_validate(work, &h) == CUVS_LEDGER_BAD_PAGE);

	/* every single-bit flip of a ledger page with keys: the header, the layout fields and the special area are
	 * protected (the result is the same header or an error), and so is every bit of every key (its own crc) */
	cuvs_ledger_page_init(page, CUVS_LEDGER_KIND_KEYS, 5, 10, 9, 0);
	for (i = 0; i < 20; i++) { k = key(10 + i); CHECK(cuvs_ledger_append(page, &k) == CUVS_LEDGER_OK); }
	{
		CuvsLedgerHeader base;
		size_t bit;
		uint32_t detected = 0, same = 0, next_changed = 0;
		CHECK(cuvs_ledger_validate(page, &base) == CUVS_LEDGER_OK);
		for (bit = 0; bit < (size_t) CUVS_COVER_PAGE_SIZE * 8; bit++) {
			size_t byte = bit / 8;
			int in_keys = byte >= CUVS_LEDGER_KEYS_AT && byte < CUVS_LEDGER_KEYS_AT + 24u * 20u;
			memcpy(work, page, sizeof(work));
			work[byte] ^= (uint8_t) (1u << (bit % 8));
			if (cuvs_ledger_validate(work, &h) != CUVS_LEDGER_OK) { detected++; continue; }
			if (h.count == base.count && h.generation == base.generation && h.first_ordinal == base.first_ordinal && h.kind == base.kind) {
				if (byte >= 8184 && byte < 8188) { next_changed++; continue; }	/* nextblkno: see the stated limit below */
				same++;
				if (in_keys) {	/* the page still validates; the key that was hit must not read */
					CuvsLedgerKey kk;
					CHECK(cuvs_ledger_key_at(work, (uint32_t) ((byte - CUVS_LEDGER_KEYS_AT) / 24u), &kk) != 0);
				}
				if (!in_keys && byte >= 24 && byte < 64 && !(byte >= 36)) CHECK(0);	/* a silent flip inside the LH */
				if (!in_keys && byte >= 8188) CHECK(0);			/* kind and page_id are checked */
				if (byte >= 12 && byte < 18) CHECK(0);			/* pd_lower, pd_upper and pd_special are checked */
			} else {
				CHECK(0);	/* a flip changed what the reader would use without being noticed */
			}
		}
		CHECK(detected > 0 && same > 0);
		/* nextblkno is the one field that changes silently (a stated limit: the chain walk checks generation and the
		 * ordinals of the page it lands on, and a chain longer than expected ends at max_pages) */
		CHECK(next_changed == 32);
	}

	/* a key survives encode and decode with every field, and refuses an ordinal that does not fit 48 bits */
	{
		CuvsLedgerKey a, b;
		uint8_t raw[24];
		int n;
		for (n = 0; n < 2000; n++) {
			a.ordinal = ((uint64_t) n * 0x9E3779B97F4A7C15ull) & CUVS_LEDGER_ORDINAL_MASK;
			a.creator_fullxid = (uint64_t) n * 0xD1B54A32D192ED03ull;
			a.heap_block = (uint32_t) (n * 2654435761u);
			a.heap_offset = (uint16_t) (n * 40503u);
			a.flags = (uint16_t) (n * 977u);
			cuvs_ledger_key_encode(raw, &a);
			memset(&b, 0, sizeof(b));
			CHECK(cuvs_ledger_key_decode(raw, &b) == 0);
			CHECK(b.ordinal == a.ordinal && b.creator_fullxid == a.creator_fullxid && b.heap_block == a.heap_block &&
			      b.heap_offset == a.heap_offset && b.flags == a.flags);
		}
		/* the bits above 48 of an ordinal are not part of the key: they must not spill into the crc field */
		a.ordinal = ((uint64_t) 1 << 50) | 5u; a.creator_fullxid = 1; a.heap_block = 2; a.heap_offset = 3; a.flags = 4;
		cuvs_ledger_key_encode(raw, &a);
		CHECK(cuvs_ledger_key_decode(raw, &b) == 0 && b.ordinal == 5u);
		cuvs_ledger_page_init(page, CUVS_LEDGER_KIND_KEYS, 5, (uint64_t) 1 << 48, CUVS_LEDGER_NO_BLOCK, 0);
		k = key(0);
		k.ordinal = (uint64_t) 1 << 48;
		CHECK(cuvs_ledger_append(page, &k) == CUVS_LEDGER_BAD_ORDINAL);
	}

	/* headers that carry a correct crc but a value no writer produces (the crc alone must not be the only guard) */
	{
		uint32_t t;
		struct { uint32_t magic; uint16_t version, kind; uint32_t count; } bad[] = {
			{0, 1, 1, 0}, {CUVS_LEDGER_MAGIC ^ 1u, 1, 1, 0}, {CUVS_LEDGER_MAGIC, 0, 1, 0}, {CUVS_LEDGER_MAGIC, 2, 1, 0},
			{CUVS_LEDGER_MAGIC, 1, 0, 0}, {CUVS_LEDGER_MAGIC, 1, 3, 0}, {CUVS_LEDGER_MAGIC, 1, 1, 339},
		};
		for (t = 0; t < sizeof(bad) / sizeof(bad[0]); t++) {
			CuvsLedgerHeader w;
			cuvs_ledger_page_init(page, CUVS_LEDGER_KIND_KEYS, 5, 10, CUVS_LEDGER_NO_BLOCK, 0);
			memcpy(&w, page + CUVS_COVER_PAGE_HEADER, sizeof(w));
			w.magic = bad[t].magic; w.version = bad[t].version; w.kind = bad[t].kind; w.count = bad[t].count;
			cuvs_ledger_write_header(page, &w);	/* writes the matching crc and pd_lower */
			cuvs_cover_put16(page, 8188, w.kind);	/* and the mirror agrees, so only the kind rule can refuse it */
			CHECK(cuvs_ledger_validate(page, &h) != CUVS_LEDGER_OK);
		}
		/* kind of the header and kind mirrored in the special area must agree */
		cuvs_ledger_page_init(page, CUVS_LEDGER_KIND_KEYS, 5, 10, CUVS_LEDGER_NO_BLOCK, 0);
		cuvs_cover_put16(page, 8188, CUVS_LEDGER_KIND_BASE);
		CHECK(cuvs_ledger_validate(page, &h) == CUVS_LEDGER_BAD_HEADER);
		/* pd_special that is not the real special offset, everything else right */
		cuvs_ledger_page_init(page, CUVS_LEDGER_KIND_KEYS, 5, 10, CUVS_LEDGER_NO_BLOCK, 0);
		cuvs_cover_put16(page, 16, 8176);
		CHECK(cuvs_ledger_validate(page, &h) == CUVS_LEDGER_BAD_PAGE);
		/* pd_lower disagrees with the count while staying inside the page */
		cuvs_ledger_page_init(page, CUVS_LEDGER_KIND_KEYS, 5, 10, CUVS_LEDGER_NO_BLOCK, 0);
		cuvs_cover_put16(page, 12, 80);
		CHECK(cuvs_ledger_validate(page, &h) == CUVS_LEDGER_BAD_LAYOUT);
	}

	/* the relation size seen with the last key (growth monitor) */
	{
		CuvsLedgerKey k2;
		size_t bit;
		cuvs_ledger_page_init(page, CUVS_LEDGER_KIND_KEYS, 5, 10, CUVS_LEDGER_NO_BLOCK, 777);
		CHECK(cuvs_ledger_validate(page, &h) == CUVS_LEDGER_OK && h.graph_nblocks_seen == 777);
		k2 = key(10);
		CHECK(cuvs_ledger_append(page, &k2) == CUVS_LEDGER_OK);
		CHECK(cuvs_ledger_validate(page, &h) == CUVS_LEDGER_OK && h.graph_nblocks_seen == 777);	/* left as it was */
		k2 = key(11);
		CHECK(cuvs_ledger_append_seen(page, &k2, 900) == CUVS_LEDGER_OK);
		CHECK(cuvs_ledger_validate(page, &h) == CUVS_LEDGER_OK && h.graph_nblocks_seen == 900 && h.count == 2);
		for (bit = (CUVS_COVER_PAGE_HEADER + 28) * 8; bit < (CUVS_COVER_PAGE_HEADER + 32) * 8; bit++) {
			memcpy(work, page, sizeof(work));
			work[bit / 8] ^= (uint8_t) (1u << (bit % 8));
			CHECK(cuvs_ledger_validate(work, &h) == CUVS_LEDGER_BAD_HEADER);	/* the crc covers the seen field */
		}
	}

	/* chain walk */
	{
		Chain ch; Sink sink; uint64_t total;
		build_chain(&ch, 4, 100, 9, 500);
		memset(&sink, 0, sizeof(sink)); sink.ordered = 1; sink.next = 500;
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 500, CUVS_LEDGER_NO_BLOCK, sink_key, &sink, &total) == CUVS_LEDGER_WALK_OK);
		CHECK(total == 400 && sink.seen == 400 && sink.ordered);
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 10, 10, 500, CUVS_LEDGER_NO_BLOCK, NULL, NULL, &total) == CUVS_LEDGER_WALK_STALE);		/* other generation */
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 3, 500, CUVS_LEDGER_NO_BLOCK, NULL, NULL, &total) == CUVS_LEDGER_WALK_TOO_LONG);	/* 4 pages, 3 allowed */
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 499, CUVS_LEDGER_NO_BLOCK, NULL, NULL, &total) == CUVS_LEDGER_WALK_GAP);		/* wrong start */
		CHECK(cuvs_ledger_walk(chain_read, &ch, 99, 9, 10, 500, CUVS_LEDGER_NO_BLOCK, NULL, NULL, &total) == CUVS_LEDGER_WALK_UNREADABLE);
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 500, CUVS_LEDGER_NO_BLOCK, stop_key, NULL, &total) == CUVS_LEDGER_WALK_ABORTED);
		/* the chain has to pass the tail the descriptor names */
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 500, 3, NULL, NULL, &total) == CUVS_LEDGER_WALK_OK);
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 500, 7, NULL, NULL, &total) == CUVS_LEDGER_WALK_TAIL_MISSING);
		cuvs_ledger_set_next(ch.pages[1], CUVS_LEDGER_NO_BLOCK);	/* cut after block 1 */
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 500, 3, NULL, NULL, &total) == CUVS_LEDGER_WALK_TAIL_MISSING);
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 500, CUVS_LEDGER_NO_BLOCK, NULL, NULL, &total) == CUVS_LEDGER_WALK_OK);	/* nothing asked */
		cuvs_ledger_set_next(ch.pages[1], 2);
		/* a flipped bit in a heap TID of one key */
		ch.pages[2][CUVS_LEDGER_KEYS_AT + 24 * 7 + 16] ^= 0x10;
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 500, CUVS_LEDGER_NO_BLOCK, NULL, NULL, &total) == CUVS_LEDGER_WALK_BAD_KEY);
		ch.pages[2][CUVS_LEDGER_KEYS_AT + 24 * 7 + 16] ^= 0x10;
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 500, CUVS_LEDGER_NO_BLOCK, NULL, NULL, &total) == CUVS_LEDGER_WALK_OK);
		/* a loop in the chain ends at max_pages */
		cuvs_ledger_set_next(ch.pages[3], 0);
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 50, 500, CUVS_LEDGER_NO_BLOCK, NULL, NULL, &total) != CUVS_LEDGER_WALK_OK);
		cuvs_ledger_set_next(ch.pages[3], CUVS_LEDGER_NO_BLOCK);
		/* the chain was recycled: page 2 now belongs to a newer generation */
		cuvs_ledger_page_init(ch.pages[2], CUVS_LEDGER_KIND_KEYS, 10, 0, CUVS_LEDGER_NO_BLOCK, 0);
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 500, CUVS_LEDGER_NO_BLOCK, NULL, NULL, &total) == CUVS_LEDGER_WALK_STALE);
		/* a damaged page in the middle */
		cuvs_ledger_page_init(ch.pages[2], CUVS_LEDGER_KIND_KEYS, 9, 700, 3, 0);
		ch.pages[2][CUVS_COVER_PAGE_HEADER] ^= 1;
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 500, CUVS_LEDGER_NO_BLOCK, NULL, NULL, &total) == CUVS_LEDGER_WALK_INVALID);
		free(ch.pages);
		/* a key whose ordinal does not match its position inside a valid-looking page */
		build_chain(&ch, 1, 10, 9, 500);
		{
			CuvsLedgerKey bad = key(999);
			cuvs_ledger_key_encode(ch.pages[0] + CUVS_LEDGER_KEYS_AT + 24 * 4, &bad);	/* a valid key at the wrong position */
		}
		CHECK(cuvs_ledger_walk(chain_read, &ch, 0, 9, 10, 500, CUVS_LEDGER_NO_BLOCK, NULL, NULL, &total) == CUVS_LEDGER_WALK_INVALID);
		free(ch.pages);
	}

	/* a carried key for a row the walk already listed is dropped (review 2f6789ed C3: a TID twice in one ledger) */
	{
		CuvsLedgerKey ks[6];
		uint64_t listed[3];
		size_t i, kept;

		for (i = 0; i < 6; i++) {
			ks[i] = key(i);
			ks[i].heap_block = (uint32_t) (10 + i);
			ks[i].heap_offset = (uint16_t) (1 + i);
		}
		listed[0] = cuvs_ledger_tid_code(11, 2);
		listed[1] = cuvs_ledger_tid_code(13, 4);
		listed[2] = cuvs_ledger_tid_code(99, 1);
		kept = cuvs_ledger_drop_listed(ks, 6, listed, 3);
		CHECK(kept == 4);
		CHECK(ks[0].ordinal == 0 && ks[1].ordinal == 2 && ks[2].ordinal == 4 && ks[3].ordinal == 5);	/* the order is kept */
		kept = cuvs_ledger_drop_listed(ks, 4, listed, 0);
		CHECK(kept == 4);	/* nothing listed: nothing dropped */
		listed[0] = cuvs_ledger_tid_code(10, 1);
		kept = cuvs_ledger_drop_listed(ks, 4, listed, 1);
		CHECK(kept == 3 && ks[0].ordinal == 2);	/* the first key, and a list of one */
		listed[0] = cuvs_ledger_tid_code(14, 5);
		kept = cuvs_ledger_drop_listed(ks, 3, listed, 1);
		CHECK(kept == 2 && ks[1].ordinal == 5);	/* the last key but one: a TID is block and offset together, a block alone is not a match */
		listed[0] = cuvs_ledger_tid_code(14, 6);
		kept = cuvs_ledger_drop_listed(ks, 2, listed, 1);
		CHECK(kept == 2);	/* same block, other offset */
		/* a listed TID to the right of the middle of the list */
		for (i = 0; i < 6; i++) {
			ks[i] = key(i);
			ks[i].heap_block = (uint32_t) (10 + i);
			ks[i].heap_offset = (uint16_t) (1 + i);
		}
		listed[0] = cuvs_ledger_tid_code(11, 2);
		listed[1] = cuvs_ledger_tid_code(12, 3);
		listed[2] = cuvs_ledger_tid_code(15, 6);
		kept = cuvs_ledger_drop_listed(ks, 6, listed, 3);
		CHECK(kept == 3 && ks[0].ordinal == 0 && ks[1].ordinal == 3 && ks[2].ordinal == 4);
	}
	{
		/* a copy pass takes the keys it read, drops the listed ones and moves on by what it READ: a second pass that moved on by what stayed would read
		 * the dropped keys again and the keys behind them twice */
		CuvsLedgerKey ks[5];
		uint64_t listed[2];
		size_t i, kept, advance = 0;

		for (i = 0; i < 5; i++) {
			ks[i] = key(i);
			ks[i].heap_block = (uint32_t) (20 + i);
			ks[i].heap_offset = (uint16_t) (1 + i);
		}
		listed[0] = cuvs_ledger_tid_code(21, 2);
		listed[1] = cuvs_ledger_tid_code(23, 4);
		kept = cuvs_ledger_carry(ks, 5, listed, 2, &advance);
		CHECK(kept == 3 && advance == 5);
		CHECK(ks[0].ordinal == 0 && ks[1].ordinal == 2 && ks[2].ordinal == 4);
		kept = cuvs_ledger_carry(ks, 3, listed, 0, &advance);
		CHECK(kept == 3 && advance == 3);
		kept = cuvs_ledger_carry(ks, 0, listed, 2, &advance);
		CHECK(kept == 0 && advance == 0);
	}
	if (failures) {
		fprintf(stderr, "test_cover_ledger: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cover_ledger: all checks passed\n");
	return 0;
}
