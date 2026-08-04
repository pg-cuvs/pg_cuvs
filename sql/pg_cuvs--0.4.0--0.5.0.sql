/* pg_cuvs 0.4.0 -> 0.5.0 — ADR-075 Phase 1: hardware-profile introspection.
 * Read-only; exposes the physical constants the daemon measures at boot. The
 * cost model does not consume these yet (Phase 2). */

CREATE FUNCTION pg_cuvs_hw_profile(
    OUT gpu_name                text,
    OUT n_gpus                  integer,
    OUT total_vram_bytes        bigint,
    OUT link_bw_bytes_per_us    double precision,
    OUT hbm_bw_bytes_per_us     double precision,
    OUT gpu_bf_tput             double precision,
    OUT ipc_rtt_us              double precision,
    OUT measured_at_epoch       bigint,
    OUT probe_status            integer,
    OUT source                  text,
    OUT matches_running_daemon  boolean,
    OUT cpu_dist_tput           double precision,
    OUT gpu_cagra_lat_us        double precision
)
RETURNS SETOF record
AS '$libdir/pg_cuvs', 'pg_cuvs_hw_profile'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_hw_profile() IS
  'Measured (or DEFAULT) hardware profile written by the pg_cuvs daemon at boot '
  '(ADR-075 Phase 1). source = measured|default; matches_running_daemon flags a '
  'stale profile vs the running daemon (GPU swap / migration). Bandwidths are '
  'bytes per microsecond; gpu_bf_tput is (vectors*dim) per microsecond. Read-only; '
  'not yet consumed by the cost model.';

CREATE OR REPLACE FUNCTION cuvs_filtered_knn(
    index_rel   regclass,
    query       vector,
    filter_tids tid[],
    k           integer
)
RETURNS TABLE (ctid tid, distance float4)
LANGUAGE sql STABLE AS $$
    SELECT * FROM cuvs_filtered_knn(
        index_rel,
        query,
        CASE
            WHEN filter_tids IS NULL THEN NULL::bigint[]
            ELSE COALESCE(
                (SELECT array_agg(encoded ORDER BY encoded)
                 FROM (
                     SELECT (((t::text::point)[0])::bigint << 16) |
                             ((t::text::point)[1])::bigint AS encoded
                     FROM unnest(filter_tids) t
                 ) encoded_tids),
                ARRAY[]::bigint[])
        END,
        k
    );
$$;

-- These five reach into daemon-global state (VRAM budget, a VRAM balloon, and the
-- fault-injection counters). PostgreSQL grants EXECUTE to PUBLIC by default, which
-- would let any role arm a build failure or pin VRAM and break *other* sessions'
-- index builds. Applied on upgrade as well as fresh install.
REVOKE ALL ON FUNCTION pg_cuvs_set_vram_budget(bigint)     FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_eat_vram(bigint)            FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_free_vram()                 FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_inject_extend_oom(integer)  FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_inject_build_oom(integer)   FROM PUBLIC;

-- #124: the production daemon rejects INJECT_* opcodes (CUVS_TEST_HOOKS-only);
-- mirror the base script's COMMENT text so an upgraded install matches a fresh
-- 0.5.0 install (see upgrade_path.sql, which diffs the two).
COMMENT ON FUNCTION pg_cuvs_inject_extend_oom(integer) IS
  'Test-only: arm (1) or disarm (0) synthetic OOM injection in cuvs_cagra_extend. '
  'When armed, the next extend throws bad_alloc, exercising _pr.poison() → '
  'BUILD_FAILED → delta fallback. The flag self-clears on fire. '
  '#124: the production daemon rejects this call (CUVS_OP_INJECT_EXTEND_OOM '
  'is compiled in only under CUVS_TEST_HOOKS) — point cuvs.socket_path at a '
  'pg_cuvs_server_test daemon (make installcheck-fault) to use it.';

COMMENT ON FUNCTION pg_cuvs_inject_build_oom(integer) IS
  'Test-only (ADR-070 Bug #3): arm synthetic OOM for the next n_fail '
  'cuvs_cagra_build calls in the daemon (0 = disarm), to exercise the build '
  'evict-and-retry path. Each failing build decrements the counter. '
  '#124: the production daemon rejects this call (CUVS_OP_INJECT_BUILD_OOM '
  'is compiled in only under CUVS_TEST_HOOKS) — point cuvs.socket_path at a '
  'pg_cuvs_server_test daemon (make installcheck-fault) to use it.';
REVOKE ALL ON FUNCTION pg_cuvs_gc_orphans(boolean)         FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_compact(regclass)           FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_build_hnsw(regclass, text)  FROM PUBLIC;
