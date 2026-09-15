--
-- CREATE_INDEX
-- Create ancillary data structures (i.e. indices)
--

-- directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR

--
-- BTREE
--
CREATE INDEX onek_unique1 ON onek USING btree(unique1 int4_ops);

CREATE INDEX IF NOT EXISTS onek_unique1 ON onek USING btree(unique1 int4_ops);

CREATE INDEX IF NOT EXISTS ON onek USING btree(unique1 int4_ops);

CREATE INDEX onek_unique2 ON onek USING btree(unique2 int4_ops);

CREATE INDEX onek_hundred ON onek USING btree(hundred int4_ops);

CREATE INDEX onek_stringu1 ON onek USING btree(stringu1 name_ops);

CREATE INDEX tenk1_unique1 ON tenk1 USING btree(unique1 int4_ops);

CREATE INDEX tenk1_unique2 ON tenk1 USING btree(unique2 int4_ops);

CREATE INDEX tenk1_hundred ON tenk1 USING btree(hundred int4_ops);

CREATE INDEX tenk1_thous_tenthous ON tenk1 (thousand, tenthous);

CREATE INDEX tenk2_unique1 ON tenk2 USING btree(unique1 int4_ops);

CREATE INDEX tenk2_unique2 ON tenk2 USING btree(unique2 int4_ops);

CREATE INDEX tenk2_hundred ON tenk2 USING btree(hundred int4_ops);

CREATE INDEX rix ON road USING btree (name text_ops);

CREATE INDEX iix ON ihighway USING btree (name text_ops);

CREATE INDEX six ON shighway USING btree (name text_ops);

-- test comments
COMMENT ON INDEX six_wrong IS 'bad index';
COMMENT ON INDEX six IS 'good index';
COMMENT ON INDEX six IS NULL;
SELECT obj_description('six'::regclass, 'pg_class') IS NULL AS six_comment_is_null;
COMMENT ON INDEX six IS 'add the comment back';
COMMENT ON INDEX six IS ''; -- empty string removes the comment, same as NULL
SELECT obj_description('six'::regclass, 'pg_class') IS NULL AS six_comment_is_null;

--
-- BTREE partial indices
--
CREATE INDEX onek2_u1_prtl ON onek2 USING btree(unique1 int4_ops)
	where unique1 < 20 or unique1 > 980;

CREATE INDEX onek2_u2_prtl ON onek2 USING btree(unique2 int4_ops)
	where stringu1 < 'B';

CREATE INDEX onek2_stu1_prtl ON onek2 USING btree(stringu1 name_ops)
	where onek2.stringu1 >= 'J' and onek2.stringu1 < 'K';

--
-- GiST (rtree-equivalent opclasses only)
--

CREATE TABLE slow_emp4000 (
	home_base	 box
);

CREATE TABLE fast_emp4000 (
	home_base	 box
);

\set filename :abs_srcdir '/data/rect.data'
COPY slow_emp4000 FROM :'filename';

INSERT INTO fast_emp4000 SELECT * FROM slow_emp4000;

ANALYZE slow_emp4000;
ANALYZE fast_emp4000;

CREATE INDEX grect2ind ON fast_emp4000 USING gist (home_base);

-- we want to work with a point_tbl that includes a null
CREATE TEMP TABLE point_tbl AS SELECT * FROM public.point_tbl;
INSERT INTO POINT_TBL(f1) VALUES (NULL);

CREATE INDEX gpointind ON point_tbl USING gist (f1);

CREATE TEMP TABLE gpolygon_tbl AS
    SELECT polygon(home_base) AS f1 FROM slow_emp4000;
INSERT INTO gpolygon_tbl VALUES ( '(1000,0,0,1000)' );
INSERT INTO gpolygon_tbl VALUES ( '(0,1000,1000,1000)' );

CREATE TEMP TABLE gcircle_tbl AS
    SELECT circle(home_base) AS f1 FROM slow_emp4000;

CREATE INDEX ggpolygonind ON gpolygon_tbl USING gist (f1);

CREATE INDEX ggcircleind ON gcircle_tbl USING gist (f1);

--
-- Test GiST indexes
--

-- get non-indexed results for comparison purposes

SET enable_seqscan = ON;
SET enable_indexscan = OFF;
SET enable_bitmapscan = OFF;

SELECT * FROM fast_emp4000
    WHERE home_base <@ '(200,200),(2000,1000)'::box
    ORDER BY (home_base[0])[0];

SELECT count(*) FROM fast_emp4000 WHERE home_base && '(1000,1000,0,0)'::box;

