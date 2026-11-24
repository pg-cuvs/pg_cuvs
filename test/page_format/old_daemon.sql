\set ON_ERROR_STOP on
CREATE EXTENSION vector;
CREATE EXTENSION pg_cuvs;
SELECT count(*) FROM pg_cuvs_gpu_search_stats();
CREATE TABLE page_current (id int, v vector(3));
INSERT INTO page_current VALUES (1,'[1,0,0]'),(2,'[2,0,0]');
DO $$
DECLARE
    hint text;
BEGIN
    BEGIN
        CREATE INDEX page_current_idx ON page_current USING cagra(v vector_l2_ops);
        RAISE EXCEPTION 'an older daemon accepted the new BUILD message';
    EXCEPTION WHEN object_not_in_prerequisite_state THEN
        GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT;
        IF hint NOT LIKE '%supports the page format%' THEN
            RAISE EXCEPTION 'wrong upgrade hint: %',hint;
        END IF;
    END;
END $$;
