#ifndef CUVS_COVER_FORMAT_H
#define CUVS_COVER_FORMAT_H
/* cover descriptor: physical layout and classification.
 *
 * Pure C with no PostgreSQL headers: a page is an 8192-byte image. All integers are host byte order,
 * like the rest of a PostgreSQL data directory.
 *
 * Metapage (block 0) of a pg_cuvs_hnsw index, from low to high addresses:
 *   [PageHeader 24][pgvector HnswMetaPageData 28][optional receipts] <pd_lower>
 *   ...hole...  <pd_upper> [descriptor 184][connectivity record 64][special 8]
 * The descriptor sits ABOVE the hole on purpose: a full-page image or a Generic WAL delta drops the
 * bytes between pd_lower and pd_upper, so a descriptor in the hole would vanish at crash recovery and
 * the index would then look like an old (LEGACY) one.
 *
 * Which indexes are LEGACY: pd_upper equals pd_special (no tail at all) or pd_special - 64 (only the
 * connectivity record) AND block 1 carries no witness. Anything else that is not a valid descriptor
 * is UNTRUSTED, never LEGACY, so damage cannot make a guarded index fall back to approximate answers.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define CUVS_COVER_PAGE_SIZE 8192u
#define CUVS_COVER_PAGE_HEADER 24u
#define CUVS_COVER_PGV_META_END 52u		/* page header + HnswMetaPageData */
#define CUVS_COVER_SPECIAL_SIZE 8u
#define CUVS_COVER_CONN_SIZE 64u
#define CUVS_COVER_D_SIZE 184u
#define CUVS_COVER_PGV_MAGIC 0xA953A953u
#define CUVS_COVER_D_MAGIC 0x43564356u
#define CUVS_COVER_FORMAT_VERSION 1u
#define CUVS_COVER_HEAD_WITNESS 0xC0E1u		/* HnswPageOpaqueData.unused of block 1 */
#define CUVS_COVER_INVALID_BLOCK 0xFFFFFFFFu

/* Page header offsets (PostgreSQL PageHeaderData) */
#define CUVS_COVER_OFF_LOWER 12u
#define CUVS_COVER_OFF_UPPER 14u
#define CUVS_COVER_OFF_SPECIAL 16u
#define CUVS_COVER_OFF_PGV_MAGIC 24u

enum { CUVS_COVER_STATE_OFF = 0, CUVS_COVER_STATE_CERTIFIED = 1, CUVS_COVER_STATE_LOST = 2 };

/* Stored reasons (descriptor.reason) */
enum {
	CUVS_COVER_REASON_NONE = 0,
	CUVS_COVER_REASON_USER = 1,
	CUVS_COVER_REASON_SHAPE = 2,
	CUVS_COVER_REASON_UNSUPPORTED_TYPE = 3,
	CUVS_COVER_REASON_VACUUM = 4,
	CUVS_COVER_REASON_CAPACITY = 5,
	CUVS_COVER_REASON_UNCERTIFIED = 6,
	CUVS_COVER_REASON_UNKEYED_WRITER = 7,
	CUVS_COVER_REASON_RECERT_FAILED = 8,
	CUVS_COVER_REASON_REENABLED = 9,
	CUVS_COVER_REASON_DOWNGRADE = 10,	/* pg_cuvs_cover_invalidate_all(): taken off before an older library or SQL */
	CUVS_COVER_REASON_LEDGER_DAMAGED = 11,	/* a ledger page did not read as the descriptor says: the chain was dropped, a recertification builds a new one */
	CUVS_COVER_REASON_BREAKGLASS = 12	/* never stored: what a CERTIFIED index reads as while the break-glass marker exists */
};

