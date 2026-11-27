#ifndef CUVS_COVER_SCAN_H
#define CUVS_COVER_SCAN_H
#include "postgres.h"
#include "access/itup.h"
#include "utils/rel.h"
#include "cuvs_cover_format.h"

/* Classify a pg_cuvs_hnsw index for a scan: reads block 0 (and block 1 when the shape is an old one). */
extern void cuvs_cover_scan_classify(Relation index, CuvsCoverResult *res);

/*
 * Whether the break-glass marker exists: the function pg_cuvs_cover_breakglass_marker(), made by pg_cuvs_cover_breakglass()
 * when the access method handler was pointed back at the older C symbol and removed only by the extension update script. While
 * it exists every CERTIFIED index reads as LOST (reason breakglass): an older library may have inserted rows or vacuumed
 * without recording anything, and nothing in the index says so. Catalog cache lookups only; the answer follows the catalog.
 */
extern bool cuvs_cover_breakglass_marked(void);

/*
 * Read the whole ledger chain of a CERTIFIED or LOST index as the descriptor `res` describes it: every page valid and of
 * its generation, every key's crc, the ordinals continuous, the chain passing the tail the descriptor names. *nkeys is the
 * number of keys when true; *why says what was wrong when false.
 */
extern bool cuvs_cover_ledger_check(Relation index, const CuvsCoverResult *res, uint64 *nkeys, const char **why);
extern bool cuvs_cover_ledger_stale(const char *why);

/*
 * True when the ledger of the CERTIFIED index described by `res` holds no key at all (the last certification found nothing the
 * graph cannot reach and nothing was inserted since): the merge then adds nothing to the graph's own answers, and the scan can
 * be the unwrapped one. Read after the scan's snapshot was taken, so a row that snapshot can see has its key in the chain
 * (the key is written before the row is inserted into the graph, and before the inserting transaction commits).
 */
extern bool cuvs_cover_ledger_is_empty(Relation index, const CuvsCoverResult *res);

/*
 * Every heap TID the ledger of a CERTIFIED index holds (the base keys of the last certification and every key written
 * since), in `cxt`. Retries when a recertification replaces the chain under the reader. Returns false if the chain cannot
 * be read consistently (a page that is not what the descriptor says, a key whose crc does not match, a chain that ends
 * before `tail`, the tail the descriptor named): the caller must then treat the index like an untrusted one and rank the
 * whole heap.
 */
extern bool cuvs_cover_scan_ledger(Relation index, uint32 head, uint32 tail, uint64 generation, MemoryContext cxt,
				   ItemPointerData **tids, Size *ntids);

#endif