SELECT count(*) FROM fast_emp4000 WHERE home_base IS NULL;

SELECT count(*) FROM gpolygon_tbl WHERE f1 && '(1000,1000,0,0)'::polygon;

SELECT count(*) FROM gcircle_tbl WHERE f1 && '<(500,500),500>'::circle;

SELECT count(*) FROM point_tbl WHERE f1 <@ box '(0,0,100,100)';

SELECT count(*) FROM point_tbl WHERE box '(0,0,100,100)' @> f1;

SELECT count(*) FROM point_tbl WHERE f1 <@ polygon '(0,0),(0,100),(100,100),(50,50),(100,0),(0,0)';

SELECT count(*) FROM point_tbl WHERE f1 <@ circle '<(50,50),50>';

SELECT count(*) FROM point_tbl p WHERE p.f1 << '(0.0, 0.0)';

SELECT count(*) FROM point_tbl p WHERE p.f1 >> '(0.0, 0.0)';

SELECT count(*) FROM point_tbl p WHERE p.f1 <<| '(0.0, 0.0)';

SELECT count(*) FROM point_tbl p WHERE p.f1 |>> '(0.0, 0.0)';

SELECT count(*) FROM point_tbl p WHERE p.f1 ~= '(-5, -12)';

SELECT * FROM point_tbl ORDER BY f1 <-> '0,1';

SELECT * FROM point_tbl WHERE f1 IS NULL;

SELECT * FROM point_tbl WHERE f1 IS NOT NULL ORDER BY f1 <-> '0,1';

SELECT * FROM point_tbl WHERE f1 <@ '(-10,-10),(10,10)':: box ORDER BY f1 <-> '0,1';

SELECT * FROM gpolygon_tbl ORDER BY f1 <-> '(0,0)'::point LIMIT 10;

SELECT circle_center(f1), round(radius(f1)) as radius FROM gcircle_tbl ORDER BY f1 <-> '(200,300)'::point LIMIT 10;

-- Now check the results from plain indexscan
SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;

EXPLAIN (COSTS OFF)
SELECT * FROM fast_emp4000
    WHERE home_base <@ '(200,200),(2000,1000)'::box
    ORDER BY (home_base[0])[0];
SELECT * FROM fast_emp4000
    WHERE home_base <@ '(200,200),(2000,1000)'::box
    ORDER BY (home_base[0])[0];

EXPLAIN (COSTS OFF)
SELECT count(*) FROM fast_emp4000 WHERE home_base && '(1000,1000,0,0)'::box;
SELECT count(*) FROM fast_emp4000 WHERE home_base && '(1000,1000,0,0)'::box;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM fast_emp4000 WHERE home_base IS NULL;
SELECT count(*) FROM fast_emp4000 WHERE home_base IS NULL;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM gpolygon_tbl WHERE f1 && '(1000,1000,0,0)'::polygon;
SELECT count(*) FROM gpolygon_tbl WHERE f1 && '(1000,1000,0,0)'::polygon;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM gcircle_tbl WHERE f1 && '<(500,500),500>'::circle;
SELECT count(*) FROM gcircle_tbl WHERE f1 && '<(500,500),500>'::circle;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl WHERE f1 <@ box '(0,0,100,100)';
SELECT count(*) FROM point_tbl WHERE f1 <@ box '(0,0,100,100)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl WHERE box '(0,0,100,100)' @> f1;
SELECT count(*) FROM point_tbl WHERE box '(0,0,100,100)' @> f1;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl WHERE f1 <@ polygon '(0,0),(0,100),(100,100),(50,50),(100,0),(0,0)';
SELECT count(*) FROM point_tbl WHERE f1 <@ polygon '(0,0),(0,100),(100,100),(50,50),(100,0),(0,0)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl WHERE f1 <@ circle '<(50,50),50>';
SELECT count(*) FROM point_tbl WHERE f1 <@ circle '<(50,50),50>';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl p WHERE p.f1 << '(0.0, 0.0)';
SELECT count(*) FROM point_tbl p WHERE p.f1 << '(0.0, 0.0)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl p WHERE p.f1 >> '(0.0, 0.0)';
SELECT count(*) FROM point_tbl p WHERE p.f1 >> '(0.0, 0.0)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl p WHERE p.f1 <<| '(0.0, 0.0)';
SELECT count(*) FROM point_tbl p WHERE p.f1 <<| '(0.0, 0.0)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl p WHERE p.f1 |>> '(0.0, 0.0)';
SELECT count(*) FROM point_tbl p WHERE p.f1 |>> '(0.0, 0.0)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl p WHERE p.f1 ~= '(-5, -12)';
SELECT count(*) FROM point_tbl p WHERE p.f1 ~= '(-5, -12)';

