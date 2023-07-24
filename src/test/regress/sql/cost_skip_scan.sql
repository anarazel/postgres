set work_mem='100MB';
set effective_cache_size='24GB';
set random_page_cost=2.0;
set track_io_timing to off;
set enable_seqscan to off;
set client_min_messages=error;
set max_parallel_workers_per_gather=0;
-- set skipscan_skipsupport_enabled=false;
-- set skipscan_prefix_cols=0;
set vacuum_freeze_min_age = 0;
set cursor_tuple_fraction=1.000;
set default_statistics_target=10000;
set statement_timeout='30s';
create extension if not exists pageinspect; -- just to have it
reset client_min_messages;

-- Set log_btree_verbosity to 1 without depending on having that patch
-- applied (HACK, just sets commit_siblings instead when we don't have that
-- patch available):
select set_config((select coalesce((select name from pg_settings where name = 'log_btree_verbosity'), 'commit_siblings')), '1', false);

-- Establish if this server is master or the patch -- want to skip stress
-- tests if it's the latter
--
-- Reminder: Don't vary the database state between master and patch (just the
-- tests run, which must be read-only)
select (setting = '5432') as testing_patch from pg_settings where name = 'port'
       \gset

select setseed(0.5);

-- Quick sanity check, to make it obvious when you forgot to initdb correctly:
-- Shows the available skip support routines in the database
select
  amp.oid as skip_proc_oid,
  amp.amproc::regproc as proc,
  opf.opfname as opfamily_name,
  opc.opcname as opclass_name,
  opc.opcintype::regtype as opcintype
from pg_am as am
join pg_opclass as opc on opc.opcmethod = am.oid
join pg_opfamily as opf on opc.opcfamily = opf.oid
join pg_amproc as amp on amp.amprocfamily = opf.oid and
    amp.amproclefttype = opc.opcintype and amp.amprocnum = 6
where am.amname = 'btree'
order by 1, 2, 3, 4;

----------------------
-- Cost model tests --
----------------------
set client_min_messages=error;
drop table if exists cost_of_skipping;
reset client_min_messages;

-- Uniformly random, unlike tenk1 table there are no cross-column correlations
-- that slightly violate our assumptions inside selfuncs.c:
create unlogged table cost_of_skipping as
select
  i,
  (random() * 9)::int4 ten,
  (random() * 99)::int4 hundred,
  (random() * 999)::int4 thousand,
  (random() * 999)::int4 ten_thousand,
  i::text || repeat('A', 50) as empty_payload -- don't want index-only scans for "select *" queries
from
  generate_series(1, 1_000_000) i;
create index on cost_of_skipping (ten, hundred, thousand, ten_thousand, i);
vacuum analyze cost_of_skipping;

set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Don't change cost of this compared to master:
select count(*) from cost_of_skipping where ten between 8 and 9;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from cost_of_skipping where ten between 8 and 9;

-- Don't change cost of this compared to master:
select count(*) from cost_of_skipping where ten between 9 and 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from cost_of_skipping where ten between 9 and 10;

-------------------------------------------------------------------------------
-- As selectivity increases, we want to see a commensurate increase in costs --
-------------------------------------------------------------------------------

-- # 1
select *
from cost_of_skipping
where
  ten between 1 and 1
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten between 1 and 1
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
-- # 1 SAOP
select *
from cost_of_skipping
where
  ten in (1)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten in (1)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;

-- # 2
select *
from cost_of_skipping
where
  ten between 1 and 2
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten between 1 and 2
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
-- # 2 SAOP
select *
from cost_of_skipping
where
  ten in (1, 2)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten in (1, 2)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;

-- # 3
select *
from cost_of_skipping
where
  ten between 1 and 3
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten between 1 and 3
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
-- # 3 SAOP
select *
from cost_of_skipping
where
  ten in (1, 2, 3)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten in (1, 2, 3)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;

-- # 4
select *
from cost_of_skipping
where
  ten between 1 and 4
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten between 1 and 4
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
-- # 4 SAOP
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;

-- # 5
select *
from cost_of_skipping
where
  ten between 1 and 5
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten between 1 and 5
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
-- # 5 SAOP
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;

