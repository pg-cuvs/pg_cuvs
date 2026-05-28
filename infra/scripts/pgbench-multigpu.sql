\set qid random(1, 2000)
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET enable_seqscan = off;
SELECT id FROM mgpu16_a ORDER BY v <-> (SELECT v FROM mgpu16_a WHERE id = :qid) LIMIT 5;
SELECT id FROM mgpu16_b ORDER BY v <-> (SELECT v FROM mgpu16_b WHERE id = :qid) LIMIT 5;
