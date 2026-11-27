/* physical layout and classification of the cover descriptor.
 * Pure C, no PostgreSQL. The pages here are plain 8192-byte images. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cuvs_cover_format.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void put16(uint8_t *p, size_t off, uint16_t v) { memcpy(p + off, &v, 2); }
static void put32(uint8_t *p, size_t off, uint32_t v) { memcpy(p + off, &v, 4); }
static uint16_t get16(const uint8_t *p, size_t off) { uint16_t v; memcpy(&v, p + off, 2); return v; }
static uint32_t get32(const uint8_t *p, size_t off) { uint32_t v; memcpy(&v, p + off, 4); return v; }

/* A pgvector 0.8.7 metapage as an older pg_cuvs build left it: header, HnswMetaPageData, optional policy
 * receipt (extends pd_lower), optional connectivity record above pd_upper. */
static void make_legacy_meta(uint8_t *page, int with_conn, uint16_t lower)
{
	memset(page, 0, CUVS_COVER_PAGE_SIZE);
	put16(page, 12, lower);
	put16(page, 16, CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE);
	put16(page, 14, CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE - (with_conn ? CUVS_COVER_CONN_SIZE : 0));
	put32(page, 24, CUVS_COVER_PGV_MAGIC);
	put32(page, 28, 1);
	if (with_conn)
		memset(page + CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE - CUVS_COVER_CONN_SIZE, 0x5A, CUVS_COVER_CONN_SIZE);
}

static void make_head(uint8_t *page, int witness)
{
	memset(page, 0, CUVS_COVER_PAGE_SIZE);
	put16(page, 12, 24);
	put16(page, 14, CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE);
	put16(page, 16, CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE);
	put16(page, CUVS_COVER_PAGE_SIZE - 2, 0xFF90);	/* pgvector page_id */
	if (witness)
		cuvs_cover_stage_head_witness(page);
}

static void make_descriptor(CuvsCoverDescriptor *d, uint8_t state, uint8_t reason)
{
	memset(d, 0, sizeof(*d));
	d->magic = CUVS_COVER_D_MAGIC;
	d->format_version = CUVS_COVER_FORMAT_VERSION;
	d->min_reader_version = 1;
	d->d_len = CUVS_COVER_D_SIZE;
	d->state = state;
	d->reason = reason;
	d->flags = CUVS_COVER_FLAG_HEAD_WITNESS;
	d->generation = 0x1122334455667788ull;
	d->ledger_head = 2;
	d->ledger_tail = 2;
	d->free_head = 0xFFFFFFFFu;
	d->base_rows = 1000;
	cuvs_cover_descriptor_seal(d);
}

static void make_new_meta(uint8_t *page, uint8_t state, uint8_t reason)
{
	CuvsCoverDescriptor d;
	make_legacy_meta(page, 1, 212);
	make_descriptor(&d, state, reason);
	CHECK(cuvs_cover_stage_tail(page, NULL, &d) == 0);
}

