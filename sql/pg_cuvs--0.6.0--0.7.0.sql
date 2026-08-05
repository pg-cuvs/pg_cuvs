-- pg_cuvs upgrade: 0.6.0 -> 0.7.0
-- #160: the daemon's Phase 3L-9 micro-batch worker now also serves unsharded
-- single-query CAGRA searches, gated by the new cuvs.cagra_batch_wait_us GUC.
-- This adds the observability counter for those coalesced dispatches.
--
-- Usage:
--   ALTER EXTENSION pg_cuvs UPDATE TO '0.7.0';

\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO ''0.7.0''" to load this file. \quit

-- ----------------------------------------------------------------
-- pg_stat_gpu_search — 0.7.0 adds cagra_batch_count. OUT parameters cannot be
-- changed by CREATE OR REPLACE, so the dependent view and the function are
-- dropped and recreated. Column order must match the SRF in src/pg_cuvs.c
-- (pg_cuvs_gpu_search_stats).
-- ----------------------------------------------------------------
DROP VIEW pg_stat_gpu_search;
DROP FUNCTION pg_cuvs_gpu_search_stats();

CREATE FUNCTION pg_cuvs_gpu_search_stats(
    OUT database_oid    oid,
    OUT index_oid       oid,
    OUT index_name      text,
    OUT dim             integer,
    OUT metric          text,
    OUT n_vecs          bigint,
    OUT vram_bytes      bigint,
    OUT resident        boolean,
    OUT search_count    bigint,
    OUT error_count     bigint,
    OUT avg_latency_us  double precision,
    OUT p50_latency_us  integer,
    OUT p95_latency_us  integer,
    OUT p99_latency_us  integer,
    OUT last_status     text,
    OUT last_error      text,
    OUT last_search_at  timestamptz,
    OUT requested_k     integer,
    OUT returned_k      integer,
    OUT stale           boolean,
    OUT stale_since     timestamptz,
    OUT delta_rows         bigint,
    OUT delta_generation   bigint,
    OUT delta_vram_bytes   bigint,
    OUT delta_merged_count bigint,
    OUT delta_search_mode  text,
    OUT warmup_state       text,
    OUT last_warmup_at     timestamptz,
    OUT warmup_duration_ms integer,
    OUT download_count     bigint,
    OUT cache_miss_count   bigint,
    OUT gpu_device_id      integer,
    OUT shard_count        integer,
    OUT search_mode        text,
    OUT bf_batch_count     bigint,
    OUT extend_count       bigint,
    OUT compact_count      bigint,
    OUT last_compact_at    timestamptz,
    OUT prefilter_fallback_count bigint,
    OUT cagra_batch_count  bigint
)
RETURNS SETOF record
AS '$libdir/pg_cuvs', 'pg_cuvs_gpu_search_stats'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_gpu_search_stats() IS
  'Per-index GPU search statistics from the pg_cuvs sidecar daemon for the '
  'current database. Backs the pg_stat_gpu_search view. Empty when the '
  'daemon is unavailable.';

CREATE VIEW pg_stat_gpu_search AS
  SELECT * FROM pg_cuvs_gpu_search_stats();

COMMENT ON VIEW pg_stat_gpu_search IS
  'GPU CAGRA per-index search stats: counts, fallbacks/errors, and '
  'approximate p50/p95/p99 latency. Counters reset on index rebuild or '
  'daemon restart; empty while the daemon is down. prefilter_fallback_count '
  '(#133/ADR-083) counts 3O->gpu_bf_prefilter retries triggered by a short-fill '
  'collapse detection (anti-correlated filter) -- watch it against '
  'search_count to see an index "quietly went slow" on a hostile filter shape. '
  'cagra_batch_count (#160) counts coalesced CAGRA micro-batch dispatches, the '
  'cagra twin of bf_batch_count -- non-zero only while cuvs.cagra_batch_wait_us '
  'is set, and the way to confirm that window is actually routing through the '
  'daemon batch worker. One dispatch can serve many concurrent requests, so '
  'search_count / cagra_batch_count is the average coalesced batch width.';
