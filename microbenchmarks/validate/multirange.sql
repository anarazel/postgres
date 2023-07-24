set log_btree_verbosity to 0;
set log_array_advance to off;

\pset pager off
set client_min_messages='notice';
set enable_seqscan=off;
\set numrows 1_000_000

create unlogged table test_multirange -- fake
(
  c1 int
);
select (select not exists(select * from pg_class where relname = 'test_multirange') or (select count(*) != :'numrows' from test_multirange)) as load_data
       \gset

\if :load_data
  drop table if exists test_multirange;
  create unlogged table test_multirange (a int, b int, c int);
  create index test_multirange_idx on test_multirange (a, b, c);
  insert into test_multirange select 42, x, x from generate_series(1, :numrows) x;
  vacuum (freeze, verbose, analyze) test_multirange;

  drop table if exists test_multirange_allhighcardinality;
  create unlogged table test_multirange_allhighcardinality (a int, b int, c int);
  create index test_multirange_allhighcardinality_idx on test_multirange_allhighcardinality (a, b, c);
  insert into test_multirange_allhighcardinality select x, x, x from generate_series(1, :numrows) x;
  vacuum (freeze, verbose, analyze) test_multirange_allhighcardinality;

  drop table if exists test_multirange_medcard;
  create unlogged table test_multirange_medcard(a int, b int, c int);
  create index test_multirange_medcard_idx on test_multirange_medcard(a, b, c);
  insert into test_multirange_medcard select x % ( :numrows / 300), x, x from generate_series(1, :numrows) x;
  vacuum (freeze, verbose, analyze) test_multirange_medcard;
\endif

---------------------------------------------
-- test_multirange, skip array on 'b' only --
---------------------------------------------
\echo 'select * from test_multirange where a = 42 and c = 1:'
select * from test_multirange where a = 42 and c = 1;
select * from test_multirange where a = 42 and c = 1;
select * from test_multirange where a = 42 and c = 1;
select * from test_multirange where a = 42 and c = 1;
select * from test_multirange where a = 42 and c = 1;
select * from test_multirange where a = 42 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange where a = 42 and c = 1;

\echo 'select * from test_multirange where a = 42 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a = 42 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange where a = 42 and c = 1 order by a desc, b desc, c desc;

-------------------------------------------------
-- test_multirange, multiple range skip arrays --
-------------------------------------------------
\echo 'select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1:'
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;

\echo 'select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;

--------------------------------------------------------
-- test_multirange, one range skip array, one regular --
--------------------------------------------------------
\echo 'select * from test_multirange where a between 0 and 42 and c = 1:'
select * from test_multirange where a between 0 and 42 and c = 1;
select * from test_multirange where a between 0 and 42 and c = 1;
select * from test_multirange where a between 0 and 42 and c = 1;
select * from test_multirange where a between 0 and 42 and c = 1;
select * from test_multirange where a between 0 and 42 and c = 1;
select * from test_multirange where a between 0 and 42 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange where a between 0 and 42 and c = 1;

\echo 'select * from test_multirange where a between 0 and 42 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;

--------------------------------------------------------
-- test_multirange, one regular, one range skip array --
--------------------------------------------------------
\echo 'select * from test_multirange where b between 0 and 1_000_000 and c = 1:'
select * from test_multirange where b between 0 and 1_000_000 and c = 1;
select * from test_multirange where b between 0 and 1_000_000 and c = 1;
select * from test_multirange where b between 0 and 1_000_000 and c = 1;
select * from test_multirange where b between 0 and 1_000_000 and c = 1;
select * from test_multirange where b between 0 and 1_000_000 and c = 1;
select * from test_multirange where b between 0 and 1_000_000 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange where b between 0 and 1_000_000 and c = 1;

\echo 'select * from test_multirange where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;

--------------------------------------------------------
-- test_multirange, one regular, one range skip array --
--------------------------------------------------------
\echo 'select * from test_multirange where b between 0 and 1 and c = 1:'
select * from test_multirange where b between 0 and 1 and c = 1;
select * from test_multirange where b between 0 and 1 and c = 1;
select * from test_multirange where b between 0 and 1 and c = 1;
select * from test_multirange where b between 0 and 1 and c = 1;
select * from test_multirange where b between 0 and 1 and c = 1;
select * from test_multirange where b between 0 and 1 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange where b between 0 and 1 and c = 1;

\echo 'select * from test_multirange where b between 0 and 1 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;

