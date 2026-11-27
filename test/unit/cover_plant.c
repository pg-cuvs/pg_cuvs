/* Test helper for test_downgrade_pg.py: stamp a cover descriptor onto the metapage (and the block 1
 * witness) of a pg_cuvs_hnsw index FILE, the way the format work will write it. The server must be
 * stopped. The product writes no descriptor yet, so this is the only way to give the downgrade
 * script an index that carries a guarantee mark.
 *
 *   cover_plant FILE certified|lost|uncertified|suspect|newer|future|off|corrupt
 *
 * Built against the real src/cuvs_cover_format.h, so the layout cannot drift from what the
 * classifier reads. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cuvs_cover_format.h"

static int
read_page(FILE *f, long blk, uint8_t *page)
{
	return fseek(f, blk * (long) CUVS_COVER_PAGE_SIZE, SEEK_SET) == 0 && fread(page, 1, CUVS_COVER_PAGE_SIZE, f) == CUVS_COVER_PAGE_SIZE;
}

static int
write_page(FILE *f, long blk, const uint8_t *page)
{
	return fseek(f, blk * (long) CUVS_COVER_PAGE_SIZE, SEEK_SET) == 0 && fwrite(page, 1, CUVS_COVER_PAGE_SIZE, f) == CUVS_COVER_PAGE_SIZE && fflush(f) == 0;
}

int
main(int argc, char **argv)
{
	uint8_t meta[CUVS_COVER_PAGE_SIZE], head[CUVS_COVER_PAGE_SIZE];
	FILE *f;
	CuvsCoverDescriptor d;
	CuvsCoverResult r;

	if (argc != 3) {
		fprintf(stderr, "usage: cover_plant FILE certified|lost|uncertified|suspect|newer|future|off|corrupt\n");
		return 2;
	}
	f = fopen(argv[1], "r+b");
	if (f == NULL || !read_page(f, 0, meta) || !read_page(f, 1, head)) {
		fprintf(stderr, "cannot read blocks 0 and 1 of %s\n", argv[1]);
		return 2;
	}
	cuvs_cover_classify(meta, head, &r);
	if (r.cls != CUVS_COVER_LEGACY) {
		fprintf(stderr, "block 0 of %s is not a LEGACY metapage (class %d)\n", argv[1], (int) r.cls);
		return 2;
	}
	memset(&d, 0, sizeof(d));
	d.magic = CUVS_COVER_D_MAGIC;
	d.format_version = CUVS_COVER_FORMAT_VERSION;
	d.min_reader_version = 1;
	d.d_len = CUVS_COVER_D_SIZE;
	d.flags = CUVS_COVER_FLAG_HEAD_WITNESS;
	d.generation = 1;
	d.ledger_head = 2;
	d.ledger_tail = 2;
	d.free_head = CUVS_COVER_INVALID_BLOCK;
	if (strcmp(argv[2], "certified") == 0 || strcmp(argv[2], "corrupt") == 0)
		d.state = CUVS_COVER_STATE_CERTIFIED;
	else if (strcmp(argv[2], "uncertified") == 0) {	/* what a row-built index starts as: LOST, no chain */
		d.state = CUVS_COVER_STATE_LOST;
		d.reason = CUVS_COVER_REASON_UNCERTIFIED;
		d.generation = 0;
		d.ledger_head = d.ledger_tail = CUVS_COVER_INVALID_BLOCK;
	} else if (strcmp(argv[2], "suspect") == 0) {	/* LOST by the growth monitor: the persistent flag is set, no chain */
		d.state = CUVS_COVER_STATE_LOST;
		d.reason = CUVS_COVER_REASON_UNKEYED_WRITER;
		d.flags |= CUVS_COVER_FLAG_SUSPECT_UNKEYED_WRITER;
		d.generation = 0;
		d.ledger_head = d.ledger_tail = CUVS_COVER_INVALID_BLOCK;
		d.graph_epoch = 3;
	} else if (strcmp(argv[2], "newer") == 0) {	/* CERTIFIED by a newer format this library may read but not write */
		d.state = CUVS_COVER_STATE_CERTIFIED;
		d.format_version = CUVS_COVER_FORMAT_VERSION + 1;
	} else if (strcmp(argv[2], "future") == 0) {	/* a format this library cannot read at all: min_reader_version above ours */
		d.state = CUVS_COVER_STATE_CERTIFIED;
		d.format_version = CUVS_COVER_FORMAT_VERSION + 2;
		d.min_reader_version = CUVS_COVER_FORMAT_VERSION + 1;
	} else if (strcmp(argv[2], "lost") == 0) {
		d.state = CUVS_COVER_STATE_LOST;
		d.reason = CUVS_COVER_REASON_VACUUM;
	} else if (strcmp(argv[2], "off") == 0) {
		d.state = CUVS_COVER_STATE_OFF;
		d.reason = CUVS_COVER_REASON_USER;
	} else {
		fprintf(stderr, "unknown mode %s\n", argv[2]);
		return 2;
	}
	cuvs_cover_descriptor_seal(&d);
	if (strcmp(argv[2], "corrupt") == 0)
		d.base_rows ^= 1;	/* the crc no longer matches */
	if (cuvs_cover_stage_tail(meta, NULL, &d) != 0) {
		fprintf(stderr, "the metapage cannot hold a descriptor\n");
		return 2;
	}
	cuvs_cover_stage_head_witness(head);
	cuvs_cover_classify(meta, head, &r);
	printf("planted %s: the classifier now reads class %d\n", argv[2], (int) r.cls);
	return write_page(f, 0, meta) && write_page(f, 1, head) ? 0 : 2;
}