#define CUVS_COVER_FLAG_HEAD_WITNESS 0x0001u
#define CUVS_COVER_FLAG_SUSPECT_UNKEYED_WRITER 0x0002u
/*
 * whether anything was inserted since the certification in force (keys count by themselves; these two cover an insert the
 * verification found reachable, which writes no key, into a page that was there). INSERTED: since cert_time. INSERTED_SINCE_START: since
 * phase 1 of the recertification that runs now, whose cert_time that will be; phase 1 clears it, publication copies it into INSERTED. An insert
 * writes block 0 only when one of the two is missing: once per certification and once per recertification, not once per insert. A reader of the
 * same format_version that does not know the bits ignores them (cuvs_cover_classify looks only at SUSPECT), and an older library's
 * recertification keeps them (it clears only SUSPECT): the next sweep walks the index once more and finds it quiet.
 */
#define CUVS_COVER_FLAG_INSERTED 0x0004u
#define CUVS_COVER_FLAG_INSERTED_SINCE_START 0x0008u
#define CUVS_COVER_INSERT_FLAGS (CUVS_COVER_FLAG_INSERTED | CUVS_COVER_FLAG_INSERTED_SINCE_START)

/* The descriptor. Offsets are fixed by the static assertions below. */
typedef struct CuvsCoverDescriptor {
	uint32_t magic;				/*   0 */
	uint16_t format_version;		/*   4 */
	uint16_t min_reader_version;		/*   6 */
	uint16_t d_len;				/*   8  (184 in version 1; the crc is the last 4 bytes) */
	uint8_t state;				/*  10 */
	uint8_t reason;				/*  11 */
	uint16_t flags;				/*  12 */
	uint16_t reserved0;			/*  14 */
	uint64_t generation;			/*  16 */
	uint32_t ledger_head;			/*  24 */
	uint32_t ledger_tail;			/*  28 */
	uint32_t free_head;			/*  32 */
	uint32_t graph_nblocks_at_cert;		/*  36 */
	uint64_t base_rows;			/*  40 */
	uint64_t base_missing;			/*  48 */
	uint64_t delta_count;			/*  56 */
	uint64_t recert_generation;		/*  64 */
	uint64_t cert_lsn;			/*  72 */
	uint64_t graph_epoch;			/*  80 */
	int64_t cert_time;			/*  88  TimestampTz (microseconds) at which the last certification began (a recertification: the instant it held the cover lock exclusively and no insert was in flight); 0 if never */
	uint32_t last_fail_nblocks;		/*  96  relation size of the last recertification that gave up; 0 if none */
	uint32_t last_fail_status;		/* 100  its CuvsReachStatus */
	uint64_t last_fail_limit;		/* 104  RAM + spill limit it had */
	uint64_t last_fail_needed;		/* 112  what it estimated it needed */
	int64_t cert_published_time;		/* 120  TimestampTz the recertification was published; 0 if the certification was not made by one (a build, or this field did not exist: a library that
						 *      publishes a certification writes the time, one that does not know the field writes zeros) */
	uint64_t cert_dropped_keys;		/* 128  keys written after the certification began (during the walk of the recertification that made it) whose row the walk listed itself:
						 *      the key was not carried over (the row is a BASE key), but the insert is still an insert since the certification and counts as one; 0 for a build */
	uint32_t pending_head;			/* 136  first block of the new ledger chain of a recertification that has written its pages and not published them (0 if none): the pages
						 *      are in no other chain, and the next recertification takes them back (an abandoned or cancelled one leaves them here) */
	uint32_t reserved2;			/* 140 */
	uint64_t pending_generation;		/* 144  the generation of the pages of that chain */
	uint8_t reserved3[28];			/* 152  everything in 120..179 that a version does not name belongs to one certification: the publication writes zeros there, every other rewrite keeps the bytes */
	uint32_t crc32c;			/* 180 */
} CuvsCoverDescriptor;

