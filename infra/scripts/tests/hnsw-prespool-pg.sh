#!/bin/bash
# I3-C actual PostgreSQL 16 + CPU-shim daemon check of the pre-spool path.
# Runs inside pgcuvs-i1-cpu:20261031 with the worktree at /src (ro), /idxtmp tmpfs.
# Usage (local, free): docker run --rm -i --tmpfs /idxtmp -v "$PWD":/src:ro \
#   pgcuvs-i1-cpu:20261031 bash -s < infra/scripts/tests/hnsw-prespool-pg.sh
# Expected: see the I3-C PR body (A/B NOTICE, C/D/E2 refused with 0 temp files).
set -euo pipefail
mkdir /tmp/b && cd /src && tar --exclude=./.git --exclude='./test-*' -cf - . | tar -xf - -C /tmp/b
cd /tmp/b
make with_llvm=no PGCUVS_CPU_SHIM=1 all server install install-server > /tmp/build.log 2>&1 || { tail -30 /tmp/build.log; exit 1; }
mkdir -p /testpg /tmp/cuvs_indexes && chown postgres /testpg /tmp/cuvs_indexes /idxtmp
su-exec postgres initdb -D /testpg > /dev/null
su-exec postgres pg_ctl -D /testpg -l /tmp/pg.log -w \
  -o "-c shared_preload_libraries=pg_cuvs -c autovacuum=off -c log_temp_files=0 -c unix_socket_directories=/tmp" start > /dev/null
DPID=
daemon() { # $1 = label, rest = extra flags
  local label=$1; shift
  if [ -n "$DPID" ]; then kill "$DPID"; wait "$DPID" 2>/dev/null || true; fi
  rm -f /tmp/.s.pg_cuvs
  su-exec postgres /usr/local/bin/pg_cuvs_server --socket /tmp/.s.pg_cuvs "$@" > "/tmp/daemon-$label.log" 2>&1 &
  DPID=$!
  for i in $(seq 1 100); do test -S /tmp/.s.pg_cuvs && break; sleep .1; done
  test -S /tmp/.s.pg_cuvs
}
q() { su-exec postgres psql -h /tmp -X -q -v ON_ERROR_STOP=0 -v VERBOSITY=default "$@" 2>&1 || true; }
mark() { q -c "DO \$\$BEGIN RAISE LOG 'I3C-MARK $1'; END\$\$;" > /dev/null; }
temps_since() { awk -v m="I3C-MARK $1" 'index($0,m){on=1;next} on && /temporary file/{n++} END{print n+0}' /tmp/pg.log; }

daemon default --index-dir /tmp/cuvs_indexes --max-vram-mb 8192
q <<'SQL'
\set VERBOSITY verbose
CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs;
CREATE TABLE t (id int, v vector(8));
INSERT INTO t SELECT i, ('[' || array_to_string(ARRAY(SELECT (random())::real FROM generate_series(1,8) WHERE i > 0), ',') || ']')::vector
  FROM generate_series(1, 400000) i;
CREATE TABLE t_nostats AS SELECT * FROM t;
ANALYZE t;
SELECT relname, reltuples FROM pg_class WHERE relname IN ('t', 't_nostats') ORDER BY 1;
SELECT current_setting('cuvs.rmm_pool_status')::jsonb ->> 'hnsw_auto_overflow' AS f_pool_status_hnsw_auto_overflow;
SQL
echo "== A: default daemon (ace), statistics present, auto over the 1536 MiB host cap (400k x 8: estimate 1659 MiB)"
mark A
q -c "SET cuvs.max_build_mem_mb = 1536" -c "CREATE INDEX t_a ON t USING pg_cuvs_hnsw (v vector_l2_ops) WITH (mode = 'nsw')"
echo "A temp files logged after mark: $(temps_since A)"
echo "== B: default daemon (ace), no statistics (reltuples -1)"
mark B
q -c "SET cuvs.max_build_mem_mb = 1536" -c "CREATE INDEX t_b ON t_nostats USING pg_cuvs_hnsw (v vector_l2_ops) WITH (mode = 'nsw')"
echo "B temp files logged after mark: $(temps_since B)"
echo "== A2: default daemon, statistics present, within the cap (resident, no transition NOTICE)"
mark A2
q -c "SET cuvs.max_build_mem_mb = 3072" -c "CREATE INDEX t_a2 ON t USING pg_cuvs_hnsw (v vector_l2_ops) WITH (mode = 'nsw')"
grep '\[hnsw_route\]' /tmp/daemon-default.log | sed 's/^/daemon-default: /'