EXPLAIN (COSTS OFF)
SELECT * FROM point_tbl ORDER BY f1 <-> '0,1';
SELECT * FROM point_tbl ORDER BY f1 <-> '0,1';

EXPLAIN (COSTS OFF)
SELECT * FROM point_tbl WHERE f1 IS NULL;
SELECT * FROM point_tbl WHERE f1 IS NULL;

EXPLAIN (COSTS OFF)
SELECT * FROM point_tbl WHERE f1 IS NOT NULL ORDER BY f1 <-> '0,1';
SELECT * FROM point_tbl WHERE f1 IS NOT NULL ORDER BY f1 <-> '0,1';

EXPLAIN (COSTS OFF)
SELECT * FROM point_tbl WHERE f1 <@ '(-10,-10),(10,10)':: box ORDER BY f1 <-> '0,1';
SELECT * FROM point_tbl WHERE f1 <@ '(-10,-10),(10,10)':: box ORDER BY f1 <-> '0,1';

EXPLAIN (COSTS OFF)
SELECT * FROM gpolygon_tbl ORDER BY f1 <-> '(0,0)'::point LIMIT 10;
SELECT * FROM gpolygon_tbl ORDER BY f1 <-> '(0,0)'::point LIMIT 10;

EXPLAIN (COSTS OFF)
SELECT circle_center(f1), round(radius(f1)) as radius FROM gcircle_tbl ORDER BY f1 <-> '(200,300)'::point LIMIT 10;
SELECT circle_center(f1), round(radius(f1)) as radius FROM gcircle_tbl ORDER BY f1 <-> '(200,300)'::point LIMIT 10;

EXPLAIN (COSTS OFF)
SELECT point(x,x), (SELECT f1 FROM gpolygon_tbl ORDER BY f1 <-> point(x,x) LIMIT 1) as c FROM generate_series(0,10,1) x;
SELECT point(x,x), (SELECT f1 FROM gpolygon_tbl ORDER BY f1 <-> point(x,x) LIMIT 1) as c FROM generate_series(0,10,1) x;

-- Now check the results from bitmap indexscan
SET enable_seqscan = OFF;
SET enable_indexscan = OFF;
SET enable_bitmapscan = ON;

EXPLAIN (COSTS OFF)
SELECT * FROM point_tbl WHERE f1 <@ '(-10,-10),(10,10)':: box ORDER BY f1 <-> '0,1';
SELECT * FROM point_tbl WHERE f1 <@ '(-10,-10),(10,10)':: box ORDER BY f1 <-> '0,1';

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

--
-- GIN over int[] and text[]
--
-- Note: GIN currently supports only bitmap scans, not plain indexscans
--

CREATE TABLE array_index_op_test (
	seqno		int4,
	i			int4[],
	t			text[]
);

\set filename :abs_srcdir '/data/array.data'
COPY array_index_op_test FROM :'filename';
ANALYZE array_index_op_test;

SELECT * FROM array_index_op_test WHERE i = '{NULL}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{NULL}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{NULL}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i <@ '{NULL}' ORDER BY seqno;

SET enable_seqscan = OFF;
SET enable_indexscan = OFF;
SET enable_bitmapscan = ON;

CREATE INDEX intarrayidx ON array_index_op_test USING gin (i);

explain (costs off)
SELECT * FROM array_index_op_test WHERE i @> '{32}' ORDER BY seqno;

SELECT * FROM array_index_op_test WHERE i @> '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{32,17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32,17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i <@ '{38,34,32,89}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i = '{47,77}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i = '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i <@ '{}' ORDER BY seqno;

CREATE INDEX textarrayidx ON array_index_op_test USING gin (t);

explain (costs off)
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAAA72908}' ORDER BY seqno;

SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAAA72908}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAAA72908}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAAA72908,AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAAA72908,AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t <@ '{AAAAAAAA72908,AAAAAAAAAAAAAAAAAAA17075,AA88409,AAAAAAAAAAAAAAAAAA36842,AAAAAAA48038,AAAAAAAAAAAAAA10611}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t = '{AAAAAAAAAA646,A87088}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t = '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t <@ '{}' ORDER BY seqno;

-- And try it with a multicolumn GIN index

DROP INDEX intarrayidx, textarrayidx;

CREATE INDEX botharrayidx ON array_index_op_test USING gin (i, t);

