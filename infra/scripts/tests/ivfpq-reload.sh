#!/usr/bin/env bash
# Run on Linux with PostgreSQL 16 and the vector/pg_cuvs extensions installed.
# SERVER_BIN can point to either revision's freshly built daemon.
# Pass --shim with a CPU shim daemon to also exercise low physical headroom.
set -euo pipefail

server=${SERVER_BIN:-./pg_cuvs_server}
estimate_bin=${IVFPQ_ESTIMATE_BIN:-./test-ivfpq-resident-format}
tmp=$(mktemp -d /tmp/pgcuvs-ivfpq-reload.XXXXXX)
socket=$tmp/daemon.sock
indexes=$tmp/indexes
daemon_pid=
pg_started=0

stop_daemon() {
    if [[ -n "$daemon_pid" ]]; then
        kill "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" || true
        daemon_pid=
    fi
}

cleanup() {
    local rc=$?
    if (( rc != 0 )); then
        for log in "$tmp"/*.log; do
            if [[ -f "$log" ]]; then tail -n 60 "$log" >&2; fi
        done
    fi
    stop_daemon
    if (( pg_started )); then pg_ctl -D "$tmp/pgdata" -m immediate -w stop >/dev/null; fi
    rm -rf "$tmp"
}
trap cleanup EXIT

start_daemon() {
    local budget_mb=$1 log=$2 i
    "$server" --socket "$socket" --index-dir "$indexes" --gpu-devices 0 \
        --max-vram-mb "$budget_mb" --rmm-pool-mb 0 --reclaim-mode off >"$log" 2>&1 &
    daemon_pid=$!
    for ((i=0; i<600; i++)); do
        if grep -qF "listening on $socket" "$log"; then return; fi
        kill -0 "$daemon_pid" || return 1
        sleep 0.5
    done
    echo "daemon did not begin listening" >&2
    return 1
}

sql() {
    PGOPTIONS="-c cuvs.socket_path=$socket -c cuvs.index_dir=$indexes" \
        psql -XqAt -v ON_ERROR_STOP=1 -h "$tmp" -p 5432 -U "$(id -un)" -d postgres -c "$1"
}

mkdir "$indexes"
initdb -D "$tmp/pgdata" --no-locale --auth=trust >"$tmp/initdb.log" 2>&1
pg_ctl -D "$tmp/pgdata" -l "$tmp/postgres.log" -w \
    -o "-k $tmp -p 5432 -c listen_addresses='' -c shared_preload_libraries=pg_cuvs" start >/dev/null
pg_started=1

start_daemon 4096 "$tmp/build.log"
sql 'CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs;
     CREATE TABLE pq_items (id int, v vector(128));
     SELECT setseed(0.42);
     INSERT INTO pq_items SELECT g, array_agg(random()::real ORDER BY i)::vector
       FROM generate_series(1, 8192) g CROSS JOIN generate_series(1, 128) i GROUP BY g;
     CREATE INDEX pq_idx ON pq_items USING ivfpq (v vector_l2_ops)
       WITH (n_lists = 32, pq_bits = 8, pq_dim = 16);' >/dev/null
# The vector addition has no result typmod, so this is a legacy expression
# index even when the extension is built with CUVS_PAGE_FORMAT=1.
sql 'CREATE INDEX cagra_idx ON pq_items USING cagra ((v + v) vector_l2_ops)
       WITH (graph_degree = 32, intermediate_graph_degree = 64);' >/dev/null
db_oid=$(sql 'SELECT oid FROM pg_database WHERE datname = current_database()')
pq_oid=$(sql "SELECT 'pq_idx'::regclass::oid")
cagra_oid=$(sql "SELECT 'cagra_idx'::regclass::oid")
artifact=$indexes/${db_oid}_${pq_oid}.ivfpq
if [[ ! -s "$indexes/${db_oid}_${cagra_oid}.cagra" ]]; then
    echo "CAGRA control did not create a legacy artifact" >&2
    exit 2
fi
stop_daemon

artifact_bytes=$(stat -c '%s' "$artifact")
needed=$("$estimate_bin" "$artifact")
cagra_bytes=$((8192 * (4 * 128 + 4 * 64)))
budget_mb=5
budget_bytes=$((budget_mb * 1024 * 1024))
echo "artifact=$artifact_bytes needed=$needed budget=$budget_bytes cagra_estimate=$cagra_bytes"
if ! (( artifact_bytes > 0 && needed <= budget_bytes && budget_bytes < cagra_bytes )); then
    echo "fixture does not distinguish the estimates" >&2
    exit 2
fi

log=$tmp/reload.log
start_daemon "$budget_mb" "$log"
if sql "SELECT pg_cuvs_load('cagra_idx')"; then
    echo "CAGRA load exceeded the established estimate" >&2
    exit 1
fi
grep -E "op=LOAD path=load .*idx=$cagra_oid algo=cagra .*gd=64 igd=128 final=$cagra_bytes .*reason=AR0 " "$log"

failed=0
query_vector="[$(printf '0,%.0s' {1..127})0]"
if result=$(sql "SET enable_seqscan=off; SET cuvs.ivfpq_n_probes=32; SET cuvs.k=4;
    SELECT count(*) = 1 AND bool_and(id BETWEEN 1 AND 8192) FROM
      (SELECT id FROM pq_items ORDER BY v <-> '$query_vector'::vector LIMIT 1) s;") &&
    [[ "$result" == t ]]; then
    [[ $(sql "SELECT search_count > 0 AND search_mode = 'ivfpq' FROM pg_stat_gpu_search WHERE index_oid = $pq_oid") == t ]]
    [[ $(sql "SELECT pg_cuvs_unload('pq_idx')") == unloaded ]]
else
    echo "IVF-PQ cold search failed after restart" >&2
    failed=1
fi

if result=$(sql "SELECT pg_cuvs_load('pq_idx')") && [[ "$result" == loaded ]]; then
    grep -E "op=LOAD path=load .*idx=$pq_oid algo=ivf_pq .*gd=0 igd=0 final=$needed scratch=0 needed=$needed .*verdict=admit reason=none " "$log"
    [[ $(sql "SELECT state = 'resident' AND kind = 'ivfpq' AND vram_bytes = $needed
      FROM pg_cuvs_index_status() WHERE db_oid = $db_oid AND index_oid = $pq_oid") == t ]]
else
    echo "IVF-PQ manual LOAD failed" >&2
    failed=1
fi
(( failed == 0 )) || exit 1

[[ $(sql "SELECT pg_cuvs_unload('pq_idx')") == unloaded ]]
mv "$artifact" "$artifact.saved"
for fixture in missing empty directory unreadable corrupt; do
    case "$fixture" in
        empty) touch "$artifact" ;;
        directory) rm "$artifact"; mkdir "$artifact" ;;
        unreadable) rmdir "$artifact"; cp "$artifact.saved" "$artifact"; chmod 000 "$artifact" ;;
        corrupt) rm "$artifact"; printf 'bad' >"$artifact" ;;
    esac
    if sql "SELECT pg_cuvs_load('pq_idx')" >"$tmp/file-error.log" 2>&1; then
        echo "IVF-PQ loaded with a $fixture artifact" >&2
        exit 1
    fi
    grep -F 'not loaded: no usable index files' "$tmp/file-error.log"
done

mv -f "$artifact.saved" "$artifact"
[[ $(sql "SELECT pg_cuvs_load('pq_idx')") == loaded ]]

if [[ ${1:-} == --shim ]]; then
    stop_daemon
    (( needed > 1024 * 1024 ))
    log=$tmp/physical.log
    CUVS_SHIM_VRAM_MB=1 start_daemon "$budget_mb" "$log"
    if sql "SELECT pg_cuvs_load('pq_idx')"; then
        echo "IVF-PQ loaded despite insufficient physical headroom" >&2
        exit 1
    fi
    grep -E "op=LOAD path=load .*idx=$pq_oid algo=ivf_pq .*needed=$needed .*reason=AR4 " "$log"
fi

echo "PASS: IVF-PQ cold search, manual LOAD, file refusals, and CAGRA admission"
