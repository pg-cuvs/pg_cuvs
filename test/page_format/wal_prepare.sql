\set ON_ERROR_STOP on
CREATE EXTENSION pageinspect;
CREATE TABLE page_durable (id int, v vector);
CREATE INDEX page_durable_idx ON page_durable USING cagra (v vector_l2_ops);
CREATE TABLE page_durable_expr (id int, v vector(3));
CREATE INDEX page_durable_expr_idx ON page_durable_expr
    USING cagra (((v + v)::vector(3)) vector_l2_ops);
CHECKPOINT;
INSERT INTO page_durable VALUES (1, '[0,1,2]');
INSERT INTO page_durable_expr VALUES (1, '[1,2,3]');
