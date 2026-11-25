-- compact_width_notice.sql — #261 follow-up: a compaction that changes the
-- graph width (graph_degree altered since the build) says so with a NOTICE,
-- and one that keeps the width because the reloptions are inconsistent
-- (intermediate_graph_degree below graph_degree, which CREATE INDEX rejects)
-- says so with a WARNING. One line per compaction and index; nothing when the
-- width is unchanged or there is nothing to compact. No ERROR inside VACUUM.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
RESET client_min_messages;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

CREATE FUNCTION pg_temp.cwn_vec(g int) RETURNS vector LANGUAGE sql IMMUTABLE AS
$$ SELECT (SELECT array_agg(((hashtext(g::text || ':' || d::text) & 65535) / 65536.0)::real)
           FROM generate_series(1, 16) d)::vector $$;
CREATE TABLE cwn (id int, v vector(16)) WITH (autovacuum_enabled = off);
INSERT INTO cwn SELECT g, pg_temp.cwn_vec(g) FROM generate_series(1, 3000) g;
CREATE INDEX cwn_c ON cwn USING cagra (v vector_l2_ops) WITH (graph_degree = 32);

-- Same width: no message.
DELETE FROM cwn WHERE id <= 400;
VACUUM cwn;

-- graph_degree altered: the compaction rebuilds at 40 and says so.
ALTER INDEX cwn_c SET (graph_degree = 40);
DELETE FROM cwn WHERE id <= 800;
VACUUM cwn;

-- intermediate_graph_degree (128) below graph_degree: the width stays 40.
-- With the ratio trigger off, amvacuumcleanup (not ambulkdelete) compacts.
ALTER INDEX cwn_c SET (graph_degree = 200);
DELETE FROM cwn WHERE id <= 1200;
SET cuvs.compact_delete_ratio = 0;
VACUUM cwn;
RESET cuvs.compact_delete_ratio;
-- pg_cuvs_compact() reports it too (VACUUM without a socket only records
-- the tombstones).
DELETE FROM cwn WHERE id <= 1600;
SET cuvs.socket_path = '';
VACUUM cwn;
RESET cuvs.socket_path;
SELECT pg_cuvs_compact('cwn_c');
-- Nothing to compact: no message.
VACUUM cwn;
SELECT pg_cuvs_compact('cwn_c');

SELECT compact_count FROM pg_stat_gpu_search WHERE index_oid = 'cwn_c'::regclass;
DROP TABLE cwn;
