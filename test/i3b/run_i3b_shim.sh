#!/bin/sh
# I3-B CPU-shim integration scenario: real PostgreSQL 16 + pg_cuvs_server
# built with PGCUVS_CPU_SHIM=1 (no GPU). Runs INSIDE the pgcuvs-i1-cpu image
# with the source tree mounted at /w:
#
#   docker run --rm -v <copy-of-tree>:/w -w /w pgcuvs-i1-cpu:20261031 sh test/i3b/run_i3b_shim.sh
#
# Each CHECK prints PASS/FAIL; the script exits nonzero on any FAIL.
# Index footprint in the shim: estimate_vram_bytes(20000, 64, 64) =
# 20000 * (64*4 + 64*4) = 10,240,000 B, so --max-vram-mb 25 holds two.
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
gcc -std=gnu11 -I src -o /usr/local/bin/i3b_ipc_cli test/i3b/i3b_ipc_cli.c src/cuvs_ipc.c \
    src/cuvs_util.c src/cuvs_build_corpus.c -lpthread -lm -lrt || exit 2

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
startd() { # extra daemon flags...
    pkill -f pg_cuvs_server 2>/dev/null; sleep 1; pkill -9 -f pg_cuvs_server 2>/dev/null
    rm -f $S
    su pg -c "pg_cuvs_server --socket $S --index-dir $IDX --max-vram-mb 25 $* >>/tmp/d.log 2>&1 &"
    for i in $(seq 1 30); do [ -S $S ] && break; sleep 1; done
}
cli() { su pg -c "i3b_ipc_cli $S $*"; }
psqlq() { su pg -c "psql -h /tmp -X -q -At -d postgres -c \"$1\"" 2>&1; }
oid() { psqlq "SELECT '$1'::regclass::oid"; }
DB=$(su pg -c "psql -h /tmp -X -At -d postgres -c 'SELECT oid FROM pg_database WHERE datname = current_database()'")
row() { cli index | awk -F'\t' -v i="$1" '$2==i'; }
col() { # index_oid, column name
    cli index | awk -F'\t' -v i="$1" -v c="$2" 'NR==2{for(k=1;k<=NF;k++) if($k==c) n=k} $2==i{print $n}'
}

# --- 0. flags: bad values refuse to start (CK-9) ---
check "flag: bad --reclaim-mode refused" sh -c "! pg_cuvs_server --socket /tmp/x.sock --index-dir $IDX --reclaim-mode bogus"
check "flag: low > high refused" sh -c "! pg_cuvs_server --socket /tmp/x.sock --index-dir $IDX --reclaim-high-pct 50 --reclaim-low-pct 60"
check "flag: idle:-1 refused" sh -c "! pg_cuvs_server --socket /tmp/x.sock --index-dir $IDX --build-evict-scope idle:-1"

startd --reclaim-tick-s 1 --evict-min-idle-s 3600
psqlq "CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs;" >/dev/null
# The 0.9.0 scripts belong to I3-A; apply the I3-B block on top of 0.8.2.
sed "s#MODULE_PATHNAME#\$libdir/pg_cuvs#" sql/pg_cuvs--0.8.2--0.9.0.sql > /tmp/i3b_block.sql
su pg -c "psql -h /tmp -X -q -v ON_ERROR_STOP=1 -d postgres -f /tmp/i3b_block.sql" >/tmp/block.log 2>&1
check "I3-B SQL block installs" sh -c "! grep -q ERROR /tmp/block.log"
for t in t1 t2 t3; do
  psqlq "CREATE TABLE $t (id int, v vector(64)); INSERT INTO $t SELECT g, (SELECT array_agg(((g*31+i) % 97)::real) FROM generate_series(1,64) i)::vector FROM generate_series(1,20000) g;" >/dev/null
done
check "cli reaches daemon (memory status)" sh -c "i3b_ipc_cli $S memory | grep -q '^0'"
check "startup INFO prints the effective cache policy" grep -q 'cache policy build_evict_scope=none reclaim_mode=headroom' /tmp/d.log