_Static_assert(sizeof(CuvsCoverDescriptor) == CUVS_COVER_D_SIZE, "descriptor size");
_Static_assert(offsetof(CuvsCoverDescriptor, magic) == 0, "magic offset");
_Static_assert(offsetof(CuvsCoverDescriptor, format_version) == 4, "format_version offset");
_Static_assert(offsetof(CuvsCoverDescriptor, min_reader_version) == 6, "min_reader_version offset");
_Static_assert(offsetof(CuvsCoverDescriptor, d_len) == 8, "d_len offset");
_Static_assert(offsetof(CuvsCoverDescriptor, state) == 10, "state offset");
_Static_assert(offsetof(CuvsCoverDescriptor, reason) == 11, "reason offset");
_Static_assert(offsetof(CuvsCoverDescriptor, flags) == 12, "flags offset");
_Static_assert(offsetof(CuvsCoverDescriptor, generation) == 16, "generation offset");
_Static_assert(offsetof(CuvsCoverDescriptor, ledger_head) == 24, "ledger_head offset");
_Static_assert(offsetof(CuvsCoverDescriptor, ledger_tail) == 28, "ledger_tail offset");
_Static_assert(offsetof(CuvsCoverDescriptor, free_head) == 32, "free_head offset");
_Static_assert(offsetof(CuvsCoverDescriptor, graph_nblocks_at_cert) == 36, "graph_nblocks_at_cert offset");
_Static_assert(offsetof(CuvsCoverDescriptor, base_rows) == 40, "base_rows offset");
_Static_assert(offsetof(CuvsCoverDescriptor, base_missing) == 48, "base_missing offset");
_Static_assert(offsetof(CuvsCoverDescriptor, delta_count) == 56, "delta_count offset");
_Static_assert(offsetof(CuvsCoverDescriptor, recert_generation) == 64, "recert_generation offset");
_Static_assert(offsetof(CuvsCoverDescriptor, cert_lsn) == 72, "cert_lsn offset");
_Static_assert(offsetof(CuvsCoverDescriptor, graph_epoch) == 80, "graph_epoch offset");
_Static_assert(offsetof(CuvsCoverDescriptor, cert_time) == 88, "cert_time offset");
_Static_assert(offsetof(CuvsCoverDescriptor, last_fail_nblocks) == 96, "last_fail_nblocks offset");
_Static_assert(offsetof(CuvsCoverDescriptor, last_fail_status) == 100, "last_fail_status offset");
_Static_assert(offsetof(CuvsCoverDescriptor, last_fail_limit) == 104, "last_fail_limit offset");
_Static_assert(offsetof(CuvsCoverDescriptor, last_fail_needed) == 112, "last_fail_needed offset");
_Static_assert(offsetof(CuvsCoverDescriptor, cert_published_time) == 120, "cert_published_time offset");
_Static_assert(offsetof(CuvsCoverDescriptor, cert_dropped_keys) == 128, "cert_dropped_keys offset");
_Static_assert(offsetof(CuvsCoverDescriptor, pending_head) == 136, "pending_head offset");
_Static_assert(offsetof(CuvsCoverDescriptor, pending_generation) == 144, "pending_generation offset");
_Static_assert(offsetof(CuvsCoverDescriptor, reserved3) == 152, "reserved3 offset");
_Static_assert(offsetof(CuvsCoverDescriptor, crc32c) == 180, "crc32c offset (last 4 bytes)");

/* The keys of the ledger that are not keys written since the certification: the total of a chain minus this is the number of inserts since the
 * certification. The BASE keys, except the ones a carried-over key was dropped for (cert_dropped_keys of them are inserts since the certification). */
static inline uint64_t
cuvs_cover_base_keys(const CuvsCoverDescriptor *d)
{
	return d->base_missing > d->cert_dropped_keys ? d->base_missing - d->cert_dropped_keys : 0;
}
/* pd_upper of a new index is 8192 - 8 - 64 - 184 = 7936. Its Hamming distance to the two old shapes
 * (8184 and 8120) is 5 and 4, so one to three flipped bits cannot make a new index look old. The unit
 * test checks the distance by brute force; 128 bytes would have given a distance of 1. */
