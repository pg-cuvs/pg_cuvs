#!/bin/sh
# I3-E CPU-shim integration scenario: real PostgreSQL 16 + pg_cuvs_server
# built with PGCUVS_CPU_SHIM=1 (no GPU). Runs INSIDE the pgcuvs-i1-cpu image
# with a copy of the source tree at /w:
#
#   docker run --rm -i -v <tree>:/src:ro pgcuvs-i1-cpu:20261031 sh -s < (copy to /w, then this)
#
# Covers the SQL path of the I3-E observation surface: column sets of the
# three functions, memory_status values against the daemon log (one reject
# count per [admit] reject line, gpu -1 in SHOW rejects_no_gpu), privilege
# rules (memory_status superuser/pg_monitor, build_status PUBLIC with row
# filter, SHOW builds null for other roles) and an in-flight build observed
# through pg_cuvs_build_status (CUVS_SHIM_BUILD_DELAY_MS holds the build).
set -u
cd /w
FAILS=0
check() { # name, condition-command...
    name=$1; shift
    if "$@" >/dev/null 2>&1; then echo "PASS $name"; else echo "FAIL $name"; FAILS=$((FAILS+1)); fi
}

make PGCUVS_CPU_SHIM=1 with_llvm=no -j8 all server >/tmp/b.log 2>&1 || { tail -20 /tmp/b.log; exit 2; }
make PGCUVS_CPU_SHIM=1 with_llvm=no install >/tmp/i.log 2>&1 || { tail -20 /tmp/i.log; exit 2; }
install -m 755 pg_cuvs_server /usr/local/bin/

adduser -D pg 2>/dev/null || true
IDX=/tmp/cuvs_indexes
mkdir -p $IDX && chmod 0777 $IDX
su pg -c "initdb -D /tmp/pgdata -A trust >/tmp/initdb.log 2>&1"
cat >> /tmp/pgdata/postgresql.conf <<EOF
shared_preload_libraries = 'pg_cuvs'
cuvs.index_dir = '$IDX'
unix_socket_directories = '/tmp'
EOF
su pg -c "pg_ctl -D /tmp/pgdata -l /tmp/pg.log -w start >/dev/null"

S=/tmp/.s.pg_cuvs
startd() { # env assignment ("" for none), extra daemon flags...
    envs=$1; shift
    pkill -f pg_cuvs_server 2>/dev/null; sleep 1; pkill -9 -f pg_cuvs_server 2>/dev/null
    rm -f $S
    su pg -c "$envs pg_cuvs_server --socket $S --index-dir $IDX --max-vram-mb 100 --rmm-scratch-permille 8000 $* >>/tmp/d.log 2>&1 &"
    for i in $(seq 1 30); do [ -S $S ] && break; sleep 1; done
}
# psql as role $1 (pg = superuser)
q() { su pg -c "psql -h /tmp -X -q -At -d postgres -U $1 -c \"$2\"" 2>&1; }
qs() { q pg "$1"; }

startd ""
qs "CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs;" >/dev/null
qs "CREATE ROLE alice LOGIN; CREATE ROLE mon LOGIN IN ROLE pg_monitor; GRANT CREATE ON SCHEMA public TO alice;" >/dev/null

# --- 1. column sets (CK-27 e: installed functions) ---
nargs() { qs "SELECT array_length(proallargtypes, 1) FROM pg_proc WHERE proname = '$1'"; }
check "memory_status has 44 OUT columns (25 I3-B + 19 I3-E)" [ "$(nargs pg_cuvs_memory_status)" = 44 ]
check "index_status has 24 OUT columns (+ dataset_attached)" [ "$(nargs pg_cuvs_index_status)" = 24 ]
check "build_status has 16 OUT columns" [ "$(nargs pg_cuvs_build_status)" = 16 ]

# --- 2. idle values ---
check "idle memory_status: budget 100 MiB, no quota, pool NULL, nothing active or rejected" \
    [ "$(qs "SELECT budget_bytes, quota_bytes, pool_used IS NULL, pool_reserved IS NULL, pending_rmm, lw_pending, builds_active, rejects_ar0+rejects_ar1+rejects_ar2+rejects_ar3+rejects_ar4+rejects_ar5+rejects_ar6+rejects_ar7, last_reject_reason IS NULL, last_reject_at IS NULL FROM pg_cuvs_memory_status() WHERE gpu = 0")" = "104857600|0|t|t|0|0|0|0|t|t" ]