# --- 1. builds never evict residents (scope none) ---
psqlq "SET client_min_messages=warning; CREATE INDEX i1 ON t1 USING cagra (v vector_l2_ops);" >/dev/null
psqlq "SET client_min_messages=warning; CREATE INDEX i2 ON t2 USING cagra (v vector_l2_ops);" >/dev/null
I1=$(oid i1); I2=$(oid i2)
check "i1 resident" sh -c "[ \"\$(i3b_ipc_cli $S index | awk -F'\t' -v i=$I1 '\$2==i{print \$3}')\" = resident ]"
check "i2 resident" sh -c "[ \"\$(i3b_ipc_cli $S index | awk -F'\t' -v i=$I2 '\$2==i{print \$3}')\" = resident ]"
check "new build: last_use 0 and file_current 1" sh -c "i3b_ipc_cli $S index | awk -F'\t' -v i=$I2 '\$2==i && \$7==0 && \$15==1{f=1} END{exit !f}'"
OUT3=$(psqlq "SET client_min_messages=warning; CREATE INDEX i3 ON t3 USING cagra (v vector_l2_ops);")
echo "  i3 build reply: $(echo "$OUT3" | head -2 | tr '\n' ' ')"
check "third build rejected, no admit_build eviction" sh -c "! grep -q 'reason=admit_build' /tmp/d.log"
check "i1,i2 still resident after the rejected build" sh -c "[ \$(i3b_ipc_cli $S index | awk -F'\t' '\$3==\"resident\"' | wc -l) -eq 2 ]"

# --- 2. headroom reclaim frees the rejected size (F) from cold indexes ---
sleep 3
check "[reclaim] trigger=headroom with f_src=build" grep -q 'trigger=headroom f_bytes=10240000 f_src=build' /tmp/d.log
check "reclaim evicted one cold index (reason=reclaim)" grep -q 'reason=reclaim requester=RECLAIM' /tmp/d.log
psqlq "SET client_min_messages=warning; DROP INDEX IF EXISTS i3; CREATE INDEX i3 ON t3 USING cagra (v vector_l2_ops);" >/dev/null
I3=$(oid i3)
check "retried build admitted after reclaim" sh -c "[ \"\$(i3b_ipc_cli $S index | awk -F'\t' -v i=$I3 '\$2==i{print \$3}')\" = resident ]"

# --- 3. reload (EV-4), soft eviction (6b), REINDEX self-replace (2a) ---
# Note: the rejected i3 was dropped and recreated with a new OID, so its
# reject record still drives F for the 30 min window (F-3 keys on the OID);
# freeze reclaim from here to observe load-demand eviction alone.
cli set reclaim-mode off >/dev/null
for i in $I2 $I3; do cli load $DB $i >/dev/null; done
check "two resident before the reload (i2, i3)" sh -c "[ \$(i3b_ipc_cli $S index | awk -F'\t' '\$3==\"resident\"' | wc -l) -eq 2 ]"
q() { psqlq "SET enable_seqscan=off; SELECT id FROM $1 ORDER BY v <-> (SELECT v FROM $1 WHERE id=1) LIMIT 1;" >/dev/null; }
q t1
check "reload of non-resident i1 evicts an eligible index (reason=reload)" grep -q 'reason=reload requester=LOAD' /tmp/d.log
check "i1 resident again with hits >= 1" sh -c "i3b_ipc_cli $S index | awk -F'\t' -v i=$I1 '\$2==i && \$3==\"resident\" && \$9>=1{f=1} END{exit !f}'"
# protect both residents by searching them (T_min 3600 s), then reload the third
for t in t1 t2 t3; do
  [ "$(col $(oid i${t#t}) state)" = resident ] && q $t