SELECT * FROM array_index_op_test WHERE i @> '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{32}' AND t && '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' AND t @> '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t = '{}' ORDER BY seqno;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

--
-- Try a GIN index with a lot of items with same key. (GIN creates a posting
-- tree when there are enough duplicates)
--
CREATE TABLE array_gin_test (a int[]);

INSERT INTO array_gin_test SELECT ARRAY[1, g%5, g] FROM generate_series(1, 10000) g;

CREATE INDEX array_gin_test_idx ON array_gin_test USING gin (a);

SELECT COUNT(*) FROM array_gin_test WHERE a @> '{2}';

DROP TABLE array_gin_test;

--
-- Test GIN index's reloptions
--
CREATE INDEX gin_relopts_test ON array_index_op_test USING gin (i)
  WITH (FASTUPDATE=on, GIN_PENDING_LIST_LIMIT=128);
\d+ gin_relopts_test

--
-- HASH
--
CREATE UNLOGGED TABLE unlogged_hash_table (id int4);
CREATE INDEX unlogged_hash_index ON unlogged_hash_table USING hash (id int4_ops);
DROP TABLE unlogged_hash_table;

-- CREATE INDEX hash_ovfl_index ON hash_ovfl_heap USING hash (x int4_ops);

-- Test hash index build tuplesorting.  Force hash tuplesort using low
-- maintenance_work_mem setting and fillfactor:
SET maintenance_work_mem = '1MB';
CREATE INDEX hash_tuplesort_idx ON tenk1 USING hash (stringu1 name_ops) WITH (fillfactor = 10);
EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1 WHERE stringu1 = 'TVAAAA';
SELECT count(*) FROM tenk1 WHERE stringu1 = 'TVAAAA';
-- OR-clauses shouldn't be transformed into SAOP because hash indexes don't
-- support SAOP scans.
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT COUNT(*) FROM tenk1 WHERE stringu1 = 'TVAAAA' OR  stringu1 = 'TVAAAB';
RESET enable_seqscan;
DROP INDEX hash_tuplesort_idx;
RESET maintenance_work_mem;


--
-- Test unique null behavior
--
CREATE TABLE ci_unique_tbl (i int, t text);

CREATE UNIQUE INDEX unique_idx1 ON ci_unique_tbl (i) NULLS DISTINCT;
CREATE UNIQUE INDEX unique_idx2 ON ci_unique_tbl (i) NULLS NOT DISTINCT;

INSERT INTO ci_unique_tbl VALUES (1, 'one');
INSERT INTO ci_unique_tbl VALUES (2, 'two');
INSERT INTO ci_unique_tbl VALUES (3, 'three');
INSERT INTO ci_unique_tbl VALUES (4, 'four');
INSERT INTO ci_unique_tbl VALUES (5, 'one');
INSERT INTO ci_unique_tbl (t) VALUES ('six');
INSERT INTO ci_unique_tbl (t) VALUES ('seven');  -- error from unique_idx2

DROP INDEX unique_idx1, unique_idx2;

INSERT INTO ci_unique_tbl (t) VALUES ('seven');

-- build indexes on filled table
CREATE UNIQUE INDEX unique_idx3 ON ci_unique_tbl (i) NULLS DISTINCT;  -- ok
CREATE UNIQUE INDEX unique_idx4 ON ci_unique_tbl (i) NULLS NOT DISTINCT;  -- error

DELETE FROM ci_unique_tbl WHERE t = 'seven';

CREATE UNIQUE INDEX unique_idx4 ON ci_unique_tbl (i) NULLS NOT DISTINCT;  -- ok now

\d ci_unique_tbl
\d unique_idx3
\d unique_idx4
SELECT pg_get_indexdef('unique_idx3'::regclass);
SELECT pg_get_indexdef('unique_idx4'::regclass);

DROP TABLE ci_unique_tbl;


--
-- Test functional index
--
CREATE TABLE func_index_heap (f1 text, f2 text);
CREATE UNIQUE INDEX func_index_index on func_index_heap (textcat(f1,f2));

INSERT INTO func_index_heap VALUES('ABC','DEF');
INSERT INTO func_index_heap VALUES('AB','CDEFG');
INSERT INTO func_index_heap VALUES('QWE','RTY');
-- this should fail because of unique index:
INSERT INTO func_index_heap VALUES('ABCD', 'EF');
-- but this shouldn't:
INSERT INTO func_index_heap VALUES('QWERTY');

-- while we're here, see that the metadata looks sane
\d func_index_heap
\d func_index_index