_Static_assert(CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE - CUVS_COVER_CONN_SIZE - CUVS_COVER_D_SIZE == 7936,
	"new pd_upper");

/* CRC-32C (Castagnoli, reflected 0x82F63B78), the value written at the end of the descriptor. One table lookup per byte: the descriptor is
 * read and checked by every scan of an index, and the bit-at-a-time loop this replaces took about 2 microseconds of a 450 microsecond
 * search (perf of a certified 1,000-row search, symbol cuvs_cover_classify). The check value of "123456789" is in test_cover_format.c. */
static inline uint32_t
cuvs_cover_crc32c(const void *data, size_t len)
{
	static uint32_t table[256];
	static int	ready = 0;
	const uint8_t *p = (const uint8_t *) data;
	uint32_t crc = 0xFFFFFFFFu;
	size_t i;

	if (!ready) {
		uint32_t n;
		int k;

		for (n = 0; n < 256; n++) {
			uint32_t c = n;

			for (k = 0; k < 8; k++)
				c = (c >> 1) ^ (0x82F63B78u & (uint32_t) -(int32_t) (c & 1u));
			table[n] = c;
		}
		ready = 1;
	}
	for (i = 0; i < len; i++)
		crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFFu];
	return ~crc;
}

/* Set magic-independent fields that the crc covers: crc over bytes [0, d_len - 4). */
static inline void
cuvs_cover_descriptor_seal(CuvsCoverDescriptor *d)
{
	d->crc32c = cuvs_cover_crc32c(d, CUVS_COVER_D_SIZE - 4u);
}