done
NR=$(i3b_ipc_cli $S index | awk -F'\t' '$3=="not_resident" && $4=="cagra"{print $2}' | head -1)
for t in t1 t2 t3; do [ "$(oid i${t#t})" = "$NR" ] && q $t; done
check "reload with only protected victims soft-evicts (reason=soft_evict)" grep -q 'reason=soft_evict requester=LOAD' /tmp/d.log
check "soft-evicted index history says soft_evict" sh -c "i3b_ipc_cli $S index | awk -F'\t' '\$20==\"soft_evict\"{f=1} END{exit !f}'"
# REINDEX of a resident index when only its old copy makes room
RI=$(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}' | head -1)
RN=$(psqlq "SELECT relname FROM pg_class WHERE oid=$RI")
psqlq "SET client_min_messages=warning; REINDEX INDEX $RN;" >/dev/null
check "REINDEX self-replace logged" grep -q '\[self_replace\] db=' /tmp/d.log
check "self-replace eviction marked self_replace=1" grep -q 'reason=admit_build requester=BUILD_CAGRA .* self_replace=1' /tmp/d.log
check "receipt: last_build_self_replace=1, unloaded>0" sh -c "i3b_ipc_cli $S index | awk -F'\t' -v i=$RI '\$2==i && \$21==1 && \$22>0{f=1} END{exit !f}'"
check "no other index evicted by the build" sh -c "! grep 'reason=admit_build' /tmp/d.log | grep -v self_replace=1 | grep -q ."
cli set reclaim-mode headroom >/dev/null

# --- 4. manual ops (C7) ---
R=$(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}' | head -1)
check "pin" sh -c "i3b_ipc_cli $S pin $DB $R | grep -q 'status=0 pinned'"
check "unload refuses a pinned index" sh -c "i3b_ipc_cli $S unload $DB $R | grep -q 'pinned: run pg_cuvs_unpin first'"
check "unpin" sh -c "i3b_ipc_cli $S unpin $DB $R | grep -q 'status=0 unpinned'"
check "unload" sh -c "i3b_ipc_cli $S unload $DB $R | grep -q 'status=0 unloaded'"
check "unloaded index listed not_resident" sh -c "i3b_ipc_cli $S index | awk -F'\t' -v i=$R '\$2==i{print \$3}' | grep -q not_resident"
check "history kept after eviction (evict_count>=1, manual_unload)" sh -c "i3b_ipc_cli $S index | awk -F'\t' -v i=$R '\$2==i && \$18>=1 && \$20==\"manual_unload\"{f=1} END{exit !f}'"
check "load" sh -c "i3b_ipc_cli $S load $DB $R | grep -q 'status=0 loaded'"
check "unpin of an unpinned index" sh -c "i3b_ipc_cli $S unpin $DB $R | grep -q 'not pinned'"

# --- 5. runtime knobs (UC-7, CK-9/CK-26) ---
check "reclaim_set mode usage" sh -c "i3b_ipc_cli $S set reclaim-mode usage | grep -q 'status=0'"
check "reclaim_set refuses restart-only knob" sh -c "! i3b_ipc_cli $S set reclaim-free-max-pct 40"
check "reclaim_set refuses low > high" sh -c "! i3b_ipc_cli $S set reclaim-low-pct 99"
check "reclaim status shows function source" sh -c "i3b_ipc_cli $S reclaim | grep -q '\"reclaim-mode\":{\"value\":\"usage\",\"source\":\"function\"'"
check "memory status mode usage, F 0" sh -c "i3b_ipc_cli $S memory | awk -F'\t' 'NR==3 && \$8==\"usage\" && \$9==0{f=1} END{exit !f}'"

# --- 6. usage stats file and startup order (UC-1 4c) ---
sleep 2
check "usage stats file written" test -s $IDX/.pg_cuvs_usage_stats
check "usage stats file not an index artifact (not_resident rows have no dot-name)" sh -c "! i3b_ipc_cli $S index | grep -q usage_stats"
startd --reclaim-tick-s 1
check "restart reads the usage stats file" grep -q 'load order: usage stats file' /tmp/d.log
printf 'garbage' > $IDX/.pg_cuvs_usage_stats
startd --reclaim-tick-s 1 --usage-stats-file off
check "usage-stats-file off: OID order" sh -c "tail -50 /tmp/d.log | grep -q 'ascending (usage stats file disabled)'"
printf 'garbage' > $IDX/.pg_cuvs_usage_stats
startd --reclaim-tick-s 1
check "corrupt file: daemon up, OID order with reason" sh -c "tail -50 /tmp/d.log | grep -q 'ascending (unknown format or version)'"
check "startup never evicts" sh -c "! grep -q 'requester=LOAD_STARTUP' /tmp/d.log"