--
-- Same test, expressional index
--
DROP TABLE func_index_heap;
CREATE TABLE func_index_heap (f1 text, f2 text);
CREATE UNIQUE INDEX func_index_index on func_index_heap ((f1 || f2) text_ops);

INSERT INTO func_index_heap VALUES('ABC','DEF');
INSERT INTO func_index_heap VALUES('AB','CDEFG');
INSERT INTO func_index_heap VALUES('QWE','RTY');
-- this should fail because of unique index:
INSERT INTO func_index_heap VALUES('ABCD', 'EF');
-- but this shouldn't:
INSERT INTO func_index_heap VALUES('QWERTY');

-- this should fail because of unsafe column type (anonymous record)
create index on func_index_heap ((f1 || f2), (row(f1, f2)));

-- but this is allowed:
create index on func_index_heap ((func_index_heap.*));

-- while we're here, see that the metadata looks sane
\d func_index_heap
\d func_index_index
\d func_index_heap_func_index_heap_idx


--
-- Test unique index with included columns
--
CREATE TABLE covering_index_heap (f1 int, f2 int, f3 text);
CREATE UNIQUE INDEX covering_index_index on covering_index_heap (f1,f2) INCLUDE(f3);

INSERT INTO covering_index_heap VALUES(1,1,'AAA');
INSERT INTO covering_index_heap VALUES(1,2,'AAA');
-- this should fail because of unique index on f1,f2:
INSERT INTO covering_index_heap VALUES(1,2,'BBB');
-- and this shouldn't:
INSERT INTO covering_index_heap VALUES(1,4,'AAA');
-- Try to build index on table that already contains data
CREATE UNIQUE INDEX covering_pkey on covering_index_heap (f1,f2) INCLUDE(f3);
-- Try to use existing covering index as primary key
ALTER TABLE covering_index_heap ADD CONSTRAINT covering_pkey PRIMARY KEY USING INDEX
covering_pkey;
DROP TABLE covering_index_heap;

--
-- Test ADD CONSTRAINT USING INDEX
--

CREATE TABLE cwi_test( a int , b varchar(10), c char);

-- add some data so that all tests have something to work with.

INSERT INTO cwi_test VALUES(1, 2), (3, 4), (5, 6);

CREATE UNIQUE INDEX cwi_uniq_idx ON cwi_test(a , b);
ALTER TABLE cwi_test ADD primary key USING INDEX cwi_uniq_idx;

\d cwi_test
\d cwi_uniq_idx

CREATE UNIQUE INDEX cwi_uniq2_idx ON cwi_test(b , a);
ALTER TABLE cwi_test DROP CONSTRAINT cwi_uniq_idx,
	ADD CONSTRAINT cwi_replaced_pkey PRIMARY KEY
		USING INDEX cwi_uniq2_idx;

\d cwi_test
\d cwi_replaced_pkey

DROP INDEX cwi_replaced_pkey;	-- Should fail; a constraint depends on it

-- Check that non-default index options are rejected
CREATE UNIQUE INDEX cwi_uniq3_idx ON cwi_test(a desc);
ALTER TABLE cwi_test ADD UNIQUE USING INDEX cwi_uniq3_idx;  -- fail
CREATE UNIQUE INDEX cwi_uniq4_idx ON cwi_test(b collate "POSIX");
ALTER TABLE cwi_test ADD UNIQUE USING INDEX cwi_uniq4_idx;  -- fail

DROP TABLE cwi_test;

-- ADD CONSTRAINT USING INDEX is forbidden on partitioned tables
CREATE TABLE cwi_test(a int) PARTITION BY hash (a);
create unique index on cwi_test (a);
alter table cwi_test add primary key using index cwi_test_a_idx ;
DROP TABLE cwi_test;

-- PRIMARY KEY constraint cannot be backed by a NULLS NOT DISTINCT index
CREATE TABLE cwi_test(a int, b int);
CREATE UNIQUE INDEX cwi_a_nnd ON cwi_test (a) NULLS NOT DISTINCT;
ALTER TABLE cwi_test ADD PRIMARY KEY USING INDEX cwi_a_nnd;
DROP TABLE cwi_test;

--
-- Check handling of indexes on system columns
--
CREATE TABLE syscol_table (a INT);

-- System columns cannot be indexed
CREATE INDEX ON syscol_table (ctid);

-- nor used in expressions
CREATE INDEX ON syscol_table ((ctid >= '(1000,0)'));

-- nor used in predicates
CREATE INDEX ON syscol_table (a) WHERE ctid >= '(1000,0)';

DROP TABLE syscol_table;

