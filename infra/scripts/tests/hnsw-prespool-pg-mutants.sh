#!/bin/bash
# For each mutant: copy tree, apply one sed, build+install, run scenario A (ace, stats)
# and C (error knob, stats), print what the user sees and spool temp-file counts.
# Usage (local, free): docker run --rm -i -v "$PWD":/src:ro pgcuvs-i1-cpu:20261031 \
#   bash -s < infra/scripts/tests/hnsw-prespool-pg-mutants.sh
set -uo pipefail
mkdir -p /testpg /tmp/cuvs_indexes && chown postgres /testpg /tmp/cuvs_indexes
su-exec postgres initdb -D /testpg > /dev/null 2>&1
run_case() {
  local name=$1 file=$2 expr=$3
  rm -rf /tmp/b && mkdir /tmp/b && cd /src && tar --exclude=./.git --exclude='./test-*' -cf - . | tar -xf - -C /tmp/b && cd /tmp/b
  if [ "$name" != baseline ]; then
    cp "$file" /tmp/orig; sed -i "$expr" "$file"
    if cmp -s /tmp/orig "$file"; then echo "[$name] MUTATION DID NOT APPLY"; return; fi
  fi
  make with_llvm=no PGCUVS_CPU_SHIM=1 all server install install-server > /tmp/build.log 2>&1 || { echo "[$name] BUILD FAILED"; tail -5 /tmp/build.log; return; }
  rm -f /tmp/pg.log
  su-exec postgres pg_ctl -D /testpg -l /tmp/pg.log -w -o "-c shared_preload_libraries=pg_cuvs -c autovacuum=off -c log_temp_files=0 -c unix_socket_directories=/tmp" start > /dev/null
  q() { su-exec postgres psql -h /tmp -X -q -v ON_ERROR_STOP=0 "$@" 2>&1 || true; }
  q -c "DROP EXTENSION IF EXISTS pg_cuvs CASCADE" -c "DROP TABLE IF EXISTS t" -c "CREATE EXTENSION IF NOT EXISTS vector" -c "CREATE EXTENSION pg_cuvs" >/dev/null
  q -c "CREATE TABLE t AS SELECT i AS id, ('[' || array_to_string(ARRAY(SELECT (random())::real FROM generate_series(1,8) WHERE i > 0), ',') || ']')::vector(8) AS v FROM generate_series(1,400000) i" -c "ANALYZE t" >/dev/null
  for mode in ace error; do
    pkill -f pg_cuvs_server; sleep .3; rm -f /tmp/.s.pg_cuvs
    if [ $mode = ace ]; then su-exec postgres pg_cuvs_server --socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --max-vram-mb 8192 > /tmp/d.log 2>&1 &
    else su-exec postgres pg_cuvs_server --socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --max-vram-mb 8192 --hnsw-auto-overflow error > /tmp/d.log 2>&1 & fi
    for i in $(seq 1 100); do test -S /tmp/.s.pg_cuvs && break; sleep .1; done
    q -c "DO \$\$BEGIN RAISE LOG 'MUT-MARK'; END\$\$" >/dev/null
    out=$(q -c "SET cuvs.max_build_mem_mb = 1536" -c "CREATE INDEX ti ON t USING pg_cuvs_hnsw (v vector_l2_ops) WITH (mode='nsw')" | grep -E '^(NOTICE|ERROR|DETAIL)' | cut -c1-150 | tr '\n' '|')
    temps=$(awk 'index($0,"MUT-MARK"){on=1;n=0;next} on && /temporary file/{n++} END{print n+0}' /tmp/pg.log)
    echo "[$name] daemon-flag=$mode spool_temp_files=$temps :: $out"
  done
  pkill -f pg_cuvs_server; su-exec postgres pg_ctl -D /testpg stop -m fast > /dev/null
}
run_case baseline - -
run_case default_reverted_to_error src/cuvs_hnsw_policy.h 's/#define CUVS_HNSW_AUTO_OVERFLOW_DEFAULT CUVS_HNSW_OVERFLOW_ACE/#define CUVS_HNSW_AUTO_OVERFLOW_DEFAULT CUVS_HNSW_OVERFLOW_ERROR/'
run_case decision_after_spool src/pg_cuvs.c 's/        prespool = cuvs_hnsw_preflight(stats_rows, spool->dim, hierarchy, build_algo,/        prespool = CUVS_HNSW_PRESPOOL_FALLBACK; (void)cuvs_hnsw_preflight; if (0) cuvs_hnsw_preflight(stats_rows, spool->dim, hierarchy, build_algo,/'
echo DONE