# --- 8. SQL surface (C7, C8, CK-25/26): owner checks, superuser-only, SHOW ---
psqlq "CREATE ROLE bob LOGIN; CREATE TABLE tb (id int, v vector(64)); ALTER TABLE tb OWNER TO bob;" >/dev/null
sq() { su pg -c "psql -h /tmp -X -q -At -d postgres -v ON_ERROR_STOP=1 -c \"$1\"" 2>&1; }
sqbob() { su pg -c "psql -h /tmp -X -q -At -d postgres -U bob -c \"$1\"" 2>&1; }
RN1=$(sq "SELECT index_name FROM pg_cuvs_index_status() WHERE state='resident' ORDER BY index_oid LIMIT 1")
check "SQL index_status names the resident index ($RN1)" test -n "$RN1"
check "SQL pin/unpin/unload" sh -c "sq() { su pg -c \"psql -h /tmp -X -q -At -d postgres -c \\\"\$1\\\"\"; }; [ \"\$(sq \"SELECT pg_cuvs_pin('$RN1')\")\" = pinned ] && [ \"\$(sq \"SELECT pg_cuvs_unpin('$RN1')\")\" = unpinned ]"
sq "SELECT pg_cuvs_pin('$RN1')" >/dev/null
su pg -c "psql -h /tmp -X -At -d postgres -c \"\\\\set VERBOSITY verbose\" -c \"SELECT pg_cuvs_unload('$RN1')\"" >/tmp/pinunload.log 2>&1
check "SQL unload of a pinned index raises 55006" grep -q '55006.*pinned: run pg_cuvs_unpin first' /tmp/pinunload.log
sq "SELECT pg_cuvs_unpin('$RN1')" >/dev/null
check "non-owner unload refused" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -U bob -c \\\"SELECT pg_cuvs_unload('$RN1')\\\"\" 2>&1 | grep -q 'must be owner'"
check "non-superuser reclaim_set refused" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -U bob -c \\\"SELECT pg_cuvs_reclaim_set('evict-hit-cap','7')\\\"\" 2>&1 | grep -q 'permission denied'"
check "non-owner sees no rows of others in index_status" sh -c "[ \"\$(su pg -c \"psql -h /tmp -X -At -d postgres -U bob -c 'SELECT count(*) FROM pg_cuvs_index_status()'\")\" = 0 ]"
check "non-privileged memory_status refused" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -U bob -c 'SELECT * FROM pg_cuvs_memory_status()'\" 2>&1 | grep -q 'permission denied'"
# session A changes a knob; a different session B reads the live value
sq "SELECT pg_cuvs_reclaim_set('evict-hit-cap','7')" >/dev/null
check "cross-session SHOW sees reclaim_params change" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -c 'SHOW cuvs.rmm_pool_status'\" | grep -q '\"evict-hit-cap\":{\"value\":7,\"source\":\"function\"'"
check "memory_status via SQL has reclaim columns" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -c 'SELECT reclaim_mode, reclaim_runs >= 0 FROM pg_cuvs_memory_status()'\" | grep -q '|t'"
check "circuit_status callable by anyone" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -U bob -c 'SELECT count(*) FROM pg_cuvs_circuit_status()'\" | grep -q '^[0-9]'"

# --- 7. DROP removes history and counts drops ---
# (also forgets the index's reject records for F: unit-tested in test_cache_policy)
psqlq "DROP INDEX i2;" >/dev/null
check "drop: no row left for i2" sh -c "! i3b_ipc_cli $S index | awk -F'\t' -v i=$I2 '\$2==i' | grep -q ."

echo "--- [evict]/[reclaim] lines"
grep -E '\[evict\]|\[reclaim\]|\[self_replace\]' /tmp/d.log | sed 's/^.*\(\[[a-z_]*\]\)/\1/' | head -20
echo "FAILS=$FAILS"
[ $FAILS -eq 0 ]
