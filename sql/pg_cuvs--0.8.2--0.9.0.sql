-- pg_cuvs upgrade: 0.8.2 -> 0.9.0
\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO '0.9.0'" to load this file. \quit

-- 0.9.0 I3 admission/eviction set (ADR-086..088). Co-deploy the 0.9.0
-- extension and daemon (IPC version 5, ADR-081). Each set PR appends only its
-- own "-- <PR> begin" / "-- <PR> end" block below, identically in
-- pg_cuvs--0.9.0.sql and pg_cuvs--0.8.2--0.9.0.sql, in merge order
-- I3-A, I3-B, I3-C, I4. test/sql/upgrade_path.sql compares the two catalogs.

-- I3-A begin
-- Last admission rejection of this backend (UC-2): reason, SQLSTATE, every
-- reported value, up to three candidates, time, requester and shard. EXECUTE
-- stays with PUBLIC: it reports only the caller's own rejection.
CREATE FUNCTION pg_cuvs_last_rejection() RETURNS jsonb
AS 'MODULE_PATHNAME', 'pg_cuvs_last_rejection'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;
COMMENT ON FUNCTION pg_cuvs_last_rejection() IS
  'Last pg_cuvs GPU memory admission rejection in this session as jsonb (NULL if none); kept in backend memory only.';
-- I3-A end

-- I3-B begin
-- ----------------------------------------------------------------
-- I3-B: manual GPU cache control and cache/memory status (daemon ops 30..37).
-- Grants: the regclass forms keep EXECUTE for PUBLIC; the C function allows
-- only the index owner or a superuser. The (oid, oid) overloads and
-- pg_cuvs_reclaim_set are revoked from PUBLIC and superuser only (C checks
-- superuser() as well). pg_cuvs_index_status is PUBLIC and filters rows in C;
-- pg_cuvs_memory_status is for superusers and pg_monitor.
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_unload(index regclass) RETURNS text
AS 'MODULE_PATHNAME', 'pg_cuvs_unload'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pg_cuvs_unload(regclass) IS
  'Unload the index from GPU VRAM now; its on-disk files are kept. '
  'Requires index ownership. Fails with SQLSTATE 55006 while the index is pinned or in use.';

CREATE FUNCTION pg_cuvs_load(index regclass) RETURNS text
AS 'MODULE_PATHNAME', 'pg_cuvs_load'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pg_cuvs_load(regclass) IS
  'Load the index into GPU VRAM ahead of the first search. Requires index ownership.';

CREATE FUNCTION pg_cuvs_pin(index regclass) RETURNS text
AS 'MODULE_PATHNAME', 'pg_cuvs_pin'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pg_cuvs_pin(regclass) IS
  'Pin the index in GPU VRAM so eviction and reclaim skip it until pg_cuvs_unpin. '
  'Requires index ownership.';

CREATE FUNCTION pg_cuvs_unpin(index regclass) RETURNS text
AS 'MODULE_PATHNAME', 'pg_cuvs_unpin'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pg_cuvs_unpin(regclass) IS
  'Release a pg_cuvs_pin so the index becomes evictable again. Requires index ownership.';

CREATE FUNCTION pg_cuvs_unload(db_oid oid, index_oid oid) RETURNS text
AS 'MODULE_PATHNAME', 'pg_cuvs_unload_oid'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pg_cuvs_unload(oid, oid) IS
  'Superuser only: unload a daemon cache entry by (database OID, index OID), '
  'e.g. an entry of another database or of an index that no longer resolves.';

CREATE FUNCTION pg_cuvs_unpin(db_oid oid, index_oid oid) RETURNS text
AS 'MODULE_PATHNAME', 'pg_cuvs_unpin_oid'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pg_cuvs_unpin(oid, oid) IS
  'Superuser only: unpin a daemon cache entry by (database OID, index OID).';

CREATE FUNCTION pg_cuvs_reclaim_set(name text, value text) RETURNS text
AS 'MODULE_PATHNAME', 'pg_cuvs_reclaim_set'
LANGUAGE C VOLATILE STRICT;
COMMENT ON FUNCTION pg_cuvs_reclaim_set(text, text) IS
  'Superuser only: change one daemon reclaim parameter at runtime (name is the '
  'daemon flag spelling without "--"). '
  'An invalid name or value fails with SQLSTATE 22023.';

