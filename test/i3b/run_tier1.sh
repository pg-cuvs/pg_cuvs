#!/bin/sh
# Tier-1 CPU-shim regression (same subset and daemon flags as ci.yml) inside
# the pgcuvs-i1-cpu image; tree mounted at /w. Prints the summary and diffs.
#   docker run --rm -v <copy>:/w -w /w pgcuvs-i1-cpu:20261031 sh test/i3b/run_tier1.sh [fault]
set -u
cd /w
make PGCUVS_CPU_SHIM=1 with_llvm=no -j8 all server >/tmp/b.log 2>&1 || { tail -20 /tmp/b.log; exit 2; }
make PGCUVS_CPU_SHIM=1 with_llvm=no install >/tmp/i.log 2>&1 || exit 2
install -m 755 pg_cuvs_server /usr/local/bin/
adduser -D pg 2>/dev/null || true
mkdir -p /tmp/cuvs_indexes && chmod 0777 /tmp/cuvs_indexes
chown -R pg /w
su pg -c "initdb -D /tmp/pgdata -A trust >/tmp/initdb.log 2>&1"
cat >> /tmp/pgdata/postgresql.conf <<'EOC'
shared_preload_libraries = 'pg_cuvs'
cuvs.index_dir = '/tmp/cuvs_indexes'
unix_socket_directories = '/tmp'
EOC
su pg -c "pg_ctl -D /tmp/pgdata -l /tmp/pg.log -w start >/dev/null"
BIN=pg_cuvs_server
su pg -c "$BIN --socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --max-vram-mb 38000 >/tmp/d.log 2>&1 &"
for i in $(seq 1 30); do [ -S /tmp/.s.pg_cuvs ] && break; sleep 1; done
if [ "${1:-}" = fault ]; then
  su pg -c "PGHOST=/tmp make PGCUVS_CPU_SHIM=1 with_llvm=no installcheck-fault 2>&1" > /tmp/r.log || true
elif [ "${1:-}" = maxidx ]; then
  su pg -c "PGHOST=/tmp PG_CUVS_SERVER_BIN=/usr/local/bin/pg_cuvs_server bash infra/scripts/tests/max-indexes-scale.sh 2>&1" > /tmp/r.log || echo "MAXIDX_RC=$?" >> /tmp/r.log
  tail -40 /tmp/r.log
else
  su pg -c "PGHOST=/tmp make PGCUVS_CPU_SHIM=1 with_llvm=no installcheck-tier1 2>&1" > /tmp/r.log || true
fi
grep -E '^(ok|not ok)|tests passed|failed|^# ' /tmp/r.log | tail -70
[ -f test/regression.diffs ] && { echo "===== diffs"; head -300 test/regression.diffs; }
