-- pg_cuvs upgrade: 0.2.0 -> 0.3.0
-- Phase 3Q: CAGRA streaming updates (in-place EXTEND + tombstone-based COMPACT)
--
-- Usage:
--   ALTER EXTENSION pg_cuvs UPDATE TO '0.3.0';
--
-- After upgrade:
--   SELECT pg_cuvs_compact('my_cagra_idx'::regclass);

\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO ''0.3.0''" to load this file. \quit

-- ----------------------------------------------------------------
-- pg_stat_gpu_search — 0.3.0 adds extend_count / compact_count /
-- last_compact_at. OUT parameters cannot be changed by CREATE OR REPLACE, so
-- the dependent view and the function are dropped and recreated. Column order
-- must match the SRF in src/pg_cuvs.c (pg_cuvs_gpu_search_stats).
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
    OUT last_compact_at    timestamptz
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
  'daemon restart; empty while the daemon is down.';

-- ----------------------------------------------------------------
-- pg_stat_gpu_fallback — per-index CPU-fallback counters (backend shmem).
-- A GPU index "falls back" when cuvsamcostestimate gates it off at plan time
-- (the planner picks seqscan/pgvector); that decision never reaches the daemon,
-- so pg_stat_gpu_search cannot show it. Backed by the SRF in src/pg_cuvs.c.
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_gpu_fallback_stats(
    OUT index_oid        regclass,
    OUT fallback_count   bigint,
    OUT last_reason      text,
    OUT last_fallback_at timestamptz
)
RETURNS SETOF record
AS '$libdir/pg_cuvs', 'pg_cuvs_gpu_fallback_stats'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_gpu_fallback_stats() IS
  'Per-index CPU-fallback counters for the current database. Backs the '
  'pg_stat_gpu_fallback view. Counts are a relative pressure signal (plan-time '
  'cost estimate may run more than once per query), not exact query counts.';

CREATE VIEW pg_stat_gpu_fallback AS
  SELECT * FROM pg_cuvs_gpu_fallback_stats();

COMMENT ON VIEW pg_stat_gpu_fallback IS
  'Per-index GPU->CPU fallback: how many times the planner dropped the GPU '
  'index path and why (last_reason: disabled/circuit_breaker/stale/delete_drift/'
  'daemon_down/no_artifact/delta_unusable/tombstone_unusable). Watch the trend '
  'against pg_stat_gpu_search.search_count to detect queries silently using CPU.';

-- ----------------------------------------------------------------
-- Phase 3Q: CAGRA streaming updates
-- ----------------------------------------------------------------
-- Manual compact trigger for a CAGRA index.
-- Removes tombstoned vectors via cuvsCagraMerge; clears the .tombstone sidecar.
-- Auto-compact is triggered by cuvs.compact_delete_ratio during VACUUM.
CREATE FUNCTION pg_cuvs_compact(index_rel regclass)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_compact'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_cuvs_compact(regclass) IS
  'Compact a CAGRA index: remove tombstoned vectors via cuvsCagraMerge, '
  'rebuild the on-disk .cagra + .tids, and delete the .tombstone sidecar. '
  'Auto-triggered during VACUUM when cuvs.compact_delete_ratio is exceeded.';

CREATE FUNCTION pg_cuvs_set_vram_budget(budget_bytes bigint)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_set_vram_budget'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pg_cuvs_set_vram_budget(bigint) IS
  'Override the per-GPU VRAM budget (bytes) for the running daemon. '
  '0 = unlimited. Intended for testing and capacity management; '
  'does not persist across daemon restarts.';

CREATE FUNCTION pg_cuvs_eat_vram(leave_bytes bigint)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_eat_vram'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pg_cuvs_eat_vram(bigint) IS
  'Test helper: pre-allocate GPU VRAM via cudaMalloc so that only '
  'leave_bytes remain free.  Forces physical CUDA OOM on the next '
  'large GPU operation, bypassing the VRAM budget-check path. '
  'Device 0. Release with pg_cuvs_free_vram().';

CREATE FUNCTION pg_cuvs_free_vram()
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_free_vram'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pg_cuvs_free_vram() IS
  'Release the VRAM held by pg_cuvs_eat_vram(). Device 0.';

CREATE FUNCTION pg_cuvs_inject_extend_oom(enable integer)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_inject_extend_oom'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pg_cuvs_inject_extend_oom(integer) IS
  'Test-only: arm (1) or disarm (0) synthetic OOM injection in cuvs_cagra_extend. '
  'When armed, the next extend throws bad_alloc, exercising _pr.poison() → '
  'BUILD_FAILED → delta fallback. The flag self-clears on fire.';

CREATE FUNCTION pg_cuvs_inject_build_oom(n_fail integer)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_inject_build_oom'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pg_cuvs_inject_build_oom(integer) IS
  'Test-only (ADR-070 Bug #3): arm synthetic OOM for the next n_fail '
  'cuvs_cagra_build calls in the daemon (0 = disarm), to exercise the build '
  'evict-and-retry path. Each failing build decrements the counter.';