CREATE FUNCTION pg_cuvs_index_status()
RETURNS TABLE (
    index_name              text,
    db_oid                  oid,
    index_oid               oid,
    state                   text,
    kind                    text,
    gpu                     integer,
    vram_bytes              bigint,
    last_use                timestamptz,
    loaded_at               timestamptz,
    hits                    bigint,
    pinned                  boolean,
    in_use                  bigint,
    protect_until           timestamptz,
    eligible                boolean,
    reclaim_exempt          text,
    file_current            boolean,
    extend_count            bigint,
    warmup_state            text,
    evict_count             bigint,
    last_evict_at           timestamptz,
    last_evict_reason       text,
    last_build_self_replace boolean,
    last_build_unloaded     bigint
)
AS 'MODULE_PATHNAME', 'pg_cuvs_index_status'
LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pg_cuvs_index_status() IS
  'One row per daemon cache entry. Superusers and pg_monitor see every database; '
  'other roles see only indexes they own in the current database. index_name is '
  'NULL for entries of other databases or of indexes that no longer exist. '
  'Empty when the daemon is unavailable.';

CREATE FUNCTION pg_cuvs_memory_status()
RETURNS TABLE (
    gpu                  integer,
    budget_eff           bigint,
    resident_bytes       bigint,
    pending_bytes        bigint,
    pinned_bytes         bigint,
    n_resident           bigint,
    n_pinned             bigint,
    reclaim_mode         text,
    reclaim_f_bytes      bigint,
    reclaim_f_src        text,
    last_reclaim_at      timestamptz,
    last_reclaim_trigger text,
    reclaim_runs         bigint,
    reclaim_skip_sidecar bigint,
    persist_failures     bigint,
    evict_admit_build    bigint,
    evict_reload         bigint,
    evict_soft_evict     bigint,
    evict_reclaim        bigint,
    evict_manual_unload  bigint,
    evict_slot           bigint,
    evictions            bigint,
    drops                bigint,
    history_dropped      bigint,
    peak_bytes           bigint
)
AS 'MODULE_PATHNAME', 'pg_cuvs_memory_status'
LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pg_cuvs_memory_status() IS
  'Per-GPU VRAM accounting, reclaim state and eviction counters by cause. '
  'Superuser or pg_monitor only. Empty when the daemon is unavailable.';

