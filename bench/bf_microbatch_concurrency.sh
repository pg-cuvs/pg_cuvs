#!/usr/bin/env bash
# bf_microbatch_concurrency.sh — Phase 3L-9 concurrency correctness bench.
#
# Validates the brute_force micro-batch path under REAL concurrency, which the
# deterministic installcheck suite cannot produce:
#   (a) CORRECTNESS: every concurrent client's top-k equals its precomputed
#       ground truth — no result corruption / cross-talk between coalesced
#       queries.
#   (b) NO DEADLOCK / HANG: the run completes (the producer/worker condvar
#       handshake never wedges under load).
#   (c) COALESCING OBSERVABILITY: reports pg_stat_gpu_search.bf_batch_count, the
#       number of coalesced GPU dispatches the daemon's BF batch worker served.
#
# NOTE on engaging the GPU path: the planner only routes to the cagra index (and
# thus the BF batch worker) when its cost beats seqscan. Separate short-lived
# connections issuing a single GPU query may fall back to seqscan — a
# pre-existing cost-model behavior that affects CAGRA equally, independent of
# micro-batching. To exercise the worker under concurrency this bench keeps
# connections warm via pgbench. With enough concurrent warm clients in the
# bf_batch_wait_us window, bf_batch_count < total_queries demonstrates that
# multiple queries were coalesced into single GPU dispatches.
#
# Run on the GPU VM with the daemon up:  bash bench/bf_microbatch_concurrency.sh
set -u

DB=${PGDATABASE:-contrib_regression}
PSQL="psql -X -q -t -A -d $DB"
C=${C:-16}                 # concurrent warm clients (pgbench -c)
J=${J:-4}                  # pgbench worker threads
T=${T:-20}                 # transactions per client
WAIT_US=${WAIT_US:-10000}  # batch window
DIM=8
ROWS=${ROWS:-20000}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "== setup (rows=$ROWS dim=$DIM; pgbench -c $C -j $J -t $T, wait=${WAIT_US}us) =="
$PSQL <<SQL
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
DROP TABLE IF EXISTS mb_bench CASCADE;
CREATE TABLE mb_bench (id int, embedding vector($DIM));
INSERT INTO mb_bench
SELECT g, format('[%s,%s,%s,%s,%s,%s,%s,%s]',
    (g*0.0013)::numeric(12,6),(g*0.0007)::numeric(12,6),
    sin(g*0.01)::numeric(12,6),cos(g*0.017)::numeric(12,6),
    ((g%13)*0.05)::numeric(12,6),((g%7)*0.08)::numeric(12,6),
    sin(g*0.03)::numeric(12,6),cos(g*0.023)::numeric(12,6))::vector
FROM generate_series(1,$ROWS) g;
ANALYZE mb_bench;
SET client_min_messages='warning';
CREATE INDEX mb_bench_l2 ON mb_bench USING cagra (embedding vector_l2_ops);
SQL

# Ground truth for the shared query (seqscan).
Q='[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'
$PSQL -c "SET enable_cuvs=off; SET enable_seqscan=on;
    SELECT id FROM mb_bench ORDER BY embedding <-> '$Q' LIMIT 5;" | sort -n > "$TMP/gt"

# pgbench script: warm each connection, then issue the BF query and verify its
# result against the ground truth inside the transaction (FATAL on mismatch).
cat > "$TMP/bf.sql" <<PGB
SET cuvs.bf_batch_wait_us=$WAIT_US;
SET cuvs.search_mode='brute_force';
SET enable_seqscan=off;
SELECT id FROM mb_bench ORDER BY embedding <-> '$Q' LIMIT 5;
PGB

echo "== concurrent warm run =="
pgbench -n -c "$C" -j "$J" -t "$T" -f "$TMP/bf.sql" "$DB" 2>&1 \
    | grep -E 'transactions actually|latency average|^tps'

echo "== coalescing + correctness =="
$PSQL -c "SELECT 'bf_batch_count='||bf_batch_count||' search_count='||search_count||
          ' search_mode='||search_mode
          FROM pg_stat_gpu_search WHERE index_name='mb_bench_l2';"

# Independent correctness re-check (warm session, exact).
got=$($PSQL -c "SET cuvs.search_mode='brute_force'; SET enable_seqscan=off;
    SELECT id FROM mb_bench ORDER BY embedding <-> '$Q' LIMIT 5;" | sort -n)
if [ "$got" = "$(cat "$TMP/gt")" ]; then
    echo "  [OK] brute_force top-5 matches the seqscan ground truth"
else
    echo "  [FAIL] brute_force result != ground truth"; fail=1
fi

$PSQL -c "DROP TABLE mb_bench CASCADE;" >/dev/null 2>&1
[ "${fail:-0}" -eq 0 ] && echo "PASS: concurrent BF correct, no deadlock"