------------------------------------------------------------
-- test_multirange_allhighcardinality, skip array on 'b' only --
------------------------------------------------------------
\echo 'select * from test_multirange_allhighcardinality where a = 42 and c = 1:'
select * from test_multirange_allhighcardinality where a = 42 and c = 1;
select * from test_multirange_allhighcardinality where a = 42 and c = 1;
select * from test_multirange_allhighcardinality where a = 42 and c = 1;
select * from test_multirange_allhighcardinality where a = 42 and c = 1;
select * from test_multirange_allhighcardinality where a = 42 and c = 1;
select * from test_multirange_allhighcardinality where a = 42 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a = 42 and c = 1;

\echo 'select * from test_multirange_allhighcardinality where a = 42 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange_allhighcardinality where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a = 42 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a = 42 and c = 1 order by a desc, b desc, c desc;

----------------------------------------------------------------
-- test_multirange_allhighcardinality, multiple range skip arrays --
----------------------------------------------------------------
\echo 'select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1:'
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;

\echo 'select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;

-----------------------------------------------------------------------
-- test_multirange_allhighcardinality, one range skip array, one regular --
-----------------------------------------------------------------------
\echo 'select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1:'
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1;
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1;
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1;
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1;
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1;
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1;

\echo 'select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;

------------------------------------------------------------------------
-- test_multirange_allhighcardinality, no skip arrays (only inequalities) --
------------------------------------------------------------------------
\echo 'select * from test_multirange_allhighcardinality where a between 0 and 42:'
select * from test_multirange_allhighcardinality where a between 0 and 42;
select * from test_multirange_allhighcardinality where a between 0 and 42;
select * from test_multirange_allhighcardinality where a between 0 and 42;
select * from test_multirange_allhighcardinality where a between 0 and 42;
select * from test_multirange_allhighcardinality where a between 0 and 42;
select * from test_multirange_allhighcardinality where a between 0 and 42;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 42;

\echo 'select * from test_multirange_allhighcardinality where a between 0 and 42 order by a desc, b desc, c desc:'
select * from test_multirange_allhighcardinality where a between 0 and 42 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 42 order by a desc, b desc, c desc;

---------------------------------------------------------------------------------
-- test_multirange_allhighcardinality, leading skip array, required sk on "b" only --
---------------------------------------------------------------------------------
\echo 'select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555:'
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555;

\echo 'select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555 order by a desc, b desc, c desc:'
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;

-----------------------------------------------------------------------------------------------------------
-- test_multirange_allhighcardinality, leading skip array, required sk on "b" only, wide range of "a" values --
-----------------------------------------------------------------------------------------------------------
\echo 'select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555:'
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555;

\echo 'select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc:'
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;

---------------------------------------------------------------------------------------------------------------
-- test_multirange_allhighcardinality, leading skip array, required sk on "c" only, wide range of "a" values --
---------------------------------------------------------------------------------------------------------------
\echo 'select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555:'
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555;

\echo 'select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc:'
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;

-----------------------------------------------------------------------
-- test_multirange_allhighcardinality, one regular, one range skip array --
-----------------------------------------------------------------------
\echo 'select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1:'
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1;
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1;
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1;
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1;
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1;
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1;

\echo 'select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;

-----------------------------------------------------------------------
-- test_multirange_allhighcardinality, one regular, one range skip array --
-----------------------------------------------------------------------
\echo 'select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1:'
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1;

\echo 'select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;

-----------------------------------------------------------------------------
-- test_multirange_allhighcardinality, one regular range, one range skip array --
-----------------------------------------------------------------------------
\echo 'select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1:'
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1;

\echo 'select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc:'
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_allhighcardinality where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;

-----------------------------------------------------
-- test_multirange_medcard, skip array on 'b' only --
-----------------------------------------------------
\echo 'select * from test_multirange_medcard where a = 42 and c = 1:'
select * from test_multirange_medcard where a = 42 and c = 1;
select * from test_multirange_medcard where a = 42 and c = 1;
select * from test_multirange_medcard where a = 42 and c = 1;
select * from test_multirange_medcard where a = 42 and c = 1;
select * from test_multirange_medcard where a = 42 and c = 1;
select * from test_multirange_medcard where a = 42 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a = 42 and c = 1;

\echo 'select * from test_multirange_medcard where a = 42 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange_medcard where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a = 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a = 42 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a = 42 and c = 1 order by a desc, b desc, c desc;

---------------------------------------------------------
-- test_multirange_medcard, multiple range skip arrays --
---------------------------------------------------------
\echo 'select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1:'
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1;

\echo 'select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 42 and b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;