check "idle build_status is empty" [ "$(qs "SELECT count(*) FROM pg_cuvs_build_status()")" = 0 ]
check "SHOW builds [] for a superuser" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -c 'SHOW cuvs.rmm_pool_status'\" | grep -q '\"builds_unlisted\":0,\"rejects_no_gpu\":{\"ar0\":0,.*},\"builds\":\[\]}'"
check "SHOW builds null for another role" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -U alice -c 'SHOW cuvs.rmm_pool_status'\" | grep -q '\"builds\":null}'"
check "SHOW builds [] for pg_monitor" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -U mon -c 'SHOW cuvs.rmm_pool_status'\" | grep -q '\"builds\":\[\]}'"
q alice "SELECT count(*) FROM pg_cuvs_memory_status()" >/tmp/ms_alice.out 2>&1
echo "  alice memory_status: $(head -1 /tmp/ms_alice.out)"
check "memory_status refused for another role (EXECUTE revoked from PUBLIC)" grep -q 'permission denied for function pg_cuvs_memory_status' /tmp/ms_alice.out
check "memory_status allowed for pg_monitor" [ "$(q mon "SELECT count(*) > 0 FROM pg_cuvs_memory_status()")" = t ]
check "build_status callable by another role" [ "$(q alice "SELECT count(*) FROM pg_cuvs_build_status()")" = 0 ]

# --- 3. rejections: counters vs [admit] reject lines ---
# Explicit fixture rho = 8000: a 20000 x 64 index has final
# 10,240,000 B and N = 92,160,000 B, so --max-vram-mb 100 admits i1 and ia
# and rejects i2 on the GPU (no eviction by default); t4 (60000 rows) is AR0.
mk() { # role table rows
    q $1 "CREATE TABLE $2 (id int, v vector(64)); INSERT INTO $2 SELECT g, (SELECT array_agg(((g*31+i) % 97)::real) FROM generate_series(1,64) i)::vector FROM generate_series(1,$3) g;" >/dev/null
}
mk pg t1 20000; mk alice t3 20000; mk pg t2 20000; mk pg t4 60000
qs "SET client_min_messages=warning; CREATE INDEX i1 ON t1 USING cagra (v vector_l2_ops);" >/dev/null
q alice "SET client_min_messages=warning; CREATE INDEX ia ON t3 USING cagra (v vector_l2_ops);" >/dev/null
qs "SET client_min_messages=warning; CREATE INDEX i2 ON t2 USING cagra (v vector_l2_ops);" >/dev/null   # no room
qs "SET client_min_messages=warning; CREATE INDEX i4 ON t4 USING cagra (v vector_l2_ops);" >/dev/null   # AR0, no GPU
grep '\[admit\]' /tmp/d.log | grep 'verdict=reject' | sed 's/.*\(gpu=[-0-9]*\).*\(reason=AR[0-7]\).*/  \1 \2/'
NG=$(grep '\[admit\]' /tmp/d.log | grep 'verdict=reject' | grep -c 'gpu=[0-9]')
NN=$(grep '\[admit\]' /tmp/d.log | grep 'verdict=reject' | grep -c 'gpu=-1')
LR=$(grep '\[admit\]' /tmp/d.log | grep 'verdict=reject' | grep 'gpu=0' | tail -1 | sed 's/.*reason=\(AR[0-7]\).*/\1/')
check "a GPU rejection and a no-GPU AR0 were logged" [ "$NG" -ge 1 -a "$NN" -ge 1 ]
check "sum of memory_status rejects = [admit] reject lines with a GPU ($NG)" \
    [ "$(qs "SELECT sum(rejects_ar0+rejects_ar1+rejects_ar2+rejects_ar3+rejects_ar4+rejects_ar5+rejects_ar6+rejects_ar7) FROM pg_cuvs_memory_status()")" = "$NG" ]