daemon error --index-dir /tmp/cuvs_indexes --max-vram-mb 8192 --hnsw-auto-overflow error
echo "== C: --hnsw-auto-overflow error, statistics present: refused before the scan"
mark C
q -c "\\set VERBOSITY verbose" -c "SET cuvs.max_build_mem_mb = 1536" -c "CREATE INDEX t_c ON t USING pg_cuvs_hnsw (v vector_l2_ops) WITH (mode = 'nsw')"
echo "C temp files logged after mark: $(temps_since C)"
echo "== C2: --hnsw-auto-overflow error, no statistics: refused by the build after the scan"
mark C2
q -c "\\set VERBOSITY verbose" -c "SET cuvs.max_build_mem_mb = 1536" -c "CREATE INDEX t_c2 ON t_nostats USING pg_cuvs_hnsw (v vector_l2_ops) WITH (mode = 'nsw')"
echo "C2 temp files logged after mark: $(temps_since C2)"
q -c "SELECT current_setting('cuvs.rmm_pool_status')::jsonb ->> 'hnsw_auto_overflow' AS c_pool_status_hnsw_auto_overflow"
grep '\[hnsw_route\]\|HNSW auto overflow' /tmp/daemon-error.log | sed 's/^/daemon-error: /'

daemon tmpfs --index-dir /idxtmp --max-vram-mb 8192
echo "== D: default daemon, index dir on tmpfs: disk precheck refuses before the scan"
mark D
q -c "SET cuvs.max_build_mem_mb = 3072" -c "CREATE INDEX t_d ON t USING pg_cuvs_hnsw (v vector_l2_ops) WITH (mode = 'nsw')"
echo "D temp files logged after mark: $(temps_since D)"
grep '\[hnsw_route\]' /tmp/daemon-tmpfs.log | sed 's/^/daemon-tmpfs: /'

daemon ar0 --index-dir /tmp/cuvs_indexes --max-vram-mb 1024
echo "== E: CAGRA, 1 GiB limit, reltuples 400000 (115 MB estimate) under a 1 GiB limit: current path"
q -c "SET cuvs.index_dir = '/tmp/cuvs_indexes'" -c "SET cuvs.shard_count = 1" -c "CREATE INDEX t_e ON t USING cagra (v vector_l2_ops)" -c "DROP INDEX IF EXISTS t_e"
q -c "UPDATE pg_class SET reltuples = 10000000 WHERE relname = 't'"
echo "== E2: CAGRA, reltuples forged to 1e7 (2.88 GB estimate) > 1 GiB: AR0 before the scan"
mark E2
q -c "\\set VERBOSITY verbose" -c "SET cuvs.index_dir = '/tmp/cuvs_indexes'" -c "SET cuvs.shard_count = 1" -c "CREATE INDEX t_e2 ON t USING cagra (v vector_l2_ops)"
echo "E2 temp files logged after mark: $(temps_since E2)"
echo "== E3: same, shard_count = 2 (could be split): no pre-scan AR0"
q -c "SET cuvs.index_dir = '/tmp/cuvs_indexes'" -c "SET cuvs.shard_count = 2" -c "CREATE INDEX t_e3 ON t USING cagra (v vector_l2_ops)"
echo "== E4: same, reltuples -1 (no statistics): no pre-scan AR0"
q -c "UPDATE pg_class SET reltuples = -1 WHERE relname = 't'"
q -c "SET cuvs.index_dir = '/tmp/cuvs_indexes'" -c "SET cuvs.shard_count = 1" -c "CREATE INDEX t_e4 ON t USING cagra (v vector_l2_ops)"
kill "$DPID" 2>/dev/null || true
su-exec postgres pg_ctl -D /testpg stop -m fast > /dev/null
echo "== DONE"
