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
#   pg_cuvs_bsr_*   batch search reply (per batch-search call)
#
# ADR-057's leak-verify.sh watches pg_cuvs_bld_* only — the backend-owned prefix,
# which was never affected. That blind spot is why #165 survived a full suite.
#
# CUVS_OP_SEARCH_BATCH is reached ONLY through pg_cuvs_batch_search()
# (src/pg_cuvs.c, the sole caller of cuvs_ipc_search_batch). An ordinary
# `ORDER BY embedding <-> ...` scan uses CUVS_OP_SEARCH and never allocates a
# pg_cuvs_bsr_* segment — asserting residue after plain scans passes vacuously
# (#166 review P1). The batch arm below therefore calls the SRF and proves it
# actually ran by requiring rows back.
#
# #176: this used to assume `t`/`t_cagra` already existed — an implicit shared
# fixture nothing else in the repo referenced (grep confirms this script was
# the only consumer). On a freshly provisioned VM that ambient state was never
# there, and the script failed with "relation \"t\" does not exist", a
# misleading error unrelated to the /dev/shm invariant it exists to check. It
# now builds its own small fixture below, same pattern as
# umask-hardened-export.sh's private table.
#
# Run on the GPU VM.
set -u
IDIR="${CUVS_INDEX_DIR:-/tmp/cuvs_indexes}"
PSQL="psql -d ${PGDATABASE:-shadeform} -qtA -v ON_ERROR_STOP=1"
PASS=0; FAIL=0
ok(){  PASS=$((PASS+1)); echo "  [PASS] $1"; }
bad(){ FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

residue(){ ls /dev/shm 2>/dev/null | grep -c "^pg_cuvs_${1}_"; }

echo "== #165 /dev/shm residue =="

pre=$(ls /dev/shm 2>/dev/null | grep -c '^pg_cuvs_')
if [ "$pre" != "0" ]; then
  echo "  [SKIP-GUARD] $pre pre-existing pg_cuvs_* segment(s); clear them first" >&2
  ls /dev/shm | grep '^pg_cuvs_' | head -5 >&2
  exit 1
fi

# 100 rows clears the batch loop's id range (up to 80, below) with margin;
# dim=16 keeps CAGRA build fast. Dropped and rebuilt fresh every run so the
# fixture never drifts from what this script expects.
if ! $PSQL <<SQL >/dev/null 2>/tmp/shm_residue_setup.txt
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;
SET cuvs.index_dir='${IDIR}';
DROP TABLE IF EXISTS t CASCADE;
CREATE TABLE t (id bigint, embedding vector(16));
INSERT INTO t
    SELECT g, ('[' || array_to_string(ARRAY(
        SELECT (random())::numeric(6,4) FROM generate_series(1,16)), ',') || ']')::vector
    FROM generate_series(1, 100) g;
CREATE INDEX t_cagra ON t USING cagra (embedding vector_l2_ops);
SQL
then
  bad "fixture setup: $(grep -m1 '^ERROR' /tmp/shm_residue_setup.txt 2>/dev/null || head -1 /tmp/shm_residue_setup.txt)"
  echo "== $PASS passed, $FAIL failed =="
  exit 1
fi

# --- exports: both modes, repeated ------------------------------------------
for mode in nsw hnswlib nsw hnswlib; do
  if ! $PSQL -c "SET cuvs.index_dir='${IDIR}';
                 SET maintenance_work_mem='8GB';
                 DROP INDEX IF EXISTS t_hnsw_3i;
                 CREATE INDEX t_hnsw_3i ON t USING pg_cuvs_hnsw (embedding vector_l2_ops)
                        WITH (source='t_cagra', mode='${mode}');" >/dev/null; then
    bad "export mode=${mode} failed"
  fi
done
a=$(residue adj); h=$(residue hnsw)
[ "$a" = "0" ] && ok "no pg_cuvs_adj_ residue after 4 exports" \
               || bad "pg_cuvs_adj_ residue=$a after 4 exports"
[ "$h" = "0" ] && ok "no pg_cuvs_hnsw_ residue after 4 exports" \
               || bad "pg_cuvs_hnsw_ residue=$h after 4 exports"

# --- batch search: the per-call producer -------------------------------------
# pg_cuvs_batch_search() is the only route to CUVS_OP_SEARCH_BATCH. Requiring
# rows back is what keeps this arm from passing without exercising the path.
rows_total=0
batch_failed=0
for i in $(seq 1 10); do
  lo=$(( (i - 1) * 8 + 1 )); hi=$(( i * 8 ))
  n=$($PSQL -c "SET cuvs.index_dir='${IDIR}';
                SELECT count(*) FROM pg_cuvs_batch_search(
                    't',
                    ARRAY(SELECT embedding FROM t WHERE id BETWEEN ${lo} AND ${hi}),
                    10);" 2>/dev/null | tr -d '[:space:]')
  case "$n" in
    ''|*[!0-9]*) batch_failed=$((batch_failed+1)) ;;
    *)           rows_total=$((rows_total + n)) ;;
  esac
done

if [ "$batch_failed" -ne 0 ]; then
  bad "pg_cuvs_batch_search failed on ${batch_failed}/10 calls"
elif [ "$rows_total" -eq 0 ]; then
  bad "pg_cuvs_batch_search returned no rows — batch path never ran"
else
  ok "pg_cuvs_batch_search ran 10x (${rows_total} rows)"
  b=$(residue bsr)
  [ "$b" = "0" ] && ok "no pg_cuvs_bsr_ residue after 10 batch searches" \
                 || bad "pg_cuvs_bsr_ residue=$b after 10 batch searches"
fi

if [ "$FAIL" -gt 0 ]; then
  echo "  residue sample:"; ls /dev/shm 2>/dev/null | grep '^pg_cuvs_' | head -5
fi
echo "== $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
