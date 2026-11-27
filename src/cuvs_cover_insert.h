#ifndef CUVS_COVER_INSERT_H
#define CUVS_COVER_INSERT_H
#include "postgres.h"
#include "access/amapi.h"
#include "cuvs_cover_ledger.h"

/* Wrap aminsert of a cover index routine: write the heap TID into the ledger before pgvector inserts it. */
extern void cuvs_cover_wrap_insert(IndexAmRoutine *amroutine);

/* Append a ledger key (its ordinal is assigned) to the chain that ends at `tail`; see the definition. */
extern void cuvs_cover_append_key(Relation index, uint32 tail, uint64 generation, bool certified, const CuvsLedgerKey *proto,
								  bool update_meta, uint32 *tail_out, uint64 *ordinal_out);

/* The database-object lock that orders inserts against a recertification (LockDatabaseObject subid). The subid is a
 * uint16 (the design's 0x434F5645 did not fit); 0x5645 = 22085 is above MaxHeapAttributeNumber (1600), which
 * PostgreSQL uses for column numbers in the same lock tag. */
#define CUVS_COVER_LOCKSUBID 0x5645

/* Pages the relation may grow between two ledger keys before the growth is taken as rows inserted without keys. A normal
 * insert adds at most one graph page per key, so even eight sessions stay far below this. */
#define CUVS_COVER_GROWTH_PAGES 256

#endif
