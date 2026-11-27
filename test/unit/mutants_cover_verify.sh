#!/bin/bash
# Mutants of the insert verification (cuvs.cover_verify_inserts, src/cuvs_cover_verify.c and cuvs_cover_insert.c). Each mutant is a copy of the
# tree with one edit that breaks one thing the verification must do; the scan suite (test_cover_scan_pg.py) and the insert suite
# (test_cover_insert_pg.py) must turn at least one check red for every mutant. A mutant that no check catches is a missing test.
#
#   F  the search says "found" for every row            -> S17 cut-off rows "the search does not find E"
#   G  the search says "not found" for every row        -> S17 key counts, "the search finds a row the graph reaches"
#   H  the search runs in the caller's memory context        -> S17 "2,000 searches in a row leave the memory ... within 64 KB"
#      (no INSERT ... SELECT shows it: the executor resets the context of an insert per row; a C caller that does not reset one would)
#   K  the search runs without the check for an active snapshot -> S17 "without an active snapshot ... the backend lives" (the backend dies)
#   I  a verified insert records no event for the growth monitor
#                                                       -> S17 high-dimensional growth (a false suspect after a key)
#   J  a verified insert records the size but never judges the growth
#                                                       -> I10 (an unkeyed writer is not noticed by a verified insert)
#
# usage: mutants_cover_verify.sh <gate tree dir (a built copy of the tree, CPU shim)> <old library .so> <result file>
# Needs a PostgreSQL 16 with pgvector and the tools the scan suite needs, and about 6 GB of memory for the five parallel scan runs.
set -u
GATE=${1:?built tree}; OLDLIB=${2:?old pg_cuvs.so}; RESULT=${3:?result file}
export PATH=/usr/lib/postgresql/16/bin:$PATH
W=$(mktemp -d)
mk() { # name file old new
  n=$1; G=$W/mut-$n; mkdir -p $G
  (cd $GATE && tar cf - --exclude="*.o" --exclude="*.so" --exclude="*.bc" .) | (cd $G && tar xf -)
  (cd $G && python3 - "$2" "$3" "$4" <<'PY'
import sys
f, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(f).read()
assert s.count(a) == 1, (f, a, s.count(a))
open(f, 'w').write(s.replace(a, b))
PY
  make -j6 PGCUVS_CPU_SHIM=1 > build.log 2>&1; echo "$n build=$?")
}
mk F src/cuvs_cover_verify.c "		if (ItemPointerEquals(&scan->xs_heaptid, heap_tid))" "		if (1)"
mk G src/cuvs_cover_verify.c "		if (ItemPointerEquals(&scan->xs_heaptid, heap_tid))" "		if (0)"
mk H src/cuvs_cover_verify.c "	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return found;" "	MemoryContextSwitchTo(old);
	return found;"
mk K src/cuvs_cover_verify.c "isnull[0] || d->ambeginscan == NULL || !ActiveSnapshotSet()" "isnull[0] || d->ambeginscan == NULL"
mk I src/cuvs_cover_insert.c "			cover_note_verified(index, tail, generation);" "			(void) 0;"
mk J src/cuvs_cover_insert.c "			suspect = h2.graph_nblocks_seen != 0 && nblocks - h2.graph_nblocks_seen > cover_growth_pages();" "			suspect = false;"
cd "$GATE"
p=5900
for n in F G H K I J; do
  p=$((p+2))
  ( R=$W/scan-$n; python3 test/unit/test_cover_scan_pg.py --output $R --new-lib $W/mut-$n/pg_cuvs.so --old-lib $OLDLIB --port $p > $W/scan-$n.log 2>&1
    echo "$n scan exit=$? fails=$(grep -c '^FAIL' $R/transcript.txt 2>/dev/null)" >> $RESULT
    grep '^FAIL' $R/transcript.txt 2>/dev/null | cut -c1-170 | head -6 >> $RESULT
    R=$W/ins-$n; python3 test/unit/test_cover_insert_pg.py --output $R --new-lib $W/mut-$n/pg_cuvs.so --old-lib $OLDLIB --port $((p+100)) > $W/ins-$n.log 2>&1
    echo "$n insert exit=$? fails=$(grep -c '^FAIL' $R/transcript.txt 2>/dev/null)" >> $RESULT
    grep '^FAIL' $R/transcript.txt 2>/dev/null | cut -c1-170 | head -6 >> $RESULT ) &
done
wait
echo done >> $RESULT
