#!/bin/bash
# #166 review P1: the HNSW export must survive a hardened daemon umask.
#
# The daemon hands the serialized sidecar to the backend as an SCM_RIGHTS fd.
# Reads through that descriptor carry the access rights granted at open time, so
# they work regardless of the file's mode. Reaching the same inode by *path*
# does not: opening "/proc/self/fd/N" re-checks inode permissions, and with
# UMask=077 the segment lands 0600 owned by the daemon uid — the PG backend
# (a different uid) then gets EACCES. That is why the importer consumes the
# descriptor directly instead of reopening a path.
#
# Pre-fix reproduction on this harness:
#   ERROR:  pg_cuvs: cannot open .hnsw sidecar "/proc/self/fd/48": Permission denied
#
# SCOPE: this exercises the TRANSIENT handoff only. Persistent sidecars in
# cuvs.index_dir (.cagra/.tids/.vectors) also inherit the daemon's umask, and the
# backend reads .tids by path — so a genuinely hardened daemon breaks that too.
# That is a separate concern this guard does not cover: the run normalizes
# sidecar modes first so the assertion isolates the handoff, and restores them
# afterwards so a hardened-umask run cannot leave the shared index_dir
# unreadable for later tests.
#
# Installs a temporary systemd drop-in, exercises both export modes, and always
# removes the drop-in again. Run on the GPU VM; needs sudo, systemd, and a table
# `t` with a CAGRA index `t_cagra`.
set -u
IDIR="${CUVS_INDEX_DIR:-/tmp/cuvs_indexes}"
PSQL="psql -d ${PGDATABASE:-shadeform} -qtA -v ON_ERROR_STOP=1"
UNIT_DIR=/etc/systemd/system/pg-cuvs-server.service.d
DROPIN="$UNIT_DIR/zz-umask-hardened-test.conf"
PASS=0; FAIL=0
ok(){  PASS=$((PASS+1)); echo "  [PASS] $1"; }
bad(){ FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

restore(){
  sudo rm -f "$DROPIN"
  sudo systemctl daemon-reload
  sudo systemctl restart pg-cuvs-server || true
  sudo systemctl restart postgresql@16-main || true
  sleep 4
  # Anything the hardened daemon persisted landed 0600; leaving it that way
  # would break every later cross-uid run in this index_dir.
  sudo chmod 644 "$IDIR"/* 2>/dev/null || true
}
trap restore EXIT

echo "== #166 hardened-umask export =="

# Start from readable sidecars so the assertion isolates the transient handoff.
sudo chmod 644 "$IDIR"/* 2>/dev/null || true

sudo mkdir -p "$UNIT_DIR"
printf '[Service]\nUMask=077\n' | sudo tee "$DROPIN" >/dev/null
sudo systemctl daemon-reload
sudo rm -f /dev/shm/pg_cuvs_*
sudo systemctl restart pg-cuvs-server || { bad "daemon restart"; exit 1; }
sudo systemctl restart postgresql@16-main || { bad "postgres restart"; exit 1; }
sleep 5

actual=$(systemctl show pg-cuvs-server -p UMask --value)
[ "$actual" = "0077" ] && ok "daemon running with UMask=0077" \
                       || bad "expected UMask=0077, got '${actual}'"

for mode in hnswlib nsw; do
  if $PSQL -c "SET cuvs.index_dir='${IDIR}';
               SET maintenance_work_mem='8GB';
               DROP INDEX IF EXISTS t_hnsw_3i;
               CREATE INDEX t_hnsw_3i ON t USING pg_cuvs_hnsw (embedding vector_l2_ops)
                      WITH (source='t_cagra', mode='${mode}');" >/dev/null 2>/tmp/umask_err.txt; then
    ok "export mode=${mode} under UMask=077"
  else
    bad "export mode=${mode} under UMask=077: $(head -1 /tmp/umask_err.txt)"
  fi
done

r=$(ls /dev/shm 2>/dev/null | grep -c '^pg_cuvs_')
[ "$r" = "0" ] && ok "no pg_cuvs_* residue under UMask=077" \
               || bad "pg_cuvs_* residue=$r under UMask=077"

echo "== $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