--
-- Tests for IS NULL/IS NOT NULL with b-tree indexes
--

CREATE TABLE onek_with_null AS SELECT unique1, unique2 FROM onek;
INSERT INTO onek_with_null(unique1, unique2)
VALUES (NULL, -1), (NULL, 2_147_483_647), (NULL, NULL),
       (100, NULL), (500, NULL);
CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2,unique1);

SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = ON;

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
SELECT unique1, unique2 FROM onek_with_null WHERE unique1 = 500 ORDER BY unique2 DESC, unique1 DESC LIMIT 1;

DROP INDEX onek_nulltest;

CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2 desc,unique1);

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IN (-1, 0, 1);
SELECT unique1, unique2 FROM onek_with_null WHERE unique1 = 500 ORDER BY unique2 DESC, unique1 DESC LIMIT 1;

DROP INDEX onek_nulltest;

CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2 desc nulls last,unique1);

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
SELECT unique1, unique2 FROM onek_with_null WHERE unique1 = 500 ORDER BY unique2 DESC, unique1 DESC LIMIT 1;

DROP INDEX onek_nulltest;

CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2  nulls first,unique1);

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
SELECT unique1, unique2 FROM onek_with_null WHERE unique1 = 500 ORDER BY unique2 DESC, unique1 DESC LIMIT 1;

DROP INDEX onek_nulltest;

-- Check initial-positioning logic too

CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2);

SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;

SELECT unique1, unique2 FROM onek_with_null
  ORDER BY unique2 LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 >= -1
  ORDER BY unique2 LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 >= 0
  ORDER BY unique2 LIMIT 2;

SELECT unique1, unique2 FROM onek_with_null
  ORDER BY unique2 DESC LIMIT 5;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 >= -1
  ORDER BY unique2 DESC LIMIT 3;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 < 999
  ORDER BY unique2 DESC LIMIT 2;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

DROP TABLE onek_with_null;

--
-- Check bitmap index path planning
--

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1 OR tenthous = 3 OR tenthous = 42 OR tenthous = 0);
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1 OR tenthous = 3 OR tenthous = 42 OR tenthous = 0);

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1 OR tenthous = (SELECT 1 + 2) OR tenthous = 42);
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1 OR tenthous = (SELECT 1 + 2) OR tenthous = 42);

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1 OR tenthous = 3 OR tenthous = 42 OR tenthous IS NULL);

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1::int2 OR tenthous::int2 = 3::int8 OR tenthous = 42::int8);

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1::int2 OR tenthous::int2 = 3::int8 OR tenthous::int2 = 42::int8);


EXPLAIN (COSTS OFF)
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1::int2 OR tenthous = 3::int8 OR tenthous = 42::int8);

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand = 42 OR thousand = 99);
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand = 42 OR thousand = 99);

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1 OR tenthous = 3 OR tenthous = 42);
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1 OR tenthous = 3 OR tenthous = 42);

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1::numeric OR tenthous = 3::int4 OR tenthous = 42::numeric);

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1
  WHERE tenthous = 1::numeric OR tenthous = 3::int4 OR tenthous = 42::numeric;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1 t1
  WHERE t1.thousand = 42 OR t1.thousand = (SELECT t2.tenthous FROM tenk1 t2 WHERE t2.thousand = t1.tenthous + 1 LIMIT 1);
SELECT count(*) FROM tenk1 t1
  WHERE t1.thousand = 42 OR t1.thousand = (SELECT t2.tenthous FROM tenk1 t2 WHERE t2.thousand = t1.tenthous + 1 LIMIT 1);

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand = 42 OR thousand = 99);
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand = 42 OR thousand = 99);

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand < 42 OR thousand < 99 OR 43 > thousand OR 42 > thousand);
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand < 42 OR thousand < 99 OR 43 > thousand OR 42 > thousand);

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1 OR tenthous = 3) OR thousand = 41;
SELECT count(*) FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1 OR tenthous = 3) OR thousand = 41;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand = 42 OR thousand = 99 OR tenthous < 2) OR thousand = 41;
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand = 42 OR thousand = 99 OR tenthous < 2) OR thousand = 41;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand = 42 OR thousand = 41 OR thousand = 99 AND tenthous = 2);
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand = 42 OR thousand = 41 OR thousand = 99 AND tenthous = 2);

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1, tenk2
  WHERE tenk1.hundred = 42 AND (tenk2.thousand = 42 OR tenk1.thousand = 41 OR tenk2.tenthous = 2) AND
  tenk2.hundred = tenk1.hundred;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1, tenk2
  WHERE tenk1.hundred = 42 AND (tenk2.thousand = 42 OR tenk2.thousand = 41 OR tenk2.tenthous = 2) AND
  tenk2.hundred = tenk1.hundred;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1 JOIN tenk2 ON
  tenk1.hundred = 42 AND (tenk2.thousand = 42 OR tenk2.thousand = 41 OR tenk2.tenthous = 2) AND
  tenk2.hundred = tenk1.hundred;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1 LEFT JOIN tenk2 ON
  tenk1.hundred = 42 AND (tenk2.thousand = 42 OR tenk2.thousand = 41 OR tenk2.tenthous = 2) AND
  tenk2.hundred = tenk1.hundred;