static inline uint16_t
cuvs_cover_get16(const uint8_t *p, size_t off)
{
	uint16_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

static inline uint32_t
cuvs_cover_get32(const uint8_t *p, size_t off)
{
	uint32_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

static inline void
cuvs_cover_put16(uint8_t *p, size_t off, uint16_t v)
{
	memcpy(p + off, &v, sizeof(v));
}

/* The one place that writes the metapage tail. The descriptor and the connectivity record are placed
 * above the hole and pd_upper is set to the descriptor start. `conn` is the 64-byte connectivity record
 * or NULL (zero bytes: older readers report `not_recorded`). Returns 0, or -1 if the page cannot hold
 * the tail (wrong pd_special, or pd_lower reaches into it). */
static inline int
cuvs_cover_stage_tail(uint8_t *page, const uint8_t *conn, const CuvsCoverDescriptor *d)
{
	const size_t special = CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE;
	const size_t d_at = special - CUVS_COVER_CONN_SIZE - CUVS_COVER_D_SIZE;

	if (cuvs_cover_get16(page, CUVS_COVER_OFF_SPECIAL) != special ||
	    cuvs_cover_get16(page, CUVS_COVER_OFF_LOWER) < CUVS_COVER_PGV_META_END ||
	    cuvs_cover_get16(page, CUVS_COVER_OFF_LOWER) > d_at ||
	    d->d_len != CUVS_COVER_D_SIZE)
		return -1;
	if (conn != NULL)
		memcpy(page + special - CUVS_COVER_CONN_SIZE, conn, CUVS_COVER_CONN_SIZE);
	else
		memset(page + special - CUVS_COVER_CONN_SIZE, 0, CUVS_COVER_CONN_SIZE);
	memcpy(page + d_at, d, CUVS_COVER_D_SIZE);
	cuvs_cover_put16(page, CUVS_COVER_OFF_UPPER, (uint16_t) d_at);
	return 0;
}

/* Block 1 (the first element page) gets a second, independent witness in the `unused` field of its
 * special area. pgvector 0.8.7 never reads or writes that field. */
static inline void
cuvs_cover_stage_head_witness(uint8_t *head_page)
{
	cuvs_cover_put16(head_page, CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE + 4u, CUVS_COVER_HEAD_WITNESS);
}

typedef enum {
	CUVS_COVER_ERROR = 0,		/* the page cannot be classified: report an error, never LEGACY */
	CUVS_COVER_LEGACY,		/* an old index: no guarantee was ever promised, answer as before */
	CUVS_COVER_OFF,			/* built with the guarantee switched off or for an unsupported shape */
	CUVS_COVER_CERTIFIED,		/* the ledger must still be verified (needs_ledger_check) */
	CUVS_COVER_LOST,		/* had a guarantee and lost it */
	CUVS_COVER_UNTRUSTED		/* should have a guarantee but cannot be read: treated like LOST */
} CuvsCoverClass;

typedef enum {
	CUVS_COVER_UNTRUSTED_NONE = 0,
	CUVS_COVER_UNTRUSTED_LAYOUT,
	CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT,
	CUVS_COVER_UNTRUSTED_DESCRIPTOR_LOST,
	CUVS_COVER_UNTRUSTED_FUTURE_FORMAT,
	CUVS_COVER_UNTRUSTED_HEAD_UNREADABLE,
	CUVS_COVER_UNTRUSTED_DESCRIPTOR_INCONSISTENT	/* a valid crc over a combination the design never writes */
} CuvsCoverUntrustedReason;

typedef struct CuvsCoverResult {
	CuvsCoverClass cls;
	CuvsCoverUntrustedReason reason;	/* for UNTRUSTED */
	uint8_t stored_reason;			/* descriptor.reason for OFF and LOST */
	int needs_ledger_check;			/* CERTIFIED: verify ledger_head and generation next */
	/* 1 only for a readable OFF, CERTIFIED or LOST descriptor whose format_version is not newer than this
	 * library's. A newer format that min_reader_version still lets us read is classified normally but is
	 * "read, do not write": the caller must not add ledger keys or change the state. LEGACY,
	 * UNTRUSTED and ERROR are never writable. */
	int writable;
	/* the ledger chain the descriptor names, for CERTIFIED and LOST (a LOST index that never had a chain carries
	 * CUVS_COVER_INVALID_BLOCK and generation 0) */
	uint32_t ledger_head;
	uint32_t ledger_tail;
	uint64_t generation;
	uint16_t flags;					/* descriptor.flags, for CERTIFIED and LOST */
} CuvsCoverResult;

static inline void
cuvs_cover_set_untrusted(CuvsCoverResult *out, CuvsCoverUntrustedReason why)
{
	out->cls = CUVS_COVER_UNTRUSTED;
	out->reason = why;
	out->writable = 0;
}

/* Classify an index from its metapage and its first element page (block 1; NULL if unreadable). The
 * order of the checks is the design's rule list: sanity (errors), old shapes, descriptor.
 *
 * `meta` and `head` must each point at a complete CUVS_COVER_PAGE_SIZE (8192) byte image, as
 * BufferGetPage returns. The function takes no length, so a shorter buffer cannot be expressed and is
 * not checked; it never reads at or beyond offset 8192 of either page, whatever the page contains
 * (the unit test seals descriptors past the end of the image to prove it). */
static inline void
cuvs_cover_classify(const uint8_t *meta, const uint8_t *head, CuvsCoverResult *out)
{
	const uint32_t special = CUVS_COVER_PAGE_SIZE - CUVS_COVER_SPECIAL_SIZE;
	uint32_t lower, upper, d_at;
	CuvsCoverDescriptor d;
	uint32_t stored_crc, want_crc;
	uint16_t d_len;

	memset(out, 0, sizeof(*out));
	out->cls = CUVS_COVER_ERROR;
	if (meta == NULL)
		return;
	/* sanity first: nothing below may run on a page whose header is not the one pgvector writes */
	if (cuvs_cover_get16(meta, CUVS_COVER_OFF_SPECIAL) != special)
		return;
	lower = cuvs_cover_get16(meta, CUVS_COVER_OFF_LOWER);
	upper = cuvs_cover_get16(meta, CUVS_COVER_OFF_UPPER);
	if (lower < CUVS_COVER_PGV_META_END || lower > upper || upper > special)
		return;
	if (cuvs_cover_get32(meta, CUVS_COVER_OFF_PGV_MAGIC) != CUVS_COVER_PGV_MAGIC)
		return;

	/* old shapes: no tail, or only the connectivity record */
	if (upper == special || upper == special - CUVS_COVER_CONN_SIZE) {
		if (head == NULL || cuvs_cover_get16(head, CUVS_COVER_OFF_SPECIAL) != special) {
			cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_HEAD_UNREADABLE);
			return;
		}
		if (cuvs_cover_get16(head, special + 4u) == CUVS_COVER_HEAD_WITNESS) {
			/* a new index whose descriptor shape was damaged: the second witness is still set */
			cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_LOST);
			return;
		}
		out->cls = CUVS_COVER_LEGACY;
		return;
	}

	/* a descriptor must start at pd_upper and end right below the connectivity record */
	d_at = upper;
	if (d_at + 16u > special - CUVS_COVER_CONN_SIZE) {
		cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_LAYOUT);
		return;
	}
	if (cuvs_cover_get32(meta, d_at) != CUVS_COVER_D_MAGIC) {
		cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT);
		return;
	}
	d_len = cuvs_cover_get16(meta, d_at + 8u);
	if (d_len < CUVS_COVER_D_SIZE || (d_len & 7u) != 0 || d_at + d_len != special - CUVS_COVER_CONN_SIZE) {
		cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_LAYOUT);
		return;
	}
	stored_crc = cuvs_cover_get32(meta, d_at + d_len - 4u);
	want_crc = cuvs_cover_crc32c(meta + d_at, d_len - 4u);
	if (stored_crc != want_crc) {
		cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT);
		return;
	}
	memcpy(&d, meta + d_at, 180u);	/* version-1 fields; later versions only append inside d_len */
	d.crc32c = stored_crc;
	if (d.format_version == 0) {
		cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT);
		return;
	}
	if (d.min_reader_version > CUVS_COVER_FORMAT_VERSION) {
		cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_FUTURE_FORMAT);
		return;
	}
	out->stored_reason = d.reason;
	out->writable = d.format_version <= CUVS_COVER_FORMAT_VERSION;
	switch (d.state) {
	case CUVS_COVER_STATE_OFF:
		out->cls = CUVS_COVER_OFF;
		return;
	case CUVS_COVER_STATE_LOST:
		out->cls = CUVS_COVER_LOST;
		out->ledger_head = d.ledger_head;
		out->ledger_tail = d.ledger_tail;
		out->generation = d.generation;
		out->flags = d.flags;
		return;
	case CUVS_COVER_STATE_CERTIFIED:
		if (d.generation == 0 || d.ledger_head == CUVS_COVER_INVALID_BLOCK) {
			cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT);
			return;
		}
		/* a CERTIFIED descriptor never carries the suspect-writer flag: this library sets that flag only when it moves
		 * the descriptor to LOST, so the combination is inconsistent even if the crc is right */
		if ((d.flags & CUVS_COVER_FLAG_SUSPECT_UNKEYED_WRITER) != 0) {
			cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_INCONSISTENT);
			return;
		}
		out->cls = CUVS_COVER_CERTIFIED;
		out->needs_ledger_check = 1;
		out->ledger_head = d.ledger_head;
		out->ledger_tail = d.ledger_tail;
		out->generation = d.generation;
		out->flags = d.flags;
		return;
	default:
		cuvs_cover_set_untrusted(out, CUVS_COVER_UNTRUSTED_DESCRIPTOR_CORRUPT);
		return;
	}
}

#endif
