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

make PGCUVS_CPU_SHIM=1 with_llvm=no -j8 all server server-test >/tmp/b.log 2>&1 || { tail -20 /tmp/b.log; exit 2; }
make PGCUVS_CPU_SHIM=1 with_llvm=no install >/tmp/i.log 2>&1 || { tail -20 /tmp/i.log; exit 2; }
install -m 755 pg_cuvs_server pg_cuvs_server_test /usr/local/bin/
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
DBIN=pg_cuvs_server
# Integration fixture (test only, not a policy default): this scenario's byte
# arithmetic assumes N = final (two 10 MiB indexes in --max-vram-mb 25). On
# the I3 stack's Q1-derived scratch rho also reserves build scratch, so pin
# rho = 0 for every daemon this final-footprint-only scenario starts.
FIXTURE="--rmm-scratch-permille 0"
startd() { # extra daemon flags...
    pkill -f pg_cuvs_server 2>/dev/null; sleep 1; pkill -9 -f pg_cuvs_server 2>/dev/null
    rm -f $S
    su pg -c "$DBIN --socket $S --index-dir $IDX --max-vram-mb 25 $FIXTURE $* >>/tmp/d.log 2>&1 &"
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
# 0.9.0 (I3-A's scripts with the I3-B block) is the default version now.
check "extension installs at 0.9.0 with the I3-B functions" sh -c "[ \"\$(su pg -c \"psql -h /tmp -X -At -d postgres -c \\\"SELECT extversion FROM pg_extension WHERE extname='pg_cuvs'\\\"\")\" = 0.9.0 ] && su pg -c \"psql -h /tmp -X -At -d postgres -c \\\"SELECT count(*) FROM pg_proc WHERE proname IN ('pg_cuvs_unload','pg_cuvs_load','pg_cuvs_pin','pg_cuvs_unpin','pg_cuvs_reclaim_set','pg_cuvs_index_status','pg_cuvs_memory_status','pg_cuvs_circuit_status')\\\"\" | grep -qx 10"
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
check "reclaim evicted one cold index (reason=reclaim)" grep -qE 'op=RECLAIM .*reason=reclaim requester_idx=0' /tmp/d.log
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
check "reload of non-resident i1 evicts an eligible index (reason=reload)" grep -qE 'op=LOAD .*reason=reload requester_idx=[1-9]' /tmp/d.log
check "i1 resident again with hits >= 1" sh -c "i3b_ipc_cli $S index | awk -F'\t' -v i=$I1 '\$2==i && \$3==\"resident\" && \$9>=1{f=1} END{exit !f}'"
# protect both residents by searching them (T_min 3600 s), then reload the third
for t in t1 t2 t3; do
  [ "$(col $(oid i${t#t}) state)" = resident ] && q $t
done
NR=$(i3b_ipc_cli $S index | awk -F'\t' '$3=="not_resident" && $4=="cagra"{print $2}' | head -1)
for t in t1 t2 t3; do [ "$(oid i${t#t})" = "$NR" ] && q $t; done
check "reload with only protected victims soft-evicts (reason=soft_evict)" grep -qE 'op=LOAD .*reason=soft_evict' /tmp/d.log
check "soft-evicted index history says soft_evict" sh -c "i3b_ipc_cli $S index | awk -F'\t' '\$20==\"soft_evict\"{f=1} END{exit !f}'"
# REINDEX of a resident index when only its old copy makes room
RI=$(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}' | head -1)
RN=$(psqlq "SELECT relname FROM pg_class WHERE oid=$RI")
psqlq "SET client_min_messages=warning; REINDEX INDEX $RN;" >/dev/null
check "REINDEX self-replace logged" grep -q '\[self_replace\] db=' /tmp/d.log
check "self-replace NOTICE and DETAIL exact (need 10240000, free 5734400)" grep -qF "notice=\"pg_cuvs: previous resident copy of index $RI unloaded from GPU to make room for the rebuild\" detail=\"GPU memory was insufficient for both copies (need 10240000 bytes, free 5734400 bytes). Queries on this index use the CPU path or reload the old copy until the new index is resident\"" /tmp/d.log
check "self-replace eviction marked self_replace=1" grep -qE 'op=BUILD_CAGRA .*reason=admit_build .*self_replace=1' /tmp/d.log
check "receipt: last_build_self_replace=1, unloaded>0" sh -c "i3b_ipc_cli $S index | awk -F'\t' -v i=$RI '\$2==i && \$21==1 && \$22>0{f=1} END{exit !f}'"
check "no other index evicted by the build" sh -c "! grep 'reason=admit_build' /tmp/d.log | grep -v self_replace=1 | grep -q ."
cli set reclaim-mode headroom >/dev/null

# --- 4. manual ops (C7) ---
cli set reclaim-mode off >/dev/null   # no background reclaim racing the manual ops
R=$(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}' | head -1)
check "pin" sh -c "i3b_ipc_cli $S pin $DB $R | grep -q 'status=0 pinned'"
R2=$(i3b_ipc_cli $S index | awk -F'\t' -v r=$R '$3=="resident" && $2!=r{print $2}' | head -1)
check "pin cap: second pin exceeds --pin-max-pct 50" sh -c "[ -n '$R2' ] && i3b_ipc_cli $S pin $DB $R2 | grep -q 'pin limit'"
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

# --- 5b. I-FC transitions and defect 3 (CK-37 (b), CK-26 r14) ---
cli set reclaim-mode off >/dev/null
for i in $(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}'); do cli unload $DB $i >/dev/null; done
cli load $DB $I1 >/dev/null
check "after load: file_current 1, extend_count 0" sh -c "[ \"\$(i3b_ipc_cli $S index | awk -F'\t' -v i=$I1 '\$2==i{print \$15\":\"\$16}')\" = 1:0 ]"
psqlq "INSERT INTO t1 SELECT 90000 + id, v FROM t1 WHERE id <= 10;" >/dev/null
check "EXTEND: file_current 0, extend_count 10" sh -c "[ \"\$(i3b_ipc_cli $S index | awk -F'\t' -v i=$I1 '\$2==i{print \$15\":\"\$16}')\" = 0:10 ]"
check "unload after EXTEND saves (no sidecar: plain 'unloaded')" sh -c "i3b_ipc_cli $S unload $DB $I1 | grep -q 'status=0 unloaded\$'"
check "the unload saved the graph (saved_ms field, manual_unload)" grep -qE "op=MANUAL .* idx=$I1 reason=manual_unload .*saved_ms=" /tmp/d.log
cli load $DB $I1 >/dev/null
check "reload resets extend_count (defect 3) and file_current 1" sh -c "[ \"\$(i3b_ipc_cli $S index | awk -F'\t' -v i=$I1 '\$2==i{print \$15\":\"\$16}')\" = 1:0 ]"
check "unload with file_current 1 skips the save (saved_ms=0)" sh -c "i3b_ipc_cli $S unload $DB $I1 >/dev/null; grep -E 'op=MANUAL .* idx=$I1 ' /tmp/d.log | tail -1 | grep -q 'saved_ms=0 '"

# --- 5c. sidecar exclusion in background reclaim (decision 4, UC-1 7b) ---
cli load $DB $I1 >/dev/null
psqlq "INSERT INTO t1 SELECT 91000 + id, v FROM t1 WHERE id <= 5;" >/dev/null
touch $IDX/${DB}_${I1}.delta
cli load $DB $I3 >/dev/null
cli set reclaim-free-bytes 26214400 >/dev/null
cli set reclaim-mode headroom >/dev/null
sleep 3
check "reclaim skips the unsaved CAGRA with a delta file (skip_sidecar=1)" grep -qE '\[reclaim\] .*evicted=1 skip_sidecar=1 trigger=headroom f_bytes=13107200 f_src=fixed' /tmp/d.log
check "skipped index stays resident; the other was reclaimed" sh -c "[ \"\$(i3b_ipc_cli $S index | awk -F'\t' -v i=$I1 '\$2==i{print \$3\$14}')\" = residentdelta ]"
check "memory_status counts reclaim_skip_sidecar" sh -c "i3b_ipc_cli $S memory | awk -F'\t' 'NR==3 && \$14>=1{f=1} END{exit !f}'"
cli set reclaim-mode off >/dev/null
cli set reclaim-free-bytes 0 >/dev/null
check "unload of that index warns (CPU replan until REINDEX)" sh -c "i3b_ipc_cli $S unload $DB $I1 | grep -q 'unloaded; warning: delta or tombstone file present, GPU path switches to CPU replan until REINDEX'"
rm -f $IDX/${DB}_${I1}.delta
cli set reclaim-mode usage >/dev/null

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
check "startup never evicts (no [evict] between load order and loaded lines)" sh -c "! awk '/\[startup\] load order/{f=1} /\[startup\] loaded/{f=0} f && /\[evict\]/{e=1} END{exit !e}' /tmp/d.log"
check "startup left an index not resident (capacity bound reached)" grep -q 'left not resident' /tmp/d.log

# --- 6b. knobs off reproduce the old behavior (CK-9 r18) ---
startd --reclaim-tick-s 1 --evict-min-idle-s 3600 --reclaim-mode off --load-soft-evict off --reindex-self-replace off
for i in $(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}'); do cli unload $DB $i >/dev/null; done
cli load $DB $I1 >/dev/null; cli load $DB $I3 >/dev/null
q t1; q t3
MARK=$(wc -l < /tmp/d.log)
OUTL=$(cli load $DB $I2)
echo "  soft-evict-off load reply: $OUTL"
# Integration rebind (confirmed by i3-b, xsm 5c064e99): on the I3 stack a
# manual LOAD refused by admission goes through I3-A's structured reply
# (status 2, err_out reason AR5; plan C5 reload variant), not the interim I3-B
# C7 string "...pinned, in use, or still protected".
check "load-soft-evict off: reload with only protected victims refused (status 2, err_out AR5)" sh -c "echo '$OUTL' | grep -q '^status=2 ' && tail -n +$MARK /tmp/d.log | grep -q '\[admit_reject\] status=2 err_out=\"AR5 '"
check "load-soft-evict off: no soft_evict" sh -c "! tail -n +$MARK /tmp/d.log | grep -q 'reason=soft_evict'"
RN3=$(psqlq "SELECT relname FROM pg_class WHERE oid=$I3")
psqlq "SET client_min_messages=warning; REINDEX INDEX $RN3;" > /tmp/reidx_off.log 2>&1
check "reindex-self-replace off: no self-replace, REINDEX refused" sh -c "! tail -n +$MARK /tmp/d.log | grep -q 'self_replace\]' && grep -q ERROR /tmp/reidx_off.log"
rm -f $IDX/.pg_cuvs_usage_stats; sleep 3
check "usage stats file written even with --reclaim-mode off" test -s $IDX/.pg_cuvs_usage_stats
# pinned REINDEX keeps the pin when there is room
startd --reclaim-tick-s 1 --reclaim-mode off
for i in $(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}'); do cli unload $DB $i >/dev/null; done
cli pin $DB $I3 >/dev/null
psqlq "SET client_min_messages=warning; REINDEX INDEX $RN3;" >/dev/null
check "REINDEX keeps the pin (C7 7.2-N3)" sh -c "[ \"\$(i3b_ipc_cli $S index | awk -F'\t' -v i=$I3 '\$2==i{print \$10}')\" = 1 ]"
cli unpin $DB $I3 >/dev/null
startd --reclaim-tick-s 1

# --- 7b. reload while another build holds its reservation (R1, r18 B-9) ---
startd --reclaim-mode off
for i in $(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}'); do cli unload $DB $i >/dev/null; done
cli load $DB $I3 >/dev/null
pkill -f pg_cuvs_server; sleep 1; pkill -9 -f pg_cuvs_server 2>/dev/null; rm -f $S
su pg -c "CUVS_SHIM_BUILD_DELAY_MS=6000 pg_cuvs_server --socket $S --index-dir $IDX --max-vram-mb 25 $FIXTURE --reclaim-mode off >>/tmp/d.log 2>&1 &"
for i in $(seq 1 30); do [ -S $S ] && break; sleep 1; done
for i in $(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}'); do [ "$i" = "$I3" ] || cli unload $DB $i >/dev/null; done
cli load $DB $I3 >/dev/null
MARK=$(wc -l < /tmp/d.log)
psqlq "CREATE TABLE t4 (id int, v vector(64)); INSERT INTO t4 SELECT g, (SELECT array_agg(((g*7+i) % 89)::real) FROM generate_series(1,64) i)::vector FROM generate_series(1,20000) g;" >/dev/null
(psqlq "SET client_min_messages=warning; CREATE INDEX i4 ON t4 USING cagra (v vector_l2_ops);" > /tmp/i4.log 2>&1 &)
sleep 2
q t1
wait 2>/dev/null; sleep 6
check "reload during a pending build evicts a resident (build itself does not)" sh -c "tail -n +$MARK /tmp/d.log | grep -qE 'op=LOAD .*reason=(reload|soft_evict)' && ! tail -n +$MARK /tmp/d.log | grep -q 'reason=admit_build'"
check "the delayed build still completes" sh -c "! grep -q ERROR /tmp/i4.log"
startd --reclaim-tick-s 1

# --- 9. every build path keeps scope none (review BLOCK 4) and the
#        self-replace failure notice (BLOCK 2), with build-OOM injection ---
noevict_since() { ! tail -n +$1 /tmp/d.log | grep -q '\[evict\]'; }
resident_n() { i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"' | wc -l; }
DBIN=pg_cuvs_server_test
startd --reclaim-mode off
for i in $(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}'); do cli unload $DB $i >/dev/null; done
cli load $DB $I1 >/dev/null; cli load $DB $I3 >/dev/null
psqlq "CREATE TABLE t5 (id int, v vector(64)); INSERT INTO t5 SELECT g, (SELECT array_agg(((g*13+i) % 83)::real) FROM generate_series(1,64) i)::vector FROM generate_series(1,120000) g;" >/dev/null
psqlq "CREATE TABLE t6 (id int, v vector(64)); INSERT INTO t6 SELECT g, (SELECT array_agg(((g*17+i) % 79)::real) FROM generate_series(1,64) i)::vector FROM generate_series(1,20000) g;" >/dev/null
MARK=$(wc -l < /tmp/d.log)
psqlq "SET client_min_messages=warning; CREATE INDEX i5 ON t5 USING ivfpq (v vector_l2_ops);" > /tmp/i5.log 2>&1
check "IVF-PQ build that does not fit evicts nothing (BUILD_IVFPQ, scope none)" sh -c "grep -q ERROR /tmp/i5.log && ! tail -n +$MARK /tmp/d.log | grep -q '\[evict\]' && [ \$(i3b_ipc_cli $S index | awk -F'\t' '\$3==\"resident\"' | wc -l) -eq 2 ]"
MARK=$(wc -l < /tmp/d.log)
# 40,000 rows in 2 shards: each shard (10,240,000 B) exceeds the free room
psqlq "CREATE TABLE t7 (id int, v vector(64)); INSERT INTO t7 SELECT g, (SELECT array_agg(((g*19+i) % 73)::real) FROM generate_series(1,64) i)::vector FROM generate_series(1,40000) g;" >/dev/null
MARK=$(wc -l < /tmp/d.log)
psqlq "SET client_min_messages=warning; SET cuvs.shard_count = 2; CREATE INDEX i7s ON t7 USING cagra (v vector_l2_ops);" > /tmp/i6s.log 2>&1
check "sharded build that does not fit evicts nothing (BUILD_SHARD)" sh -c "grep -q ERROR /tmp/i6s.log && ! tail -n +$MARK /tmp/d.log | grep -q '\[evict\]' && [ \$(i3b_ipc_cli $S index | awk -F'\t' '\$3==\"resident\"' | wc -l) -eq 2 ]"
# post-admission OOM retry (BUILD_CAGRA): room exists, the build OOMs once
startd --reclaim-mode off --max-vram-mb 100
for i in $(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}'); do cli unload $DB $i >/dev/null; done
cli load $DB $I1 >/dev/null; cli load $DB $I3 >/dev/null
MARK=$(wc -l < /tmp/d.log)
su pg -c "psql -h /tmp -X -q -At -v VERBOSITY=verbose -d postgres -c \"SET client_min_messages=warning; SELECT pg_cuvs_inject_build_oom(1); CREATE INDEX i6o ON t6 USING cagra (v vector_l2_ops);\"" > /tmp/i6o.log 2>&1
echo "  post-admission OOM reply: $(grep ERROR /tmp/i6o.log | head -1)"
# Integration: ADR-086 (I3-A) removed the evict-to-fit retry. The contract is
# "OOM after admission fails without evicting": OOM_FALLBACK (status 2, reason
# OOM -> SQLSTATE 53200) and one [build_oom] action=fail_no_evict line.
check "post-admission build OOM evicts nothing" sh -c "grep -q ERROR /tmp/i6o.log && ! tail -n +$MARK /tmp/d.log | grep -q '\[evict\]'"
check "post-admission build OOM: [build_oom] action=fail_no_evict" sh -c "tail -n +$MARK /tmp/d.log | grep -q '\[build_oom\] op=BUILD .*action=fail_no_evict'"
check "post-admission build OOM: status 2 reported as SQLSTATE 53200" grep -q 'ERROR:  53200' /tmp/i6o.log
psqlq "SELECT pg_cuvs_inject_build_oom(0);" >/dev/null
# registry slot cap (BUILD_CAGRA): fits in VRAM, no slot -> saved, not resident
startd --reclaim-mode off --max-vram-mb 100 --max-indexes 2
for i in $(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}'); do cli unload $DB $i >/dev/null; done
cli load $DB $I1 >/dev/null; cli load $DB $I3 >/dev/null
MARK=$(wc -l < /tmp/d.log)
psqlq "SET client_min_messages=warning; CREATE INDEX i6c ON t6 USING cagra (v vector_l2_ops);" > /tmp/i6c.log 2>&1
I6C=$(oid i6c)
check "build at the registry cap evicts nothing; saved, not resident" sh -c "! grep -q ERROR /tmp/i6c.log && ! tail -n +$MARK /tmp/d.log | grep -q '\[evict\]' && [ \"\$(i3b_ipc_cli $S index | awk -F'\t' -v i=$I6C '\$2==i{print \$3}')\" = not_resident ]"
# self-replace, then the rebuild OOMs: failure notice (exact)
startd --reclaim-mode off
for i in $(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}'); do cli unload $DB $i >/dev/null; done
cli load $DB $I1 >/dev/null; cli load $DB $I3 >/dev/null
psqlq "SET client_min_messages=warning; SELECT pg_cuvs_inject_build_oom(1); REINDEX INDEX $RN3;" > /tmp/reidx_fail.log 2>&1
check "self-replace then build failure: failure NOTICE exact" grep -qF "[self_replace] idx=$I3 build_failed=1 notice=\"pg_cuvs: rebuild of index $I3 failed after its previous resident copy was unloaded from GPU\" detail=\"The next query on this index reloads the old copy\"" /tmp/d.log
psqlq "SELECT pg_cuvs_inject_build_oom(0);" >/dev/null

# --- 7b. manual LOAD / PIN refused by admission, over SQL (C5 reload variant) ---
# The err_out of a manual op reaches the backend, which raises the C5 reload
# variant (plan C5: 53200 for every reason on the reload path, DETAIL with the
# reason values, HINT) and records it for pg_cuvs_last_rejection().
# i1 and i3 resident and pinned, i6c not resident.
startd --reclaim-mode off --pin-max-pct 100
for i in $(i3b_ipc_cli $S index | awk -F'\t' '$3=="resident"{print $2}'); do cli unload $DB $i >/dev/null; done
cli load $DB $I1 >/dev/null; cli load $DB $I3 >/dev/null; cli pin $DB $I1 >/dev/null; cli pin $DB $I3 >/dev/null
sqv() { su pg -c "psql -h /tmp -X -q -At -v VERBOSITY=verbose -d postgres -c \"$1\" -c \"SELECT 'last=' || (pg_cuvs_last_rejection()->>'reason')\"" 2>&1; }
sqv "SELECT pg_cuvs_load('i6c'::regclass)" > /tmp/load_ar5.log
sqv "SELECT pg_cuvs_pin('i6c'::regclass)" > /tmp/pin_ar5.log
echo "  load AR5 reply: $(tr '\n' ' ' < /tmp/load_ar5.log | cut -c1-400)"
check "pg_cuvs_load AR5: SQLSTATE 53200 and the C5 reload message" grep -q 'ERROR:  53200: pg_cuvs: GPU memory is unavailable to load index "public.i6c"' /tmp/load_ar5.log
check "pg_cuvs_load AR5: DETAIL and HINT present" sh -c "grep -q '^DETAIL:  ' /tmp/load_ar5.log && grep -q '^HINT:  ' /tmp/load_ar5.log"
check "pg_cuvs_load AR5: pg_cuvs_last_rejection reason AR5" grep -qx 'last=AR5' /tmp/load_ar5.log
check "pg_cuvs_pin AR5: SQLSTATE 53200 and the C5 reload message" grep -q 'ERROR:  53200: pg_cuvs: GPU memory is unavailable to load index "public.i6c"' /tmp/pin_ar5.log
startd --reclaim-mode off --max-vram-mb 9
sqv "SELECT pg_cuvs_load('i6c'::regclass)" > /tmp/load_ar0.log
echo "  load AR0 reply: $(tr '\n' ' ' < /tmp/load_ar0.log | cut -c1-400)"
check "pg_cuvs_load AR0: SQLSTATE 53200 (reload variant) and the C5 reload message" grep -q 'ERROR:  53200: pg_cuvs: GPU memory is unavailable to load index "public.i6c"' /tmp/load_ar0.log
check "pg_cuvs_load AR0: DETAIL names the per-index limit, HINT present, last_rejection AR0" sh -c "grep -q '^DETAIL:  .*the limit for one index is' /tmp/load_ar0.log && grep -q '^HINT:  The index is larger than the GPU memory limit' /tmp/load_ar0.log && grep -qx 'last=AR0' /tmp/load_ar0.log"
check "pg_cuvs_load AR5: DETAIL names the pinned bytes, HINT names pg_cuvs_unpin" sh -c "grep -q '^DETAIL:  .*pinned 19 MiB in 2' /tmp/load_ar5.log && grep -q '^HINT:  .*pg_cuvs_unpin' /tmp/load_ar5.log"
DBIN=pg_cuvs_server
startd --reclaim-tick-s 1

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
# session B reads SHOW, session A changes a knob, the same session B reads again
cat > /tmp/xsess.sql <<'EOSQL'
SHOW cuvs.rmm_pool_status;
\! psql -h /tmp -X -q -At -d postgres -c "SELECT pg_cuvs_reclaim_set('evict-hit-cap','7')"
SHOW cuvs.rmm_pool_status;
EOSQL
sq "SELECT pg_cuvs_reclaim_set('evict-hit-cap','20')" >/dev/null
su pg -c "psql -h /tmp -X -q -At -d postgres -f /tmp/xsess.sql" > /tmp/xsess.out 2>&1
check "same session B sees A's reclaim_params change (no per-session cache)" sh -c "head -1 /tmp/xsess.out | grep -q '\"evict-hit-cap\":{\"value\":20' && tail -1 /tmp/xsess.out | grep -q '\"evict-hit-cap\":{\"value\":7,\"source\":\"function\"'"
check "memory_status via SQL has reclaim columns" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -c 'SELECT reclaim_mode, reclaim_runs >= 0 FROM pg_cuvs_memory_status()'\" | grep -q '|t'"
check "circuit_status callable by anyone" sh -c "su pg -c \"psql -h /tmp -X -At -d postgres -U bob -c 'SELECT count(*) FROM pg_cuvs_circuit_status()'\" | grep -q '^[0-9]'"

# --- 7. DROP removes history and counts drops ---
# (also forgets the index's reject records for F: unit-tested in test_cache_policy)
psqlq "DROP INDEX i2;" >/dev/null
check "drop: no row left for i2" sh -c "! i3b_ipc_cli $S index | awk -F'\t' -v i=$I2 '\$2==i' | grep -q ."

# CK-10 (I3-B part): ADR-086 field counts, 13 each, in schema order
check "[evict] lines have the 13 ADR-086 fields in order" sh -c "grep '\[evict\]' /tmp/d.log | sed 's/.*\[evict\] //' | awk '{n=split(\$0,f,\" \"); s=\"\"; for(i=1;i<=n;i++){split(f[i],kv,\"=\"); s=s kv[1] \",\"} if (s != \"op,gpu,db,idx,reason,requester_idx,idle_s,protect_s,hits,vram_bytes,saved_ms,mono_us,self_replace,\") bad=1} END{exit bad}'"
check "[reclaim] lines have the 13 ADR-086 fields in order" sh -c "grep '\[reclaim\] op=' /tmp/d.log | sed 's/.*\[reclaim\] //' | awk '{n=split(\$0,f,\" \"); s=\"\"; for(i=1;i<=n;i++){split(f[i],kv,\"=\"); s=s kv[1] \",\"} if (s != \"op,gpu,tick,resident_before,resident_after,hi_bytes,lo_bytes,evicted,skip_sidecar,trigger,f_bytes,f_src,mono_us,\") bad=1} END{exit bad}'"
echo "--- cross-session SHOW"; cat /tmp/xsess.out | cut -c1-200
echo "--- [evict]/[reclaim] lines"
grep -E '\[evict\]|\[reclaim\]|\[self_replace\]' /tmp/d.log | sed 's/^.*\(\[[a-z_]*\]\)/\1/' | head -20
echo "FAILS=$FAILS"
[ $FAILS -eq 0 ]