--
-- Check behavior with duplicate index column contents
--

CREATE TABLE dupindexcols AS
  SELECT unique1 as id, stringu2::text as f1 FROM tenk1;
CREATE INDEX dupindexcols_i ON dupindexcols (f1, id, f1 text_pattern_ops);
ANALYZE dupindexcols;

EXPLAIN (COSTS OFF)
  SELECT count(*) FROM dupindexcols
    WHERE f1 BETWEEN 'WA' AND 'ZZZ' and id < 1000 and f1 ~<~ 'YX';
SELECT count(*) FROM dupindexcols
  WHERE f1 BETWEEN 'WA' AND 'ZZZ' and id < 1000 and f1 ~<~ 'YX';

--
-- Check that index scans with SAOP array and/or skip array indexquals
-- return rows in index order
--

explain (costs off)
SELECT unique1 FROM tenk1
WHERE unique1 IN (1,42,7)
ORDER BY unique1;

SELECT unique1 FROM tenk1
WHERE unique1 IN (1,42,7)
ORDER BY unique1;

-- Skip array on "thousand", SAOP array on "tenthous":
explain (costs off)
SELECT thousand, tenthous FROM tenk1
WHERE thousand < 2 AND tenthous IN (1001,3000)
ORDER BY thousand;

SELECT thousand, tenthous FROM tenk1
WHERE thousand < 2 AND tenthous IN (1001,3000)
ORDER BY thousand;

-- Skip array on "thousand", SAOP array on "tenthous", backward scan:
explain (costs off)
SELECT thousand, tenthous FROM tenk1
WHERE thousand < 2 AND tenthous IN (1001,3000)
ORDER BY thousand DESC, tenthous DESC;

SELECT thousand, tenthous FROM tenk1
WHERE thousand < 2 AND tenthous IN (1001,3000)
ORDER BY thousand DESC, tenthous DESC;

explain (costs off)
SELECT thousand, tenthous FROM tenk1
WHERE thousand > 995 and tenthous in (998, 999)
ORDER BY thousand desc;

SELECT thousand, tenthous FROM tenk1
WHERE thousand > 995 and tenthous in (998, 999)
ORDER BY thousand desc;

--
-- Check elimination of redundant and contradictory index quals
--
explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 = ANY('{7, 8, 9}');

SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 = ANY('{7, 8, 9}');

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 = ANY('{7, 14, 22}') and unique1 = ANY('{33, 44}'::bigint[]);

SELECT unique1 FROM tenk1 WHERE unique1 = ANY('{7, 14, 22}') and unique1 = ANY('{33, 44}'::bigint[]);

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 = ANY(NULL);

SELECT unique1 FROM tenk1 WHERE unique1 = ANY(NULL);

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 = ANY('{NULL,NULL,NULL}');

SELECT unique1 FROM tenk1 WHERE unique1 = ANY('{NULL,NULL,NULL}');

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 IS NULL AND unique1 IS NULL;

SELECT unique1 FROM tenk1 WHERE unique1 IS NULL AND unique1 IS NULL;

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 = 1;

SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 = 1;

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 = 12345;

SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 = 12345;

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 >= 42;

SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 >= 42;

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 > 42;

SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 > 42;

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 > 9996 and unique1 >= 9999;

SELECT unique1 FROM tenk1 WHERE unique1 > 9996 and unique1 >= 9999;

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 < 3 and unique1 <= 3;

SELECT unique1 FROM tenk1 WHERE unique1 < 3 and unique1 <= 3;

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 < 3 and unique1 < (-1)::bigint;

SELECT unique1 FROM tenk1 WHERE unique1 < 3 and unique1 < (-1)::bigint;

explain (costs off)
SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 < (-1)::bigint;

SELECT unique1 FROM tenk1 WHERE unique1 IN (1, 42, 7) and unique1 < (-1)::bigint;

