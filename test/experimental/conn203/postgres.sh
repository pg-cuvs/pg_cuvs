#!/usr/bin/env bash
set -euo pipefail
export PATH=/usr/lib/postgresql/16/bin:$PATH
D=/tmp/conn203-pg-$$
mkdir -p "$D/data" "$D/socket" "$D/indexes"
chown -R runner "$D"
runuser -u runner -- initdb -D "$D/data" -A trust --no-locale > /out/pg-init.log
cat >> "$D/data/postgresql.conf" <<EOF
shared_preload_libraries='pg_cuvs'
listen_addresses=''
port=55449
unix_socket_directories='$D/socket'
cuvs.socket_path='$D/daemon.sock'
cuvs.index_dir='$D/indexes'
EOF
runuser -u runner -- pg_ctl -D "$D/data" -l "$D/pg.log" start
runuser -u runner -- /tmp/conn203-product/pg_cuvs_server --socket "$D/daemon.sock" --index-dir "$D/indexes" > /out/pg-shim-daemon.log 2>&1 &
SERVER=$!
trap 'runuser -u runner -- pg_ctl -D "$D/data" -m fast stop >/dev/null 2>&1 || true; kill "$SERVER" 2>/dev/null || true' EXIT
sleep 1
psql -X -h "$D/socket" -p 55449 -U runner -d postgres -v ON_ERROR_STOP=1 <<'SQL'
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
-- Simulate an older development 0.9.0 catalog, then check idempotent enrollment.
ALTER EXTENSION pg_cuvs DROP FUNCTION pg_cuvs_hnsw_connectivity_status(regclass);
DROP FUNCTION pg_cuvs_hnsw_connectivity_status(regclass);
\i /src/scripts/install-connectivity-status.sql
\i /src/scripts/install-connectivity-status.sql
CREATE TABLE conn_empty(v vector(4));
CREATE INDEX conn_empty_idx ON conn_empty USING pg_cuvs_hnsw(v vector_l2_ops);
DO $$BEGIN ASSERT pg_cuvs_hnsw_connectivity_status('conn_empty_idx')->>'status'='measured';
 ASSERT (pg_cuvs_hnsw_connectivity_status('conn_empty_idx')->>'nodes')::int=0;END$$;
SELECT version();
CREATE TABLE conn_rows(id int,v vector(4));
INSERT INTO conn_rows SELECT i,ARRAY[(i/128)*100+i*0.001,(i/128)*100+i*0.001,(i/128)*100+i*0.001,(i/128)*100+i*0.001]::vector FROM generate_series(0,255)i;
CREATE INDEX conn_source ON conn_rows USING cagra(v vector_l2_ops);
CREATE INDEX conn_target ON conn_rows USING pg_cuvs_hnsw(v vector_l2_ops) WITH(source='conn_source',mode='nsw');
SELECT pg_cuvs_hnsw_build_info('conn_target');
SELECT pg_cuvs_hnsw_connectivity_status('conn_target');
DO $$DECLARE c jsonb;BEGIN
 c:=pg_cuvs_hnsw_connectivity_status('conn_target');
 ASSERT c->>'status'='measured';
 ASSERT (c->>'nodes')::int=256;
 ASSERT (c->>'unreachable')::int=128;
 ASSERT (c->>'wcc_all')::int=2;
END$$;
INSERT INTO conn_rows VALUES(256,'[1,1,1,1]');
DO $$BEGIN ASSERT (pg_cuvs_hnsw_connectivity_status('conn_target')->>'nodes')::int=256;END$$;
SET enable_cuvs=off;
SET enable_seqscan=off;
EXPLAIN (COSTS OFF) SELECT id FROM conn_rows ORDER BY v <-> '[0,0,0,0]'::vector LIMIT 1;
DO $$DECLARE got int;BEGIN SELECT id INTO got FROM conn_rows ORDER BY v <-> '[0,0,0,0]'::vector LIMIT 1;ASSERT got=0;END$$;
SQL
# Crash/recovery exercises the FPI hole boundary, not just a clean flush.
runuser -u runner -- pg_ctl -D "$D/data" -m immediate stop
runuser -u runner -- pg_ctl -D "$D/data" -l "$D/pg.log" start
psql -X -h "$D/socket" -p 55449 -U runner -d postgres -v ON_ERROR_STOP=1 <<'SQL'
DO $$DECLARE c jsonb;BEGIN
 c:=pg_cuvs_hnsw_connectivity_status('conn_target');
 ASSERT c->>'status'='measured';
 ASSERT (c->>'nodes')::int=256 AND (c->>'unreachable')::int=128;
END$$;
SELECT 'PASS SQL status/NOTICE, snapshot after INSERT, immediate recovery';
SQL
cp "$D/pg.log" /out/pg-runtime.log
