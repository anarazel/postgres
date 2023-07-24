-- From https://postgr.es/m/TYWPR01MB10982A413E0EC4088E78C0E11B1A62@TYWPR01MB10982.jpnprd01.prod.outlook.com
\set ECHO all

-- prepare
DROP TABLE IF EXISTS test;
CREATE TABLE test (id1 int2, id2 int4, id3 int8, value varchar(32));
INSERT INTO test (SELECT i%11, i%103, i%1009, 'hello' FROM generate_series(1,1000000) s(i));
-- CREATE INDEX idx_id3 ON test(id3);
-- CREATE INDEX idx_id1_id3 ON test(id1, id3);
-- CREATE INDEX idx_id2_id3 ON test(id2, id3);
-- CREATE INDEX idx_id1_id2_id3 ON test(id1, id2, id3);
ANALYZE;

-- prepare
SET skipscan_prefix_cols = 3;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;

DROP EXTENSION IF EXISTS pg_prewarm;
CREATE EXTENSION pg_prewarm;
SELECT pg_prewarm('test');

-- seqscan
SET enable_seqscan = on;
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) SELECT * FROM test WHERE id3 = 101;
SET enable_seqscan = off;

-- indexscan
SET enable_indexscan = on;

CREATE INDEX idx_id3 ON test(id3);
SELECT pg_prewarm('idx_id3');
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) SELECT * FROM test WHERE id3 = 101;
DROP INDEX idx_id3;

CREATE INDEX idx_id1_id3 ON test(id1, id3);
SELECT pg_prewarm('idx_id1_id3');
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) SELECT * FROM test WHERE id3 = 101;
DROP INDEX idx_id1_id3;

CREATE INDEX idx_id2_id3 ON test(id2, id3);
SELECT pg_prewarm('idx_id2_id3');
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) SELECT * FROM test WHERE id3 = 101;
DROP INDEX idx_id2_id3;

CREATE INDEX idx_id1_id2_id3 ON test(id1, id2, id3);
SELECT pg_prewarm('idx_id1_id2_id3');
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) SELECT * FROM test WHERE id3 = 101;
DROP INDEX idx_id1_id2_id3;

SET enable_indexscan = off;

-- bitmapscan
SET enable_bitmapscan = on;

CREATE INDEX idx_id3 ON test(id3);
SELECT pg_prewarm('idx_id3');
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) SELECT * FROM test WHERE id3 = 101;
DROP INDEX idx_id3;

CREATE INDEX idx_id1_id3 ON test(id1, id3);
SELECT pg_prewarm('idx_id1_id3');
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) SELECT * FROM test WHERE id3 = 101;
DROP INDEX idx_id1_id3;

CREATE INDEX idx_id2_id3 ON test(id2, id3);
SELECT pg_prewarm('idx_id2_id3');
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) SELECT * FROM test WHERE id3 = 101;
DROP INDEX idx_id2_id3;

CREATE INDEX idx_id1_id2_id3 ON test(id1, id2, id3);
SELECT pg_prewarm('idx_id1_id2_id3');
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) SELECT * FROM test WHERE id3 = 101;
DROP INDEX idx_id1_id2_id3;

SET enable_bitmapscan = off;
