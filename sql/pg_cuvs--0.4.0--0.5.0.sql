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

-- These five reach into daemon-global state (VRAM budget, a VRAM balloon, and the
-- fault-injection counters). PostgreSQL grants EXECUTE to PUBLIC by default, which
-- would let any role arm a build failure or pin VRAM and break *other* sessions'
-- index builds. Applied on upgrade as well as fresh install.
REVOKE ALL ON FUNCTION pg_cuvs_set_vram_budget(bigint)     FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_eat_vram(bigint)            FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_free_vram()                 FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_inject_extend_oom(integer)  FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_inject_build_oom(integer)   FROM PUBLIC;