----------------------------------------------------------------
-- test_multirange_medcard, one range skip array, one regular --
----------------------------------------------------------------
\echo 'select * from test_multirange_medcard where a between 0 and 42 and c = 1:'
select * from test_multirange_medcard where a between 0 and 42 and c = 1;
select * from test_multirange_medcard where a between 0 and 42 and c = 1;
select * from test_multirange_medcard where a between 0 and 42 and c = 1;
select * from test_multirange_medcard where a between 0 and 42 and c = 1;
select * from test_multirange_medcard where a between 0 and 42 and c = 1;
select * from test_multirange_medcard where a between 0 and 42 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 42 and c = 1;

\echo 'select * from test_multirange_medcard where a between 0 and 42 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange_medcard where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 42 and c = 1 order by a desc, b desc, c desc;

-----------------------------------------------------------------
-- test_multirange_medcard, no skip arrays (only inequalities) --
-----------------------------------------------------------------
\echo 'select * from test_multirange_medcard where a between 0 and 42:'
select * from test_multirange_medcard where a between 0 and 42;
select * from test_multirange_medcard where a between 0 and 42;
select * from test_multirange_medcard where a between 0 and 42;
select * from test_multirange_medcard where a between 0 and 42;
select * from test_multirange_medcard where a between 0 and 42;
select * from test_multirange_medcard where a between 0 and 42;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 42;

\echo 'select * from test_multirange_medcard where a between 0 and 42 order by a desc, b desc, c desc:'
select * from test_multirange_medcard where a between 0 and 42 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 42 order by a desc, b desc, c desc;

--------------------------------------------------------------------------
-- test_multirange_medcard, leading skip array, required sk on "b" only --
--------------------------------------------------------------------------
\echo 'select * from test_multirange_medcard where a between 0 and 42 and b = 555:'
select * from test_multirange_medcard where a between 0 and 42 and b = 555;
select * from test_multirange_medcard where a between 0 and 42 and b = 555;
select * from test_multirange_medcard where a between 0 and 42 and b = 555;
select * from test_multirange_medcard where a between 0 and 42 and b = 555;
select * from test_multirange_medcard where a between 0 and 42 and b = 555;
select * from test_multirange_medcard where a between 0 and 42 and b = 555;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 42 and b = 555;

\echo 'select * from test_multirange_medcard where a between 0 and 42 and b = 555 order by a desc, b desc, c desc:'
select * from test_multirange_medcard where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 42 and b = 555 order by a desc, b desc, c desc;

----------------------------------------------------------------------------------------------------
-- test_multirange_medcard, leading skip array, required sk on "b" only, wide range of "a" values --
----------------------------------------------------------------------------------------------------
\echo 'select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555:'
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555;
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555;
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555;
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555;
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555;
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555;

\echo 'select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc:'
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 1_000_000 and b = 555 order by a desc, b desc, c desc;

----------------------------------------------------------------------------------------------------
-- test_multirange_medcard, leading skip array, required sk on "c" only, wide range of "a" values --
----------------------------------------------------------------------------------------------------
\echo 'select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555:'
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555;
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555;
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555;
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555;
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555;
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555;

\echo 'select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc:'
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where a between 0 and 1_000_000 and c = 555 order by a desc, b desc, c desc;

----------------------------------------------------------------
-- test_multirange_medcard, one regular, one range skip array --
----------------------------------------------------------------
\echo 'select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1:'
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1;
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1;
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1;
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1;
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1;
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1;

\echo 'select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where b between 0 and 1_000_000 and c = 1 order by a desc, b desc, c desc;

----------------------------------------------------------------
-- test_multirange_medcard, one regular, one range skip array --
----------------------------------------------------------------
\echo 'select * from test_multirange_medcard where b between 0 and 1 and c = 1:'
select * from test_multirange_medcard where b between 0 and 1 and c = 1;
select * from test_multirange_medcard where b between 0 and 1 and c = 1;
select * from test_multirange_medcard where b between 0 and 1 and c = 1;
select * from test_multirange_medcard where b between 0 and 1 and c = 1;
select * from test_multirange_medcard where b between 0 and 1 and c = 1;
select * from test_multirange_medcard where b between 0 and 1 and c = 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where b between 0 and 1 and c = 1;

\echo 'select * from test_multirange_medcard where b between 0 and 1 and c = 1 order by a desc, b desc, c desc:'
select * from test_multirange_medcard where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where b between 0 and 1 and c = 1 order by a desc, b desc, c desc;

----------------------------------------------------------------------
-- test_multirange_medcard, one regular range, one range skip array --
----------------------------------------------------------------------
\echo 'select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1:'
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1;
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1;
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1;
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1;
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1;
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1;

\echo 'select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc:'
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
EXPLAIN (ANALYZE, BUFFERS) select * from test_multirange_medcard where b between 0 and 1 and c between 0 and 1 order by a desc, b desc, c desc;