-- # 6
select *
from cost_of_skipping
where
  ten between 1 and 6
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten between 1 and 6
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
-- # 6 SAOP
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5, 6)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5, 6)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;

-- # 7
select *
from cost_of_skipping
where
  ten between 1 and 7
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten between 1 and 7
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
-- # 7 SAOP
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5, 6, 7)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5, 6, 7)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;

-- # 8
select *
from cost_of_skipping
where
  ten between 1 and 8
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten between 1 and 8
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
-- # 8 SAOP
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5, 6, 7, 8)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5, 6, 7, 8)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;

-- # 9
select *
from cost_of_skipping
where
  ten between 1 and 9
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten between 1 and 9
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
-- # 9 SAOP
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5, 6, 7, 8, 9)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5, 6, 7, 8, 9)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;

-- # 10
select *
from cost_of_skipping
where
  ten between 1 and 10
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten between 1 and 10
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
-- # 10 SAOP
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_of_skipping
where
  ten in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
  and hundred = 4 and thousand = 35 and ten_thousand = 36 and i = 441;

-----------------------------------------------------------------------------------
-- END As selectivity increases, we want to see a commensurate increase in costs --
-----------------------------------------------------------------------------------

-- Don't change cost of this compared to master, returns no rows:
select count(*) from cost_of_skipping where ten between 9000 and 10000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from cost_of_skipping where ten between 9000 and 10000;

-- Good equality case using skip arrays:
select count(*) from cost_of_skipping where thousand = 666;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from cost_of_skipping where thousand = 666;

-- Simulated good equality case skip scan using regular SAOPs:
prepare simulated_good_skip_scan as

    -- for simulated SAOP columns:
    with a as (
      select
        i
      from
        generate_series(0, 10_001) i
    )

-- Start of base query:
select count(*) from cost_of_skipping where

    -- Simulated SAOP columns:
    ten = any(array[(select array_agg(i) from a)])
    and
    hundred = any(array[(select array_agg(i) from a)])
    and

-- continuation of base query:
thousand = 666;

-- Execute simulated good equality case skip scan using regular SAOPs:
execute simulated_good_skip_scan;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
-- Expect parity with master for this case, while letting it serve as a guide
-- for the costs we ought to expect for prior true skip original:
execute simulated_good_skip_scan;

-- Good case using skip arrays, inequalities to make skip array:
select count(*) from cost_of_skipping where ten between 1 and 2 and hundred = 55;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from cost_of_skipping where ten between 1 and 2 and hundred = 55;

-- Simulated good case using skip arrays, inequalities to make skip array:
select count(*) from cost_of_skipping where ten in (1, 2) and hundred = 55;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from cost_of_skipping where ten in (1, 2) and hundred = 55;

-- Other good case using skip arrays, inequalities to make skip array:
select count(*) from cost_of_skipping where ten between 1 and 8 and hundred = 55;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from cost_of_skipping where ten between 1 and 8 and hundred = 55;

-- Simulated other good case using skip arrays, inequalities to make skip array:
select count(*) from cost_of_skipping
where ten in (1, 2, 3, 4, 5, 6, 7, 8) and hundred = 55;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from cost_of_skipping
where ten in (1, 2, 3, 4, 5, 6, 7, 8) and hundred = 55;

-- Full index scan using skip arrays:
select count(*) from cost_of_skipping where i = 123456;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from cost_of_skipping where i = 123456;

-- Simulated full index scan using regular SAOPs:
prepare simulated_full_index_scan as

    -- for simulated SAOP columns:
    with a as (
      select
        i
      from
        generate_series(0, 10_001) i
    )

-- Start of base query:
select count(*) from cost_of_skipping where

    -- Simulated SAOP columns:
    ten = any(array[(select array_agg(i) from a)])
    and
    hundred = any(array[(select array_agg(i) from a)])
    and
    thousand = any(array[(select array_agg(i) from a)])
    and
    ten_thousand = any(array[(select array_agg(i) from a)])
    and

-- continuation of base query:
i = 123456;

-- Execute simulated good case skip scan using regular SAOPs:
execute simulated_full_index_scan;
-- Expect parity with master for this case, while letting it serve as a guide
-- for the costs we ought to expect for prior true skip original:
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute simulated_full_index_scan;