-- unload/load/pin/unpin(regclass) stay PUBLIC: the C body allows only the
-- index owner or a superuser (coordination rule, conch 4e421f63).
REVOKE ALL ON FUNCTION pg_cuvs_unload(oid, oid)      FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_unpin(oid, oid)       FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_reclaim_set(text, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_cuvs_memory_status()       FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_cuvs_memory_status()    TO pg_monitor;
-- Circuit breakers of the calling backend (UC-2 3; no IPC, PUBLIC).
CREATE FUNCTION pg_cuvs_circuit_status()
RETURNS TABLE (
    index_oid          oid,
    state              text,
    transient          boolean,
    opened_at          timestamptz,
    half_open_count    bigint,
    consecutive_errors integer
)
AS 'MODULE_PATHNAME', 'pg_cuvs_circuit_status'
LANGUAGE C VOLATILE STRICT PARALLEL RESTRICTED;
-- EXECUTE stays with PUBLIC (default): it reports only the caller's backend.

COMMENT ON FUNCTION pg_cuvs_circuit_status() IS
  'Circuit breakers tracked by this backend, one row per index (no IPC). '
  'state: closed/open/half_open, evaluated with cuvs.circuit_breaker_transient_ttl_s. '
  'transient: reason class of the last open (transient reasons half-open after the TTL; '
  'permanent ones need pg_cuvs_reset_circuit). opened_at: NULL if never opened. '
  'half_open_count: OPEN->HALF_OPEN transitions.';
-- I3-B end

-- I3-E begin
-- ----------------------------------------------------------------
-- I3-E: C8 observation fields that no earlier set PR carried (append-only).
-- The daemon appends columns to the I3-B text rows (ops 34 and 35), so the
-- two status functions are redefined here with the joined column list and
-- keep the I3-B grants: pg_cuvs_memory_status is for superusers and
-- pg_monitor; pg_cuvs_index_status is PUBLIC and filters rows in C.
-- pg_cuvs_build_status reads builds[] of the pool status v2 reply; EXECUTE
-- stays with PUBLIC and the C function filters rows as index_status does.
-- ----------------------------------------------------------------
DROP FUNCTION pg_cuvs_index_status();
CREATE FUNCTION pg_cuvs_index_status()
RETURNS TABLE (
    index_name              text,
    db_oid                  oid,
    index_oid               oid,
    state                   text,
    kind                    text,
    gpu                     integer,
    vram_bytes              bigint,
    last_use                timestamptz,
    loaded_at               timestamptz,
    hits                    bigint,
    pinned                  boolean,
    in_use                  bigint,
    protect_until           timestamptz,
    eligible                boolean,
    reclaim_exempt          text,
    file_current            boolean,
    extend_count            bigint,
    warmup_state            text,
    evict_count             bigint,
    last_evict_at           timestamptz,
    last_evict_reason       text,
    last_build_self_replace boolean,
    last_build_unloaded     bigint,
    dataset_attached        boolean
)
AS 'MODULE_PATHNAME', 'pg_cuvs_index_status'
LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pg_cuvs_index_status() IS
  'One row per daemon cache entry. Superusers and pg_monitor see every database; '
  'other roles see only indexes they own in the current database. index_name is '
  'NULL for entries of other databases or of indexes that no longer exist. '
  'dataset_attached is NULL in this release. Empty when the daemon is unavailable.';

DROP FUNCTION pg_cuvs_memory_status();
CREATE FUNCTION pg_cuvs_memory_status()
RETURNS TABLE (
    gpu                     integer,
    budget_eff              bigint,
    resident_bytes          bigint,
    pending_bytes           bigint,
    pinned_bytes            bigint,
    n_resident              bigint,
    n_pinned                bigint,
    reclaim_mode            text,
    reclaim_f_bytes         bigint,
    reclaim_f_src           text,
    last_reclaim_at         timestamptz,
    last_reclaim_trigger    text,
    reclaim_runs            bigint,
    reclaim_skip_sidecar    bigint,
    persist_failures        bigint,
    evict_admit_build       bigint,
    evict_reload            bigint,
    evict_soft_evict        bigint,
    evict_reclaim           bigint,
    evict_manual_unload     bigint,
    evict_slot              bigint,
    evictions               bigint,
    drops                   bigint,
    history_dropped         bigint,
    peak_bytes              bigint,
    budget_bytes            bigint,
    quota_bytes             bigint,
    pool_used               bigint,
    pool_reserved           bigint,
    pending_rmm             bigint,
    lw_pending              bigint,
    builds_active           bigint,
    rejects_ar0             bigint,
    rejects_ar1             bigint,
    rejects_ar2             bigint,
    rejects_ar3             bigint,
    rejects_ar4             bigint,
    rejects_ar5             bigint,
    rejects_ar6             bigint,
    rejects_ar7             bigint,
    rejects_ar8             bigint,
    last_reject_reason      text,
    last_reject_at          timestamptz,
    managed_spill_builds    bigint,
    managed_spill_bytes_max bigint
)
AS 'MODULE_PATHNAME', 'pg_cuvs_memory_status'
LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pg_cuvs_memory_status() IS
  'Per-GPU VRAM accounting, pending build reservations, reclaim state, eviction '
  'counters by cause and admission rejections by reason (cumulative since daemon '
  'start). Superuser or pg_monitor only. Empty when the daemon is unavailable.';
REVOKE ALL ON FUNCTION pg_cuvs_memory_status()       FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_cuvs_memory_status()    TO pg_monitor;

CREATE FUNCTION pg_cuvs_build_status()
RETURNS TABLE (
    index_name text,
    gpu        integer,
    db_oid     oid,
    index_oid  oid,
    algo       text,
    path       text,
    started_at timestamptz,
    n          bigint,
    dim        integer,
    final      bigint,
    scratch    bigint,
    reserved_n bigint,
    policy     text,
    lw_ub      bigint,
    lw_ub_src  text,
    phase      text
)
AS 'MODULE_PATHNAME', 'pg_cuvs_build_status'
LANGUAGE C VOLATILE;
COMMENT ON FUNCTION pg_cuvs_build_status() IS
  'In-flight CAGRA builds that run outside the daemon lock (single and multi '
  'path), one row per build. Superusers and pg_monitor see every build; other '
  'roles see only builds of indexes they own in the current database. Sharded '
  'and IVF-PQ builds are not listed. Empty when the daemon is unavailable.';
-- I3-E end

-- CONN203 begin
-- #203: immutable build observation, not a live graph-integrity assertion.
CREATE FUNCTION pg_cuvs_hnsw_connectivity_status(index regclass) RETURNS jsonb
LANGUAGE SQL STABLE STRICT AS $$
 SELECT COALESCE(pg_cuvs_hnsw_build_info($1)->'connectivity',
                 '{"status":"not_recorded","scope":"build_snapshot"}'::jsonb)
$$;
COMMENT ON FUNCTION pg_cuvs_hnsw_connectivity_status(regclass) IS
 'Last build structural connectivity snapshot; does not track INSERT/DELETE/VACUUM and does not guarantee finite-ef recall. Historical indexes may be not_recorded; new imports attempt the check within maintenance_work_mem.';
-- CONN203 end
