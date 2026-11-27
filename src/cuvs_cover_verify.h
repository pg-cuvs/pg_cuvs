#ifndef CUVS_COVER_VERIFY_H
#define CUVS_COVER_VERIFY_H
/* Is a row that was just inserted into a pgvector hnsw index reachable from the entry point?
 *
 * Reached from the insert wrapper (cuvs_cover_insert.c) after pgvector has inserted the element. The row is searched for with its own
 * vector, through pgvector's own scan routines (not through the cover scan, which would merge the ledger): the search starts at the entry
 * point and moves along neighbor edges only, so a row it returns was reachable through the real search path when it ran. Nothing is trusted
 * that was certified earlier. The answer "not found" does not say the row is unreachable (the search is approximate); the caller then
 * writes the ledger key as it always did. */
#include "postgres.h"
#include "access/itup.h"
#include "storage/itemptr.h"
#include "utils/rel.h"

/* `values`/`isnull`: the index key of the inserted row as aminsert received it (the vector may be a toasted datum of another table).
 * `heap_tid`: the heap TID of the row. Returns false when no search can be run (no active snapshot, null key). */
extern bool cuvs_cover_insert_searchable(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid);

/* Experimental: verify inserts of a certified index and skip the ledger key of a row the search finds (see cuvs_cover_insert.c). */
extern bool cuvs_cover_verify_inserts;

#endif
