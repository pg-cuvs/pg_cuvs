-- pg_cuvs upgrade: 0.7.0 -> 0.8.0
-- #162: pg_cuvs_hnsw gains halfvec support (dim=3072 path via pgvector's
-- halfvec type; smaller indexes at dim=1024/1536). GPU CAGRA build stays
-- fp32 throughout -- only the final page-write step narrow-casts to fp16.
-- No existing SQL object changes; purely additive (three new opclasses).
--
-- Usage:
--   ALTER EXTENSION pg_cuvs UPDATE TO '0.8.0';

\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO ''0.8.0''" to load this file. \quit

-- ----------------------------------------------------------------
-- halfvec opclasses for pg_cuvs_hnsw (#162) -- see pg_cuvs--0.8.0.sql for
-- the full rationale comment (identical block, kept in sync by hand since
-- this is a fresh-install-vs-upgrade duplication pgvector's own SQL files
-- use the same pattern for).
-- ----------------------------------------------------------------
CREATE OPERATOR CLASS halfvec_l2_ops
FOR TYPE halfvec USING pg_cuvs_hnsw AS
    OPERATOR 1 <-> (halfvec, halfvec) FOR ORDER BY float_ops,
    FUNCTION 1 halfvec_l2_squared_distance(halfvec, halfvec),
    FUNCTION 3 hnsw_halfvec_support(internal);

CREATE OPERATOR CLASS halfvec_ip_ops
FOR TYPE halfvec USING pg_cuvs_hnsw AS
    OPERATOR 1 <#> (halfvec, halfvec) FOR ORDER BY float_ops,
    FUNCTION 1 halfvec_negative_inner_product(halfvec, halfvec),
    FUNCTION 3 hnsw_halfvec_support(internal);

CREATE OPERATOR CLASS halfvec_cosine_ops
FOR TYPE halfvec USING pg_cuvs_hnsw AS
    OPERATOR 1 <=> (halfvec, halfvec) FOR ORDER BY float_ops,
    FUNCTION 1 halfvec_negative_inner_product(halfvec, halfvec),
    FUNCTION 2 l2_norm(halfvec),
    FUNCTION 3 hnsw_halfvec_support(internal);
