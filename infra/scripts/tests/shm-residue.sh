#!/bin/bash
# #165 regression guard: daemon-created /dev/shm segments must not outlive the
# request that produced them.
#
# The daemon and the PG backend run as different uids and /dev/shm is sticky, so
# a name the daemon creates cannot be removed by the backend. Every such segment
# is therefore handed over as an SCM_RIGHTS fd with the name already unlinked.
# This asserts that invariant for all three producers:
#
#   pg_cuvs_adj_*   export_adjacency   (nsw/hnsw export, per build)
#   pg_cuvs_hnsw_*  export_hnsw_shm    (hnswlib export, per build)
#   pg_cuvs_bsr_*   batch search reply (per query)
#
# ADR-057's leak-verify.sh watches pg_cuvs_bld_* only — the backend-owned prefix,
# which was never affected. That blind spot is why #165 survived a full suite.
#
# Run on the GPU VM. Needs a table `t` with a CAGRA index `t_cagra`.
set -u
PSQL="psql -d ${PGDATABASE:-shadeform} -qtA"
PASS=0; FAIL=0
ok(){  PASS=$((PASS+1)); echo "  [PASS] $1"; }
bad(){ FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

residue(){ ls /dev/shm 2>/dev/null | grep -c "^pg_cuvs_${1}_" || true; }
report(){ ls /dev/shm 2>/dev/null | grep "^pg_cuvs_" | head -5; }

echo "== #165 /dev/shm residue =="
for p in adj hnsw bsr; do
  n=$(residue "$p")
  [ "$n" = "0" ] || { echo "  pre-existing pg_cuvs_${p}_ residue ($n) — clearing"; }
done

# --- exports: both modes, repeated ------------------------------------------
for mode in nsw hnswlib nsw hnswlib; do
  $PSQL -c "SET cuvs.index_dir='${CUVS_INDEX_DIR:-/tmp/cuvs_indexes}';
            SET maintenance_work_mem='8GB';
            DROP INDEX IF EXISTS t_hnsw_3i;
            CREATE INDEX t_hnsw_3i ON t USING pg_cuvs_hnsw (embedding vector_l2_ops)
                   WITH (source='t_cagra', mode='${mode}');" >/dev/null 2>&1 \
    || { bad "export mode=${mode} failed"; continue; }
done
a=$(residue adj); h=$(residue hnsw)
[ "$a" = "0" ] && ok "no pg_cuvs_adj_ residue after 4 exports" \
               || bad "pg_cuvs_adj_ residue=$a after 4 exports"
[ "$h" = "0" ] && ok "no pg_cuvs_hnsw_ residue after 4 exports" \
               || bad "pg_cuvs_hnsw_ residue=$h after 4 exports"

# --- batch search: the per-query producer ------------------------------------
$PSQL -c "SET cuvs.index_dir='${CUVS_INDEX_DIR:-/tmp/cuvs_indexes}';" >/dev/null 2>&1
for i in $(seq 1 20); do
  $PSQL -c "SELECT id FROM t ORDER BY embedding <-> (SELECT embedding FROM t WHERE id=$i)
            LIMIT 10;" >/dev/null 2>&1
done
b=$(residue bsr)
[ "$b" = "0" ] && ok "no pg_cuvs_bsr_ residue after 20 searches" \
               || bad "pg_cuvs_bsr_ residue=$b after 20 searches"

[ "$FAIL" -gt 0 ] && { echo "  residue sample:"; report; }
echo "== $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
