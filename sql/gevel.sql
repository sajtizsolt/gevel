-- ================================================================
-- Gevel Extension Regression Test Suite
-- ================================================================

SET extra_float_digits = 0;

CREATE EXTENSION gevel;

-- ================================================================
-- 1. GiST Index Inspection
-- ================================================================
CREATE TABLE test_gist (id int, b box);
INSERT INTO test_gist VALUES 
  (1, box(point(0,0), point(1,1))),
  (2, box(point(2,2), point(3,3)));
CREATE INDEX idx_test_gist ON test_gist USING gist(b);

SELECT gist_stat('idx_test_gist');
SELECT gist_tree('idx_test_gist');
SELECT gist_tree('idx_test_gist', 1);
SELECT * FROM gist_print('idx_test_gist') AS t(level int, valid bool, b box)
  ORDER BY b::text;

-- ================================================================
-- 2. B-Tree Index Inspection (Multi-Column & Covering INCLUDE)
-- ================================================================
CREATE TABLE test_btree (id int, val text);
INSERT INTO test_btree VALUES 
  (1, 'apple'),
  (2, 'banana'),
  (3, 'cherry');
CREATE INDEX idx_test_btree ON test_btree USING btree(id, val);

SELECT btree_stat('idx_test_btree');
SELECT btree_tree('idx_test_btree');
SELECT btree_tree('idx_test_btree', 1);
SELECT * FROM btree_print('idx_test_btree') AS t(level int, valid bool, id int, val text)
  ORDER BY level, id;

-- Covering index with non-key INCLUDE column
CREATE TABLE test_btree_inc (id int, payload text);
INSERT INTO test_btree_inc VALUES 
  (1, 'alpha'),
  (2, 'beta');
CREATE INDEX idx_test_btree_inc ON test_btree_inc (id) INCLUDE (payload);

SELECT btree_stat('idx_test_btree_inc');
SELECT * FROM btree_print('idx_test_btree_inc') AS t(level int, valid bool, id int, payload text)
  ORDER BY level, id;

-- ================================================================
-- 3. GIN Index Inspection (Single & Multi-Column)
-- ================================================================
CREATE TABLE test_gin (doc tsvector, arr int[]);
INSERT INTO test_gin VALUES 
  (to_tsvector('english', 'postgresql index inspection'), ARRAY[10, 20]);
CREATE INDEX idx_test_gin_ts ON test_gin USING gin(doc);
CREATE INDEX idx_test_gin_arr ON test_gin USING gin(arr);

SELECT gin_statpage('idx_test_gin_ts');
SELECT * FROM gin_stat('idx_test_gin_arr') AS t(val int, nrow int)
  ORDER BY val;
SELECT gin_count_estimate('idx_test_gin_ts', to_tsquery('english', 'postgresql'));

-- Multi-column GIN index with column offset
CREATE TABLE test_gin_multi (doc tsvector, arr int[]);
INSERT INTO test_gin_multi VALUES 
  (to_tsvector('english', 'multi column gin'), ARRAY[1, 2]);
CREATE INDEX idx_test_gin_multi ON test_gin_multi USING gin(doc, arr);

SELECT * FROM gin_stat('idx_test_gin_multi', 0) AS t(val text, nrow int)
  ORDER BY val;
SELECT * FROM gin_stat('idx_test_gin_multi', 1) AS t(val int, nrow int)
  ORDER BY val;

-- ================================================================
-- 4. SP-GiST Index Inspection (2D Points)
-- ================================================================
CREATE TABLE test_spgist (p point);
INSERT INTO test_spgist VALUES (point(1,1)), (point(2,2));
CREATE INDEX idx_test_spgist ON test_spgist USING spgist(p);

SELECT spgist_stat('idx_test_spgist');
SELECT * FROM spgist_print('idx_test_spgist')
  AS t(tid tid, allthesame bool, node int, level int, tid_pointer tid, prefix point, label int, leaf point)
  ORDER BY tid;

-- ================================================================
-- 5. BRIN Index Inspection
-- ================================================================
CREATE TABLE test_brin (id int);
INSERT INTO test_brin SELECT generate_series(1, 100);
CREATE INDEX idx_test_brin ON test_brin USING brin(id);

SELECT brin_stat('idx_test_brin');
SELECT brin_print('idx_test_brin');

-- ================================================================
-- 6. Empty Index Handling Across All Access Methods
-- ================================================================
CREATE TABLE test_empty (id int, b box, p point, arr int[]);
CREATE INDEX idx_empty_gist ON test_empty USING gist(b);
CREATE INDEX idx_empty_btree ON test_empty USING btree(id);
CREATE INDEX idx_empty_gin ON test_empty USING gin(arr);
CREATE INDEX idx_empty_spgist ON test_empty USING spgist(p);
CREATE INDEX idx_empty_brin ON test_empty USING brin(id);

SELECT gist_stat('idx_empty_gist');
SELECT gist_tree('idx_empty_gist');
SELECT * FROM gist_print('idx_empty_gist') AS t(level int, valid bool, b box);

SELECT btree_stat('idx_empty_btree');
SELECT btree_tree('idx_empty_btree');
SELECT * FROM btree_print('idx_empty_btree') AS t(level int, valid bool, id int);

SELECT gin_statpage('idx_empty_gin');
SELECT * FROM gin_stat('idx_empty_gin') AS t(val int, nrow int);

SELECT spgist_stat('idx_empty_spgist');
SELECT * FROM spgist_print('idx_empty_spgist')
  AS t(tid tid, allthesame bool, node int, level int, tid_pointer tid, prefix point, label int, leaf point);

SELECT brin_stat('idx_empty_brin');
SELECT brin_print('idx_empty_brin');

-- ================================================================
-- 7. Defensive Error Handling & Guidance
-- ================================================================
-- Non-existent index
DO $$
BEGIN
  PERFORM btree_stat('non_existent_index');
EXCEPTION WHEN OTHERS THEN
  RAISE NOTICE 'Handled error: %', SQLERRM;
END $$;

-- Wrong access method
DO $$
BEGIN
  PERFORM btree_stat('idx_test_gist');
EXCEPTION WHEN OTHERS THEN
  RAISE NOTICE 'Handled error: %', SQLERRM;
END $$;

-- Non-index relation (table instead of index)
DO $$
BEGIN
  PERFORM btree_stat('test_btree');
EXCEPTION WHEN OTHERS THEN
  RAISE NOTICE 'Handled error: %', SQLERRM;
END $$;

-- Partitioned index guidance
CREATE TABLE test_part (id int) PARTITION BY RANGE (id);
CREATE TABLE test_part_1 PARTITION OF test_part FOR VALUES FROM (1) TO (100);
CREATE INDEX idx_test_part ON test_part (id);

DO $$
BEGIN
  PERFORM btree_stat('idx_test_part');
EXCEPTION WHEN OTHERS THEN
  RAISE NOTICE 'Handled error: %', SQLERRM;
END $$;

-- Invalid column number for gin_stat
DO $$
BEGIN
  PERFORM * FROM gin_stat('idx_test_gin_arr', 99) AS t(val int, nrow int);
EXCEPTION WHEN OTHERS THEN
  RAISE NOTICE 'Handled error: %', SQLERRM;
END $$;

-- ================================================================
-- 8. Teardown
-- ================================================================
DROP TABLE test_gist, test_btree, test_btree_inc, test_gin, test_gin_multi, test_spgist, test_brin, test_empty, test_part;
DROP EXTENSION gevel;