check "SHOW rejects_no_gpu.ar0 = [admit] reject lines with gpu=-1 ($NN)" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -c 'SHOW cuvs.rmm_pool_status'\" | grep -q '\"rejects_no_gpu\":{\"ar0\":$NN,'"
check "gpu 0 last_reject_reason = last logged reason ($LR) with a time" \
    [ "$(qs "SELECT last_reject_reason, last_reject_at IS NOT NULL FROM pg_cuvs_memory_status() WHERE gpu = 0")" = "$LR|t" ]
check "a second read returns the same counters (no reset)" \
    [ "$(qs "SELECT sum(rejects_ar0+rejects_ar1+rejects_ar2+rejects_ar3+rejects_ar4+rejects_ar5+rejects_ar6+rejects_ar7) FROM pg_cuvs_memory_status()")" = "$NG" ]
check "index_status dataset_attached is NULL" [ "$(qs "SELECT bool_and(dataset_attached IS NULL), count(*) > 0 FROM pg_cuvs_index_status()")" = "t|t" ]

# --- 4. in-flight builds (the shim sleeps 4 s inside the wrapper) ---
qs "DROP TABLE t4; DROP TABLE t2;" >/dev/null
startd "CUVS_SHIM_BUILD_DELAY_MS=4000"
qs "SELECT pg_cuvs_unload('i1'::regclass)" >/dev/null
q alice "SET client_min_messages=warning; REINDEX INDEX ia;" >/tmp/reindex.out 2>&1 &
sleep 2
check "REINDEX by the owner: one row, named, single, building" \
    [ "$(q alice "SELECT index_name, path, phase, lw_ub_src, policy FROM pg_cuvs_build_status()")" = "public.ia|single|building|admission|managed" ]
check "build_status row: reserved_n = final + scratch, lw_ub > 0, started_at in the last 10 s" \
    [ "$(qs "SELECT reserved_n = final + scratch, lw_ub > 0, started_at > now() - interval '10 s', n, dim FROM pg_cuvs_build_status()")" = "t|t|t|20000|64" ]
check "memory_status agrees: builds_active, pending_bytes = sum(reserved_n), lw_pending = sum(lw_ub)" \
    [ "$(qs "SELECT m.builds_active = b.c AND m.pending_bytes = b.r AND m.lw_pending = b.l FROM (SELECT gpu, count(*) c, sum(reserved_n) r, sum(lw_ub) l FROM pg_cuvs_build_status() GROUP BY gpu) b JOIN pg_cuvs_memory_status() m USING (gpu)")" = t ]
check "pg_monitor sees the build" [ "$(q mon "SELECT count(*) FROM pg_cuvs_build_status()")" = 1 ]
check "SHOW builds lists it for a superuser" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -c 'SHOW cuvs.rmm_pool_status'\" | grep -q '\"builds\":\[{\"gpu\":0,.*\"path\":\"single\".*\"lw_ub_src\":\"admission\",\"phase\":\"building\"}\]}'"
wait
check "after the REINDEX: no row, nothing active" [ "$(qs "SELECT (SELECT count(*) FROM pg_cuvs_build_status()), (SELECT sum(builds_active) FROM pg_cuvs_memory_status())")" = "0|0" ]
qs "SELECT pg_cuvs_unload('ia'::regclass)" >/dev/null
qs "DROP INDEX i1;" >/dev/null
qs "SET client_min_messages=warning; CREATE INDEX i5 ON t1 USING cagra (v vector_l2_ops);" >/tmp/create.out 2>&1 &
sleep 2
check "CREATE INDEX in flight: superuser sees it with index_name NULL (not committed)" \
    [ "$(qs "SELECT count(*), bool_and(index_name IS NULL) FROM pg_cuvs_build_status()")" = "1|t" ]
check "CREATE INDEX in flight: another role sees no row" [ "$(q alice "SELECT count(*) FROM pg_cuvs_build_status()")" = 0 ]
wait
check "after CREATE INDEX: no row" [ "$(qs "SELECT count(*) FROM pg_cuvs_build_status()")" = 0 ]
echo "--- reindex/create output"; cat /tmp/reindex.out /tmp/create.out | head -5
echo "FAILS=$FAILS"
[ $FAILS -eq 0 ]