-- Index-only scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to on;
set enable_indexscan to off;

-- These two queries should have at least roughly comparable costs, since they
-- return the same rows, despite the extra pair of "hundred" inequalities for
-- the second query.

-- First query (problematic one, had way too low cost of ~7 before I fixed
-- this bug):
select count(*) from cost_of_skipping where ten = 1                               and thousand = 33;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from cost_of_skipping where ten = 1                               and thousand = 33;

-- Second query:
select count(*) from cost_of_skipping where ten = 1 and hundred between 0 and 100 and thousand = 33;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from cost_of_skipping where ten = 1 and hundred between 0 and 100 and thousand = 33;

-- Low cardinality leading expression column:
create index expression_idx on cost_of_skipping ((i % 3), i);
analyze cost_of_skipping; -- need this for stats on expression

-- This should be fast/cheap:
select * from cost_of_skipping where i = 123456;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from cost_of_skipping where i = 123456;

drop index expression_idx;

create index partial_idx on cost_of_skipping (ten, i) where hundred = 34;

-- This should be fast/cheap:
select * from cost_of_skipping where hundred = 34 and i = 123456;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from cost_of_skipping where hundred = 34 and i = 123456;

----------------------------------------------------------------------------
-- Don't pick the wrong index to skip scan when we've two similar indexes --
----------------------------------------------------------------------------
set client_min_messages=error;
drop table if exists dont_pick_wrong_index;
reset client_min_messages;

create unlogged table dont_pick_wrong_index as
select
  666 as low_cardinality_nothing,
  i
from
  generate_series(1, 100_000) i;

create index correct on dont_pick_wrong_index(i, low_cardinality_nothing);
vacuum analyze dont_pick_wrong_index;
create index wrong on dont_pick_wrong_index(low_cardinality_nothing, i);

-- We had better not pick dont_pick_me here:
select * from dont_pick_wrong_index where i = 55562;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from dont_pick_wrong_index where i = 55562;

-------------------------------
-- Masahiro Ikeda test cases --
-------------------------------

-- Taken from: https://postgr.es/m/TYWPR01MB10982A413E0EC4088E78C0E11B1A62@TYWPR01MB10982.jpnprd01.prod.outlook.com

-- prepare
set default_statistics_target=10000;
set client_min_messages=error;
DROP TABLE IF EXISTS ikeda_test;
CREATE EXTENSION if not exists pg_prewarm;
reset client_min_messages;
CREATE UNLOGGED TABLE ikeda_test (id1 int2, id2 int4, id3 int8, value varchar(32));
INSERT INTO ikeda_test (SELECT i%11, i%103, i%1009, 'hello' FROM generate_series(1,1000000) s(i));
-- CREATE INDEX idx_id3 ON ikeda_test(id3);
-- CREATE INDEX idx_id1_id3 ON ikeda_test(id1, id3);
-- CREATE INDEX idx_id2_id3 ON ikeda_test(id2, id3);
-- CREATE INDEX idx_id1_id2_id3 ON ikeda_test(id1, id2, id3);
ANALYZE;

-- prepare
SET skipscan_prefix_cols = 3;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;

SELECT pg_prewarm('ikeda_test');

-- seqscan
SET enable_seqscan = on;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
SELECT * FROM ikeda_test WHERE id3 = 101;
SET enable_seqscan = off;

-- indexscan
SET enable_indexscan = on;

CREATE INDEX idx_id3 ON ikeda_test(id3);
SELECT pg_prewarm('idx_id3');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
SELECT * FROM ikeda_test WHERE id3 = 101;
DROP INDEX idx_id3;

CREATE INDEX idx_id1_id3 ON ikeda_test(id1, id3);
SELECT pg_prewarm('idx_id1_id3');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
SELECT * FROM ikeda_test WHERE id3 = 101;
DROP INDEX idx_id1_id3;

CREATE INDEX idx_id2_id3 ON ikeda_test(id2, id3);
SELECT pg_prewarm('idx_id2_id3');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
SELECT * FROM ikeda_test WHERE id3 = 101;
DROP INDEX idx_id2_id3;

CREATE INDEX idx_id1_id2_id3 ON ikeda_test(id1, id2, id3);
SELECT pg_prewarm('idx_id1_id2_id3');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
SELECT * FROM ikeda_test WHERE id3 = 101;
DROP INDEX idx_id1_id2_id3;

