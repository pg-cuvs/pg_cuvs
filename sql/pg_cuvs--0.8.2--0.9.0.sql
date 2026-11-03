-- pg_cuvs upgrade: 0.8.2 -> 0.9.0
\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO '0.9.0'" to load this file. \quit

-- 0.9.0 I3 admission/eviction set (ADR-086..088). Co-deploy the 0.9.0
-- extension and daemon (IPC version 5, ADR-081). Each set PR appends only its
-- own "-- <PR> begin" / "-- <PR> end" block below, identically in
-- pg_cuvs--0.9.0.sql and pg_cuvs--0.8.2--0.9.0.sql, in merge order
-- I3-A, I3-B, I3-C, I4. test/sql/upgrade_path.sql compares the two catalogs.

-- I3-A begin
-- I3-A end
