-- pg_cuvs upgrade: 0.5.0 -> 0.6.0
-- #133 / ADR-083: 3O (CAGRA BITSET prefilter) recall collapses on
-- anti-correlated filters regardless of selectivity. The daemon now detects a
-- materially short fill (mean_returned << k) and retries on D-wedge; this
-- adds an observability counter for that retry.
--
-- Usage:
--   ALTER EXTENSION pg_cuvs UPDATE TO '0.6.0';

\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO ''0.6.0''" to load this file. \quit

-- ----------------------------------------------------------------
-- pg_stat_gpu_search — 0.6.0 adds prefilter_fallback_count. OUT parameters
-- cannot be changed by CREATE OR REPLACE, so the dependent view and the
-- function are dropped and recreated. Column order must match the SRF in
-- src/pg_cuvs.c (pg_cuvs_gpu_search_stats).
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
    OUT prefilter_fallback_count bigint
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
  '(#133/ADR-083) counts 3O->D-wedge retries triggered by a short-fill '
  'collapse detection (anti-correlated filter) -- watch it against '
  'search_count to see an index "quietly went slow" on a hostile filter shape.';