SET enable_indexscan = off;

-- bitmapscan
SET enable_bitmapscan = on;

CREATE INDEX idx_id3 ON ikeda_test(id3);
SELECT pg_prewarm('idx_id3');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
SELECT * FROM ikeda_test WHERE id3 = 101;
DROP INDEX idx_id3;

CREATE INDEX idx_id1_id3 ON ikeda_test(id1, id3);
SELECT pg_prewarm('idx_id1_id3');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
SELECT * FROM ikeda_test WHERE id3 = 101;
DROP INDEX idx_id1_id3;

CREATE INDEX idx_id2_id3 ON ikeda_test(id2, id3);
SELECT pg_prewarm('idx_id2_id3');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
SELECT * FROM ikeda_test WHERE id3 = 101;
DROP INDEX idx_id2_id3;

CREATE INDEX idx_id1_id2_id3 ON ikeda_test(id1, id2, id3);
SELECT pg_prewarm('idx_id1_id2_id3');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
SELECT * FROM ikeda_test WHERE id3 = 101;
DROP INDEX idx_id1_id2_id3;

SET enable_bitmapscan = on;
SET enable_indexscan = on;
SET enable_indexonlyscan = on;
SET enable_seqscan = off;

---------------------------
-- Pachot test case/demo --
---------------------------

-- From https://dev.to/yugabyte/index-skip-scan-in-yugabytedb-2ao2

set client_min_messages=error;
drop table if exists demo_pachot;
reset client_min_messages;

create unlogged table demo_pachot(
  a int,
  b int,
  primary key (a, b)
);
insert into demo_pachot select a, b
 from generate_series(1,100) a, generate_series(1,100000) b;
vacuum analyze demo_pachot;

select * from demo_pachot where a = 50 limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from demo_pachot where a = 50 limit 10;

select * from demo_pachot where b = 5000 limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from demo_pachot where b = 5000 limit 10;

select * from demo_pachot where a = 50 and b = 5000 limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from demo_pachot where a = 50 and b = 5000 limit 10;

select * from demo_pachot where a = 50 and b > 5000 limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from demo_pachot where a = 50 and b > 5000 limit 10;

select * from demo_pachot where a > 50 and b = 5000 limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from demo_pachot where a > 50 and b = 5000 limit 10;

-- Per https://github.com/yugabyte/yugabyte-db/issues/11965
select * from demo_pachot where a > 50 and b > 5000 limit 1000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from demo_pachot where a > 50 and b > 5000 limit 1000;

-- (September 26) The cost of this should not change at all, relative to master:
select * from demo_pachot where (a,b) > (50, 5000) limit 1000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from demo_pachot where (a,b) > (50, 5000) limit 1000;

----------------------------------------------
-- whack-a-doodle skip array on "b" costing --
----------------------------------------------
--
-- (January 16 2025)
--
-- Multirange perf validation test variant with whack-a-doodle costing due to
-- the presence of a skip array on "b" combined with a fairly selective
-- predicate on both "a" and "c":
set client_min_messages=error;
drop table if exists cost_multirange_medcard;
reset client_min_messages;
create unlogged table cost_multirange_medcard(a int, b int, c int);
create index cost_multirange_medcard_idx on cost_multirange_medcard(a, b, c);
insert into cost_multirange_medcard select x % ( 100_000 / 300), x, x from generate_series(1, 100_000) x;
vacuum analyze cost_multirange_medcard;

-- Note: "where a = 42" alone would return 301 rows, with "c = 99609" it's only 1 row
-- Note: When I discovered this problematic costing, master costed it at ~7,
-- whereas the patch costed it at ~1037 -- this wild disparity in cost is the
-- bug here
select *
from cost_multirange_medcard
where
  a = 42
  and c = 99609;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_multirange_medcard
where
  a = 42
  and c = 99609;

-- Same again, though this time it's a very non-selective range skip array on "b" instead:
select *
from cost_multirange_medcard
where
  a = 42
  and b between 0 and 1_000_000
  and c = 99609;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from cost_multirange_medcard
where
  a = 42
  and b between 0 and 1_000_000
  and c = 99609;