static uint64_t rng_next(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static CuvsCoverResult classify(const uint8_t *meta, const uint8_t *head)
{
	CuvsCoverResult r;
	cuvs_cover_classify(meta, head, &r);
	return r;
}

int main(void)
{
	uint8_t meta[CUVS_COVER_PAGE_SIZE], head[CUVS_COVER_PAGE_SIZE], head_w[CUVS_COVER_PAGE_SIZE], work[CUVS_COVER_PAGE_SIZE];
	CuvsCoverResult r;

	/* CRC-32C (Castagnoli) check value */
	CHECK(cuvs_cover_crc32c("123456789", 9) == 0xE3069283u);

	/* layout constants of the design */
	CHECK(CUVS_COVER_D_SIZE == 184);
	CHECK(CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE - CUVS_COVER_CONN_SIZE - CUVS_COVER_D_SIZE == 7936);

	{
		/* the fields that follow last_fail_needed: the layout other libraries read (cert_dropped_keys, pending chain), and the BASE keys that are not inserts */
		CuvsCoverDescriptor d;

		memset(&d, 0, sizeof(d));
		CHECK(offsetof(CuvsCoverDescriptor, cert_dropped_keys) == 128 && offsetof(CuvsCoverDescriptor, pending_head) == 136 &&
		      offsetof(CuvsCoverDescriptor, pending_generation) == 144 && offsetof(CuvsCoverDescriptor, crc32c) == 180);
		d.base_missing = 100;
		CHECK(cuvs_cover_base_keys(&d) == 100);
		d.cert_dropped_keys = 7;
		CHECK(cuvs_cover_base_keys(&d) == 93);
		d.cert_dropped_keys = 1000;	/* more dropped than listed cannot happen; it must not wrap */
		CHECK(cuvs_cover_base_keys(&d) == 0);
	}

	make_head(head, 0);
	make_head(head_w, 1);
	CHECK(get16(head_w, CUVS_COVER_PAGE_SIZE - 4) == 0xC0E1);

	/* old indexes are LEGACY: with and without the connectivity record, with and without receipts */
	make_legacy_meta(meta, 0, 52);
	r = classify(meta, head); CHECK(r.cls == CUVS_COVER_LEGACY);
	make_legacy_meta(meta, 1, 52);
	r = classify(meta, head); CHECK(r.cls == CUVS_COVER_LEGACY);
	make_legacy_meta(meta, 1, 248);
	r = classify(meta, head); CHECK(r.cls == CUVS_COVER_LEGACY);

	/* new indexes: every state */
	make_new_meta(meta, CUVS_COVER_STATE_CERTIFIED, 0);
	r = classify(meta, head_w);
	CHECK(r.cls == CUVS_COVER_CERTIFIED && r.needs_ledger_check && r.ledger_head == 2 && r.ledger_tail == 2 && r.generation == 0x1122334455667788ull);
	r = classify(meta, head);	/* a valid descriptor does not need the block 1 witness */
	CHECK(r.cls == CUVS_COVER_CERTIFIED);
	make_new_meta(meta, CUVS_COVER_STATE_LOST, CUVS_COVER_REASON_VACUUM);
	r = classify(meta, head_w); CHECK(r.cls == CUVS_COVER_LOST && r.stored_reason == CUVS_COVER_REASON_VACUUM);
	CHECK(r.ledger_head == 2 && r.ledger_tail == 2 && r.generation == 0x1122334455667788ull);	/* LOST carries the chain it still has */
	make_new_meta(meta, CUVS_COVER_STATE_OFF, CUVS_COVER_REASON_USER);
	r = classify(meta, head_w); CHECK(r.cls == CUVS_COVER_OFF && r.stored_reason == CUVS_COVER_REASON_USER);

	/* the descriptor and the connectivity record are both above pd_upper and the hole is untouched */
	make_new_meta(meta, CUVS_COVER_STATE_CERTIFIED, 0);
	CHECK(get16(meta, 14) == 7936);
	CHECK(get16(meta, 12) == 212);

	/* ---- the LEGACY boundary: of all 65536 pd_upper values exactly the two old shapes are LEGACY ---- */
	{
		uint32_t v, legacy_without = 0, legacy_with = 0, descriptor_lost = 0;
		make_new_meta(work, CUVS_COVER_STATE_CERTIFIED, 0);
		for (v = 0; v < 65536; v++) {
			put16(work, 14, (uint16_t) v);
			r = classify(work, head);
			if (r.cls == CUVS_COVER_LEGACY) legacy_without++;
			r = classify(work, head_w);
			if (v != 7936)	/* the descriptor starts at pd_upper: any other value cannot be a healthy index */
				CHECK(r.cls != CUVS_COVER_CERTIFIED);
			if (r.cls == CUVS_COVER_LEGACY) legacy_with++;
			if (r.cls == CUVS_COVER_UNTRUSTED && r.reason == CUVS_COVER_UNTRUSTED_DESCRIPTOR_LOST) descriptor_lost++;
		}
		CHECK(legacy_without == 2);	/* 8184 and 8120, and only when block 1 carries no witness */
		CHECK(legacy_with == 0);	/* with the witness no pd_upper value makes the index LEGACY */
		CHECK(descriptor_lost == 2);
	}

	/* ---- Hamming distance: 1..3 bit flips of pd_upper never reach a LEGACY shape; 4 do ---- */
	{
		uint32_t mask, reach4 = 0;
		make_new_meta(work, CUVS_COVER_STATE_CERTIFIED, 0);
		for (mask = 1; mask < 65536; mask++) {
			int bits = __builtin_popcount(mask);
			if (bits > 4) continue;
			put16(work, 14, (uint16_t) (7936 ^ mask));
			r = classify(work, head);
			if (bits <= 3)
				CHECK(r.cls != CUVS_COVER_LEGACY);
			else if (r.cls == CUVS_COVER_LEGACY)
				reach4++;
		}
		CHECK(reach4 >= 1);	/* documents the bound: four flips can */
	}

	/* ---- exhaustive single-bit flips of the metapage of a new index never give LEGACY ---- */
	{
		size_t bit;
		uint32_t same = 0, untrusted = 0, error = 0;
		make_new_meta(meta, CUVS_COVER_STATE_CERTIFIED, 0);
		for (bit = 0; bit < (size_t) CUVS_COVER_PAGE_SIZE * 8; bit++) {
			memcpy(work, meta, sizeof(work));
			work[bit / 8] ^= (uint8_t) (1u << (bit % 8));
			r = classify(work, head);
			CHECK(r.cls != CUVS_COVER_LEGACY);
			if (r.cls == CUVS_COVER_CERTIFIED) same++;
			else if (r.cls == CUVS_COVER_UNTRUSTED) untrusted++;
			else if (r.cls == CUVS_COVER_ERROR) error++;
			else CHECK(0);	/* a flip may not turn a CERTIFIED index into OFF or LOST either */
		}
		CHECK(untrusted > 0 && error > 0 && same > 0);
	}

	/* every flip inside the descriptor itself is detected (CRC, magic, layout, version) */
	{
		size_t bit, base = 7936;
		make_new_meta(meta, CUVS_COVER_STATE_CERTIFIED, 0);
		for (bit = base * 8; bit < (base + CUVS_COVER_D_SIZE) * 8; bit++) {
			memcpy(work, meta, sizeof(work));
			work[bit / 8] ^= (uint8_t) (1u << (bit % 8));
			r = classify(work, head_w);
			CHECK(r.cls == CUVS_COVER_UNTRUSTED);
		}
	}

	/* flips in the hole, in the connectivity record and in the page LSN do not matter */
	{
		static const size_t offs[] = {100, 3000, 7935, 8120, 8150, 8183, 0, 8};
		size_t i;
		make_new_meta(meta, CUVS_COVER_STATE_CERTIFIED, 0);
		for (i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
			memcpy(work, meta, sizeof(work));
			work[offs[i]] ^= 0x01;
			r = classify(work, head_w);
			CHECK(r.cls == CUVS_COVER_CERTIFIED);
		}
	}

	/* ---- D3: single-bit flips of an OLD index never claim a guarantee ---- */
	{
		size_t bit;
		int shape;
		for (shape = 0; shape < 2; shape++) {
			make_legacy_meta(meta, shape, 52);
			for (bit = 0; bit < (size_t) CUVS_COVER_PAGE_SIZE * 8; bit++) {
				memcpy(work, meta, sizeof(work));
				work[bit / 8] ^= (uint8_t) (1u << (bit % 8));
				r = classify(work, head);
				CHECK(r.cls == CUVS_COVER_LEGACY || r.cls == CUVS_COVER_UNTRUSTED || r.cls == CUVS_COVER_ERROR);
			}
		}
	}

	/* the block 1 witness: its flips cannot hide a lost descriptor of a LEGACY-shaped page */
	{
		size_t bit;
		uint32_t legacy = 0;
		make_new_meta(meta, CUVS_COVER_STATE_CERTIFIED, 0);
		put16(meta, 14, 8184);	/* descriptor lost: shape reverted to LEGACY */
		for (bit = (size_t) (CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE) * 8; bit < (size_t) CUVS_COVER_PAGE_SIZE * 8; bit++) {
			memcpy(work, head_w, sizeof(work));
			work[bit / 8] ^= (uint8_t) (1u << (bit % 8));
			r = classify(meta, work);
			if (r.cls == CUVS_COVER_LEGACY) legacy++;
		}
		/* clearing the witness by one bit is possible (0xC0E1 -> another value), so the witness is a cheap
		 * extra defense and not a proof; the count documents which special-area bits clear it */
		CHECK(legacy >= 1);
	}

	/* ---- format version rules ---- */
	{
		CuvsCoverDescriptor d;
		make_legacy_meta(meta, 1, 212);
		make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
		d.format_version = 2; d.min_reader_version = 2; cuvs_cover_descriptor_seal(&d);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w);
		CHECK(r.cls == CUVS_COVER_UNTRUSTED && r.reason == CUVS_COVER_UNTRUSTED_FUTURE_FORMAT);
		make_legacy_meta(meta, 1, 212);
		d.format_version = 2; d.min_reader_version = 1; cuvs_cover_descriptor_seal(&d);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w);
		CHECK(r.cls == CUVS_COVER_CERTIFIED);
		make_legacy_meta(meta, 1, 212);
		d.format_version = 0; d.min_reader_version = 1; cuvs_cover_descriptor_seal(&d);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w);
		CHECK(r.cls == CUVS_COVER_UNTRUSTED);
	}

	/* CERTIFIED needs a generation and a ledger head */
	{
		CuvsCoverDescriptor d;
		make_legacy_meta(meta, 1, 212);
		make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
		d.generation = 0; cuvs_cover_descriptor_seal(&d);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w); CHECK(r.cls == CUVS_COVER_UNTRUSTED);
		make_legacy_meta(meta, 1, 212);
		make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
		d.ledger_head = 0xFFFFFFFFu; cuvs_cover_descriptor_seal(&d);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w); CHECK(r.cls == CUVS_COVER_UNTRUSTED);
		make_legacy_meta(meta, 1, 212);
		make_descriptor(&d, 7, 0);	/* unknown state */
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w); CHECK(r.cls == CUVS_COVER_UNTRUSTED);
	}

	/* ---- sanity: an unusable page is an error, never LEGACY or UNTRUSTED ---- */
	{
		make_legacy_meta(meta, 1, 52);
		memcpy(work, meta, sizeof(work)); put16(work, 16, 8180);
		r = classify(work, head); CHECK(r.cls == CUVS_COVER_ERROR);
		memcpy(work, meta, sizeof(work)); put16(work, 12, 40);
		r = classify(work, head); CHECK(r.cls == CUVS_COVER_ERROR);
		memcpy(work, meta, sizeof(work)); put16(work, 12, 8130);	/* pd_lower > pd_upper (8120) */
		r = classify(work, head); CHECK(r.cls == CUVS_COVER_ERROR);
		memcpy(work, meta, sizeof(work)); put32(work, 24, 0);
		r = classify(work, head); CHECK(r.cls == CUVS_COVER_ERROR);
		memset(work, 0, sizeof(work));
		r = classify(work, head); CHECK(r.cls == CUVS_COVER_ERROR);	/* a zero page */
		r = classify(NULL, head); CHECK(r.cls == CUVS_COVER_ERROR);
	}

	/* a LEGACY-shaped page whose block 1 cannot be read is not LEGACY */
	{
		make_legacy_meta(meta, 1, 52);
		r = classify(meta, NULL);
		CHECK(r.cls == CUVS_COVER_UNTRUSTED && r.reason == CUVS_COVER_UNTRUSTED_HEAD_UNREADABLE);
		memcpy(work, head, sizeof(work)); put16(work, 16, 100);
		r = classify(meta, work);
		CHECK(r.cls == CUVS_COVER_UNTRUSTED && r.reason == CUVS_COVER_UNTRUSTED_HEAD_UNREADABLE);
	}

	/* staging refuses a page it cannot fit */
	{
		CuvsCoverDescriptor d;
		make_legacy_meta(meta, 1, 8000);	/* pd_lower past the descriptor start */
		put16(meta, 14, 8120);
		make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) != 0);
		make_legacy_meta(meta, 1, 212);
		put16(meta, 16, 8000);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) != 0);
	}

	/* ---- writable: "read but do not write" is a field of the result ---- */
	{
		CuvsCoverDescriptor d;
		uint8_t state;
		for (state = CUVS_COVER_STATE_OFF; state <= CUVS_COVER_STATE_LOST; state++) {
			/* this library's own version: it may write */
			make_new_meta(meta, state, 0);
			r = classify(meta, head_w);
			CHECK(r.cls != CUVS_COVER_UNTRUSTED && r.cls != CUVS_COVER_ERROR && r.writable == 1);
			/* a newer format whose min_reader_version still allows reading: readable, not writable */
			make_legacy_meta(meta, 1, 212);
			make_descriptor(&d, state, 0);
			d.format_version = 2; d.min_reader_version = 1; cuvs_cover_descriptor_seal(&d);
			CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
			r = classify(meta, head_w);
			CHECK(r.cls == (state == CUVS_COVER_STATE_OFF ? CUVS_COVER_OFF :
					state == CUVS_COVER_STATE_CERTIFIED ? CUVS_COVER_CERTIFIED : CUVS_COVER_LOST));
			CHECK(r.writable == 0);
			/* this library's own version written explicitly: writable */
			make_legacy_meta(meta, 1, 212);
			make_descriptor(&d, state, 0);
			d.format_version = 1; cuvs_cover_descriptor_seal(&d);
			CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
			r = classify(meta, head_w);
			CHECK(r.writable == 1);
		}
		/* nothing is writable when there is no readable guarded state */
		make_legacy_meta(meta, 1, 52);
		r = classify(meta, head); CHECK(r.cls == CUVS_COVER_LEGACY && r.writable == 0);
		r = classify(meta, NULL); CHECK(r.cls == CUVS_COVER_UNTRUSTED && r.writable == 0);
		memset(work, 0, sizeof(work));
		r = classify(work, head); CHECK(r.cls == CUVS_COVER_ERROR && r.writable == 0);
		r = classify(NULL, head); CHECK(r.cls == CUVS_COVER_ERROR && r.writable == 0);
		/* a descriptor that turns UNTRUSTED after it was decoded must not keep writable */
		make_legacy_meta(meta, 1, 212);
		make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
		d.generation = 0; cuvs_cover_descriptor_seal(&d);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w); CHECK(r.cls == CUVS_COVER_UNTRUSTED && r.writable == 0);
		/* a future format that can no longer be read */
		make_legacy_meta(meta, 1, 212);
		make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
		d.format_version = 2; d.min_reader_version = 2; cuvs_cover_descriptor_seal(&d);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w); CHECK(r.cls == CUVS_COVER_UNTRUSTED && r.writable == 0);
	}

	/* ---- the persistent SUSPECT flag: a CERTIFIED descriptor never carries it, so the combination is
	 * inconsistent even with a valid crc; LOST and OFF may carry it ---- */
	{
		CuvsCoverDescriptor d;
		make_legacy_meta(meta, 1, 212);
		make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
		d.flags |= CUVS_COVER_FLAG_SUSPECT_UNKEYED_WRITER; cuvs_cover_descriptor_seal(&d);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w);
		CHECK(r.cls == CUVS_COVER_UNTRUSTED && r.reason == CUVS_COVER_UNTRUSTED_DESCRIPTOR_INCONSISTENT);
		CHECK(r.writable == 0 && r.needs_ledger_check == 0);
		make_legacy_meta(meta, 1, 212);
		make_descriptor(&d, CUVS_COVER_STATE_LOST, CUVS_COVER_REASON_UNKEYED_WRITER);
		d.flags |= CUVS_COVER_FLAG_SUSPECT_UNKEYED_WRITER; cuvs_cover_descriptor_seal(&d);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w);
		CHECK(r.cls == CUVS_COVER_LOST && r.stored_reason == CUVS_COVER_REASON_UNKEYED_WRITER && r.writable == 1);
		make_legacy_meta(meta, 1, 212);
		make_descriptor(&d, CUVS_COVER_STATE_OFF, CUVS_COVER_REASON_USER);
		d.flags |= CUVS_COVER_FLAG_SUSPECT_UNKEYED_WRITER; cuvs_cover_descriptor_seal(&d);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w);
		CHECK(r.cls == CUVS_COVER_OFF && r.writable == 1);
		/* the flag alone, without the witness flag, is the same inconsistency */
		make_legacy_meta(meta, 1, 212);
		make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
		d.flags = CUVS_COVER_FLAG_SUSPECT_UNKEYED_WRITER; cuvs_cover_descriptor_seal(&d);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		r = classify(meta, head_w);
		CHECK(r.cls == CUVS_COVER_UNTRUSTED && r.reason == CUVS_COVER_UNTRUSTED_DESCRIPTOR_INCONSISTENT);
	}

	/* staging refuses a descriptor whose d_len is not the version-1 size, and a page whose special is wrong */
	{
		CuvsCoverDescriptor d;
		make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
		d.d_len = 192;
		make_legacy_meta(meta, 1, 212);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) != 0);
		d.d_len = 176;
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) != 0);
		make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
		put16(meta, 16, 8176);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) != 0);
		make_legacy_meta(meta, 1, 51);	/* pd_lower inside the pgvector meta data */
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) != 0);
		/* lower == the descriptor start is allowed, one more is not */
		make_legacy_meta(meta, 1, 7936);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		make_legacy_meta(meta, 1, 7937);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) != 0);
	}

	/* a staged page without a connectivity record zeroes the 64 bytes (the old build left 0x5A there) */
	{
		CuvsCoverDescriptor d;
		size_t i;
		make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
		make_legacy_meta(meta, 1, 212);
		CHECK(cuvs_cover_stage_tail(meta, NULL, &d) == 0);
		for (i = 8120; i < 8184; i++)
			CHECK(meta[i] == 0);
		{
			uint8_t conn[CUVS_COVER_CONN_SIZE];
			memset(conn, 0xA7, sizeof(conn));
			make_legacy_meta(meta, 0, 212);
			CHECK(cuvs_cover_stage_tail(meta, conn, &d) == 0);
			for (i = 8120; i < 8184; i++)
				CHECK(meta[i] == 0xA7);
		}
	}

	/* ---- range protection: each size or position check of the classifier is pinned by a case that only
	 * that check catches. `big` holds the page followed by bytes the classifier must never need: a case
	 * is sealed (valid crc) at the position its d_len claims, even past the end of the page, so a missing
	 * check shows up as a different result and not only as an out-of-bounds read. The same page is then
	 * classified from an exactly page-sized heap block, where a sanitizer build catches the read. ---- */
	{
		static uint8_t big[CUVS_COVER_PAGE_SIZE + 65536 + 64];
		struct { uint32_t upper; uint32_t d_len; uint32_t magic; CuvsCoverClass cls; CuvsCoverUntrustedReason reason; } cases[] = {
			/* control: the real descriptor */
			{7936, 184, CUVS_COVER_D_MAGIC, CUVS_COVER_CERTIFIED, CUVS_COVER_UNTRUSTED_NONE},
			/* a later version may append fields inside d_len: 192 bytes ending right below the record */
			{7928, 192, CUVS_COVER_D_MAGIC, CUVS_COVER_CERTIFIED, CUVS_COVER_UNTRUSTED_NONE},
			/* d_len too small although the end position fits and it is a multiple of 8 (min check) */
			{7944, 176, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{8112, 8, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			/* d_len not a multiple of 8 although the end position fits (alignment check) */
			{7935, 185, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7934, 186, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7929, 191, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			/* the end position does not meet the connectivity record (end-position check) */
			{7936, 192, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7936, 176, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7936, 200, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7936, 248, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7936, 0xFFF8, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7936, 0xFFFF, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7936, 0, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7936, 3, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7936, 4, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7936, 180, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{7936, 183, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			/* the magic is wrong although the crc is right (sealed after the damage) */
			{7936, 184, CUVS_COVER_D_MAGIC ^ 1u, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT},
			{7936, 184, 0, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT},
			/* the descriptor does not even have room for its 16-byte head below the record (room check):
			 * 8104 is the last start that fits, 8105 the first that does not */
			{8104, 8, CUVS_COVER_D_MAGIC ^ 1u, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT},
			{8105, 8, CUVS_COVER_D_MAGIC ^ 1u, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{8110, 8, 0, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{8119, 8, 0, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{8121, 8, 0, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{8150, 8, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			{8183, 8, CUVS_COVER_D_MAGIC, CUVS_COVER_UNTRUSTED, CUVS_COVER_UNTRUSTED_LAYOUT},
			/* a start above pd_special is a sanity error, never a descriptor (upper > special check) */
			{8185, 8, CUVS_COVER_D_MAGIC, CUVS_COVER_ERROR, CUVS_COVER_UNTRUSTED_NONE},
			{8190, 8, 0, CUVS_COVER_ERROR, CUVS_COVER_UNTRUSTED_NONE},
			{8192, 8, 0, CUVS_COVER_ERROR, CUVS_COVER_UNTRUSTED_NONE},
			{9000, 8, 0, CUVS_COVER_ERROR, CUVS_COVER_UNTRUSTED_NONE},
			{65535, 0xFFFF, CUVS_COVER_D_MAGIC, CUVS_COVER_ERROR, CUVS_COVER_UNTRUSTED_NONE},
		};
		size_t ci;

		for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
			CuvsCoverDescriptor d;
			CuvsCoverResult from_big, from_exact;
			uint8_t *exact;

			memset(big, 0, sizeof(big));
			make_legacy_meta(big, 1, 212);
			make_descriptor(&d, CUVS_COVER_STATE_CERTIFIED, 0);
			if (cases[ci].upper + CUVS_COVER_D_SIZE <= sizeof(big))
				memcpy(big + cases[ci].upper, &d, CUVS_COVER_D_SIZE);
			put16(big, 14, (uint16_t) cases[ci].upper);
			if (cases[ci].upper + 12u <= sizeof(big)) {
				put32(big, cases[ci].upper, cases[ci].magic);
				put16(big, cases[ci].upper + 8u, (uint16_t) cases[ci].d_len);
			}
			if (cases[ci].d_len >= 16 && (size_t) cases[ci].upper + cases[ci].d_len <= sizeof(big)) {
				uint32_t crc = cuvs_cover_crc32c(big + cases[ci].upper, cases[ci].d_len - 4u);
				memcpy(big + cases[ci].upper + cases[ci].d_len - 4u, &crc, 4);
			}
			from_big = classify(big, head_w);
			if (from_big.cls != cases[ci].cls || (cases[ci].cls == CUVS_COVER_UNTRUSTED && from_big.reason != cases[ci].reason)) {
				fprintf(stderr, "case %zu (upper %u d_len %u): got class %d reason %d\n", ci, cases[ci].upper,
					cases[ci].d_len, (int) from_big.cls, (int) from_big.reason);
				CHECK(0);
			}
			/* the classifier must not need a single byte past the page */
			exact = malloc(CUVS_COVER_PAGE_SIZE);
			CHECK(exact != NULL);
			memcpy(exact, big, CUVS_COVER_PAGE_SIZE);
			from_exact = classify(exact, head_w);
			CHECK(from_exact.cls == from_big.cls && from_exact.reason == from_big.reason);
			free(exact);
		}
	}

	/* every pd_upper value of a healthy index page, the descriptor magic planted at that position:
	 * the result depends only on the position rules, never on bytes past the page */
	{
		uint32_t v, errors = 0, layout = 0, lost_descriptor = 0, other = 0;
		for (v = 0; v < 65536; v++) {
			uint8_t *page = malloc(CUVS_COVER_PAGE_SIZE);
			CHECK(page != NULL);
			make_new_meta(page, CUVS_COVER_STATE_CERTIFIED, 0);
			put16(page, 14, (uint16_t) v);
			if (v + 12u <= CUVS_COVER_PAGE_SIZE)
				put32(page, v, CUVS_COVER_D_MAGIC);	/* a planted magic with whatever follows */
			r = classify(page, head_w);
			if (r.cls == CUVS_COVER_ERROR) errors++;
			else if (r.cls == CUVS_COVER_UNTRUSTED && r.reason == CUVS_COVER_UNTRUSTED_LAYOUT) layout++;
			else if (r.cls == CUVS_COVER_UNTRUSTED && r.reason == CUVS_COVER_UNTRUSTED_DESCRIPTOR_LOST) lost_descriptor++;
			else if (v != 7936) other++;
			free(page);
		}
		CHECK(other == 0);			/* never LEGACY, never a guarded state, except the real position */
		CHECK(lost_descriptor == 2);		/* the two old shapes, with the block 1 witness present */
		CHECK(errors > 0 && layout > 0);
	}

	/* ---- deterministic fuzz with an independent oracle: whatever the bytes, the classifier stays inside the
	 * page, answers ERROR exactly when the page header fails the sanity rules, says LEGACY only for the two
	 * old shapes without the block 1 witness, and reports a guarded state only for a descriptor that really
	 * starts at pd_upper, ends below the record and carries a matching magic and crc ---- */
	{
		uint64_t s = 0x9E3779B97F4A7C15ull;
		uint32_t n;
		uint32_t seen_guarded = 0, seen_legacy = 0, seen_error = 0;
#define NEXT() rng_next(&s)
		for (n = 0; n < 100000; n++) {
			uint8_t *page = malloc(CUVS_COVER_PAGE_SIZE), *hd = malloc(CUVS_COVER_PAGE_SIZE);
			int witness = (int) (NEXT() & 1u);
			uint32_t upper, k, flips, want_error, want_guarded, d_at, d_len;
			static const uint32_t near_upper[] = {7936, 8120, 8184, 8104, 8105, 7944, 7928, 8183, 8185};
			CHECK(page != NULL && hd != NULL);
			if (NEXT() % 4 == 0)
				make_legacy_meta(page, (int) (NEXT() & 1u), 52);
			else
				make_new_meta(page, (uint8_t) (NEXT() % 3), 0);
			make_head(hd, witness);
			upper = (NEXT() % 3 == 0) ? (uint32_t) (NEXT() & 0xFFFFu)
				: near_upper[NEXT() % (sizeof(near_upper) / sizeof(near_upper[0]))] + (uint32_t) (NEXT() % 3) - 1u;
			upper &= 0xFFFFu;
			if (NEXT() % 2 == 0)
				put16(page, 14, (uint16_t) upper);
			else
				upper = get16(page, 14);
			if (upper + 12u <= CUVS_COVER_PAGE_SIZE && NEXT() % 2 == 0) {
				put32(page, upper, CUVS_COVER_D_MAGIC);
				put16(page, upper + 8u, (uint16_t) ((NEXT() % 3 == 0) ? NEXT() : (CUVS_COVER_PAGE_SIZE - 72u - upper)));
			}
			flips = (uint32_t) (NEXT() % 4);
			for (k = 0; k < flips; k++)
				page[NEXT() % CUVS_COVER_PAGE_SIZE] ^= (uint8_t) (1u << (NEXT() % 8));
			if (NEXT() % 2 == 0) {
				upper = get16(page, 14);
				d_len = get16(page, upper + 8u > CUVS_COVER_PAGE_SIZE - 2 ? 0 : upper + 8u);
				if (upper + 12u <= CUVS_COVER_PAGE_SIZE && d_len >= 4 && upper + d_len <= CUVS_COVER_PAGE_SIZE) {
					uint32_t crc = cuvs_cover_crc32c(page + upper, d_len - 4u);
					memcpy(page + upper + d_len - 4u, &crc, 4);	/* sealed after the damage */
				}
			}
			r = classify(page, hd);
			{
				uint32_t lower = get16(page, 12), up = get16(page, 14), special = get16(page, 16);
				uint32_t pgv_magic;
				memcpy(&pgv_magic, page + 24, 4);
				want_error = !(special == 8184 && lower >= 52 && lower <= up && up <= 8184 && pgv_magic == CUVS_COVER_PGV_MAGIC);
				CHECK((r.cls == CUVS_COVER_ERROR) == (want_error != 0));
				if (r.cls == CUVS_COVER_LEGACY) {
					CHECK(up == 8184 || up == 8120);
					CHECK(get16(hd, 8184 + 4u) != CUVS_COVER_HEAD_WITNESS);
					seen_legacy++;
				}
				if (r.cls == CUVS_COVER_ERROR) seen_error++;
				want_guarded = r.cls == CUVS_COVER_CERTIFIED || r.cls == CUVS_COVER_LOST || r.cls == CUVS_COVER_OFF;
				if (want_guarded) {
					uint32_t crc, stored;
					d_at = up;
					CHECK(d_at + 16u <= 8120u);
					CHECK(get32(page, d_at) == CUVS_COVER_D_MAGIC);
					d_len = get16(page, d_at + 8u);
					CHECK(d_len >= CUVS_COVER_D_SIZE && (d_len & 7u) == 0 && d_at + d_len == 8120u);
					crc = cuvs_cover_crc32c(page + d_at, d_len - 4u);
					memcpy(&stored, page + d_at + d_len - 4u, 4);
					CHECK(crc == stored);
					seen_guarded++;
				}
				if (r.cls == CUVS_COVER_CERTIFIED)
					CHECK(r.generation != 0 && r.ledger_head != CUVS_COVER_INVALID_BLOCK);
				if (r.writable)
					CHECK(want_guarded && get16(page, up + 4u) <= CUVS_COVER_FORMAT_VERSION);
			}
			free(page);
			free(hd);
		}
#undef NEXT
		CHECK(seen_guarded > 300 && seen_legacy > 300 && seen_error > 300);
	}

	if (failures) {
		fprintf(stderr, "test_cover_format: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cover_format: all checks passed\n");
	return 0;
}
