-- Admission errors consume the live daemon coefficient, not a frozen default.
-- The full diagnostics are logged unchanged; the client comparison normalizes
-- only numeric DETAIL text and separately checks the independently derived bytes.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET max_parallel_maintenance_workers = 0;

CREATE FUNCTION pg_temp.ar_expect(cmd text, reason text, state text,
    requester text, need_mib bigint, limit_mib bigint, shard integer DEFAULT NULL)
RETURNS TABLE(err_message text, err_detail text, err_hint text, contract_ok boolean)
LANGUAGE plpgsql AS $$
DECLARE
    actual_state text;
    actual_detail text;
    r jsonb;
BEGIN
    BEGIN
        EXECUTE cmd;
        RAISE EXCEPTION 'expected admission refusal, command succeeded';
    EXCEPTION WHEN OTHERS THEN
        GET STACKED DIAGNOSTICS actual_state = RETURNED_SQLSTATE,
            err_message = MESSAGE_TEXT, actual_detail = PG_EXCEPTION_DETAIL,
            err_hint = PG_EXCEPTION_HINT;
        r := pg_cuvs_last_rejection();
        contract_ok := actual_state = state AND r->>'reason' = reason
            AND r->>'sqlstate' = state AND r->>'requester' = requester
            AND (r->'fields'->>'need_mib')::bigint = need_mib
            AND (r->'fields'->>'quota_mib')::bigint = limit_mib
            AND (r->>'shard')::integer IS NOT DISTINCT FROM shard
            AND position(format('needs %s MiB', need_mib) IN actual_detail) > 0;
        RAISE LOG 'I3_ERROR_SNAPSHOT state=% message=% detail=% hint=%',
            actual_state, err_message, actual_detail, err_hint;
        err_detail := regexp_replace(actual_detail, '[0-9]+', '<n>', 'g');
        RETURN NEXT;
    END;
END
$$;

SELECT pg_cuvs_last_rejection() IS NULL AS no_rejection_yet;
SELECT (current_setting('cuvs.rmm_pool_status')::jsonb->>'scratch_permille')::bigint AS rho \gset

CREATE TABLE ar_items (id int, embedding vector(8)) WITH (autovacuum_enabled=false);
INSERT INTO ar_items
SELECT g, format('[%s,%s,0,0,0,0,0,1]', g, g % 7)::vector FROM generate_series(1, 1000) g;
-- Half the final footprint refuses for every supported coefficient, even zero.
SELECT pg_cuvs_set_vram_budget(144000);
SELECT * FROM pg_temp.ar_expect(
    'CREATE INDEX ar_cagra ON ar_items USING cagra (embedding vector_l2_ops)',
    'AR0','54000','BUILD',(288000::bigint+(288000::bigint*:rho+999)/1000)/1048576,0);

SELECT pg_cuvs_set_vram_budget(4096);
SELECT * FROM pg_temp.ar_expect(
    'CREATE INDEX ar_ivfpq ON ar_items USING ivfpq (embedding vector_l2_ops)',
    'AR0','54000','BUILD',0,0);
SELECT (r->>'index_oid')::oid <> 0 AS has_index_oid FROM pg_cuvs_last_rejection() r;

CREATE TABLE ar_shard (id int, embedding vector(8)) WITH (autovacuum_enabled=false);
INSERT INTO ar_shard
SELECT g, format('[%s,%s,0,0,0,0,1,0]', g, g % 11)::vector FROM generate_series(1, 4000) g;
SET cuvs.shard_count = 2;
SELECT pg_cuvs_set_vram_budget(288000);
SELECT * FROM pg_temp.ar_expect(
    'CREATE INDEX ar_sharded ON ar_shard USING cagra (embedding vector_l2_ops)',
    'AR0','54000','SHARD',(576000::bigint+(576000::bigint*:rho+999)/1000)/1048576,0,0);
RESET cuvs.shard_count;

-- Explicit unload arranges a reload miss without bypassing build scope none.
SELECT pg_cuvs_set_vram_budget(0);
CREATE TABLE ar_a (id int, embedding vector(8));
INSERT INTO ar_a SELECT g, format('[%s,0,0,0,0,0,0,1]', g)::vector FROM generate_series(1,1000) g;
CREATE INDEX ar_a_idx ON ar_a USING cagra (embedding vector_l2_ops);
SELECT pg_cuvs_unload('ar_a_idx');
SELECT pg_cuvs_set_vram_budget(100000);
SET enable_seqscan = off;
SELECT * FROM pg_temp.ar_expect(
    'SELECT id FROM ar_a ORDER BY embedding <-> ''[3,0,0,0,0,0,0,1]''::vector LIMIT 1',
    'AR0','53200','LOAD',0,0);
RESET enable_seqscan;
SELECT (r->>'index_oid')::oid = 'ar_a_idx'::regclass AS is_ar_a FROM pg_cuvs_last_rejection() r;

SELECT pg_cuvs_set_vram_budget(0);
SET client_min_messages = warning;
SELECT pg_cuvs_reset_circuit('ar_a_idx');
RESET client_min_messages;
CREATE INDEX ar_cagra ON ar_items USING cagra (embedding vector_l2_ops);
SELECT pg_cuvs_last_rejection()->>'reason' AS last_reason_unchanged;
-- ANALYZE makes the permanent AR0 preflight run before any heap scan.
DROP INDEX ar_cagra;
ANALYZE ar_items;
SELECT pg_cuvs_set_vram_budget(144000);
SELECT contract_ok
    AND position('before the heap scan, no rows were read' IN err_detail) > 0
    AND position('ACE-disk requires regular disk' IN err_detail) > 0
    AND position('private workdir under /tmp/cuvs_indexes' IN err_detail) > 0
    AND position('checked by the HNSW forecast' IN err_detail) > 0
    AND position('build_algo=''ace''' IN err_hint) > 0 AS prespool_guidance_ok
FROM pg_temp.ar_expect(
    'CREATE INDEX ar_prespool ON ar_items USING cagra (embedding vector_l2_ops)',
    'AR0','54000','BUILD',1,1);

DROP TABLE ar_items;
DROP TABLE ar_shard;
DROP TABLE ar_a;