explain (costs off)
SELECT unique1 FROM tenk1 WHERE (thousand, tenthous) > (NULL, 5);

SELECT unique1 FROM tenk1 WHERE (thousand, tenthous) > (NULL, 5);

-- Skip array redundancy (pair of redundant low_compare inequalities)
explain (costs off)
SELECT thousand, tenthous FROM tenk1
WHERE thousand > -1 and thousand >= 0 AND tenthous = 3000
ORDER BY thousand;

SELECT thousand, tenthous FROM tenk1
WHERE thousand > -1 and thousand >= 0 AND tenthous = 3000
ORDER BY thousand;

-- Skip array redundancy (pair of redundant high_compare inequalities)
explain (costs off)
SELECT thousand, tenthous FROM tenk1
WHERE thousand < 3 and thousand <= 2 AND tenthous = 1001
ORDER BY thousand;

SELECT thousand, tenthous FROM tenk1
WHERE thousand < 3 and thousand <= 2 AND tenthous = 1001
ORDER BY thousand;

-- Skip array preprocessing increments "thousand > -1" to  "thousand >= 0"
explain (costs off)
SELECT thousand, tenthous FROM tenk1
WHERE thousand > -1 AND tenthous IN (1001,3000)
ORDER BY thousand limit 2;

SELECT thousand, tenthous FROM tenk1
WHERE thousand > -1 AND tenthous IN (1001,3000)
ORDER BY thousand limit 2;

--
-- Check elimination of constant-NULL subexpressions
--

explain (costs off)
  select * from tenk1 where (thousand, tenthous) in ((1,1001), (null,null));

--
-- Check matching of boolean index columns to WHERE conditions and sort keys
--

create temp table boolindex (b bool, i int, unique(b, i), junk float);

explain (costs off)
  select * from boolindex order by b, i limit 10;
explain (costs off)
  select * from boolindex where b order by i limit 10;
explain (costs off)
  select * from boolindex where b = true order by i desc limit 10;
explain (costs off)
  select * from boolindex where not b order by i limit 10;
explain (costs off)
  select * from boolindex where b is true order by i desc limit 10;
explain (costs off)
  select * from boolindex where b is false order by i desc limit 10;

-- No OR-clause groupings should happen, and there should be no clause
-- permutations in the filtering conditions we could see in the EXPLAIN.
EXPLAIN (COSTS OFF)
SELECT * FROM tenk1 WHERE unique1 < 1 OR hundred < 2;

-- OR clauses in the 'unique1' column are grouped, so clause permutation
-- occurs. W e can see it in the 'Recheck Cond': the second clause involving
-- the 'unique1' column goes just after the first one.
EXPLAIN (COSTS OFF)
SELECT * FROM tenk1 WHERE unique1 < 1 OR unique1 < 3 OR hundred < 2;

-- Check bitmap scan can consider similar OR arguments separately without
-- grouping them into SAOP.
CREATE TABLE bitmap_split_or (a int NOT NULL, b int NOT NULL, c int NOT NULL);
INSERT INTO bitmap_split_or (SELECT 1, 1, i FROM generate_series(1, 1000) i);
INSERT INTO bitmap_split_or (select i, 2, 2 FROM generate_series(1, 1000) i);
VACUUM ANALYZE bitmap_split_or;
CREATE INDEX t_b_partial_1_idx ON bitmap_split_or (b) WHERE a = 1;
CREATE INDEX t_b_partial_2_idx ON bitmap_split_or (b) WHERE a = 2;
EXPLAIN (COSTS OFF)
SELECT * FROM bitmap_split_or WHERE (a = 1 OR a = 2) AND b = 2;
DROP INDEX t_b_partial_1_idx;
DROP INDEX t_b_partial_2_idx;
CREATE INDEX t_a_b_idx ON bitmap_split_or (a, b);
CREATE INDEX t_b_c_idx ON bitmap_split_or (b, c);
CREATE STATISTICS t_a_b_stat (mcv) ON a, b FROM bitmap_split_or;
CREATE STATISTICS t_b_c_stat (mcv) ON b, c FROM bitmap_split_or;
ANALYZE bitmap_split_or;
EXPLAIN (COSTS OFF)
SELECT * FROM bitmap_split_or t1, bitmap_split_or t2
WHERE t1.a = t2.b OR t1.a = 2;
EXPLAIN (COSTS OFF)
SELECT * FROM bitmap_split_or WHERE a = 1 AND (b = 1 OR b = 2) AND c = 2;
DROP TABLE bitmap_split_or;
