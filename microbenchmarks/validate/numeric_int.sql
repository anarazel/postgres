-- based on https://postgr.es/m/527571eb98b9bed54c8cadb261c14795@oss.nttdata.com

-- HOWTO get number of distinct values for a given page range (blkno 1 - 1000 here):
/*
select i, count(distinct (regexp_match(data, '\(([0-9]+),'))[1])
from (select i from
    generate_series(1, 1000) i) ii,
lateral bt_page_items('t_idx', i)
where dead is not null
group by i;
*/
set log_btree_verbosity to 0;
set log_array_advance to off;

\pset pager off
set client_min_messages='notice';
set enable_seqscan=off;
\set numrows 1_000_000
-- set skipscan_prefix_cols=0;
-- set skipscan_skipsupport_enabled=false;

create unlogged table test_one_numeric_int -- fake
(
  c1 int
);
select (select not exists(select * from pg_class where relname = 'test_one_numeric_int') or (select count(*) != :'numrows' from test_one_numeric_int)) as load_data
       \gset

\if :load_data
  -- One
  \set rows_per_group 1
  DROP TABLE IF EXISTS test_one_numeric_int;
  CREATE unlogged TABLE test_one_numeric_int (id1 numeric, id2 int);
  CREATE INDEX t_one_numeric_int_idx on test_one_numeric_int (id1, id2);
  insert into test_one_numeric_int (
    select
      abs(hashint4(i::int4)),
      abs(hashint4(i::int4) # hashint4(j::int4)) % :numrows
    from
      generate_series(1, (:numrows / :rows_per_group)) s(i),
      generate_series(1, :rows_per_group) j);
  vacuum (freeze, verbose, analyze) test_one_numeric_int;

  -- One sequential
  \set rows_per_group 1
  DROP TABLE IF EXISTS test_one_numeric_int_sequential;
  CREATE unlogged TABLE test_one_numeric_int_sequential (id1 numeric, id2 int);
  CREATE INDEX t_one_sequential_numeric_int_idx on test_one_numeric_int_sequential (id1, id2);
  insert into test_one_numeric_int_sequential (
    select
      i,
      abs(hashint4(i::int4) # hashint4(j::int4)) % :numrows
    from
      generate_series(1, (:numrows / :rows_per_group)) s(i),
      generate_series(1, :rows_per_group) j);
  vacuum (freeze, verbose, analyze) test_one_numeric_int_sequential;


  -- Five
  \set rows_per_group 5
  DROP TABLE IF EXISTS test_five_numeric_int;
  CREATE unlogged TABLE test_five_numeric_int (id1 numeric, id2 int);
  CREATE INDEX t_five_numeric_int_idx on test_five_numeric_int (id1, id2);
  insert into test_five_numeric_int (
    select
      abs(hashint4(i::int4)),
      abs(hashint4(i::int4) # hashint4(j::int4)) % :numrows
    from
      generate_series(1, (:numrows / :rows_per_group)) s(i),
      generate_series(1, :rows_per_group) j);
  vacuum (freeze, verbose, analyze) test_five_numeric_int;

  -- Ten
  \set rows_per_group 10
  DROP TABLE IF EXISTS test_ten_numeric_int;
  CREATE unlogged TABLE test_ten_numeric_int (id1 numeric, id2 int);
  CREATE INDEX t_ten_numeric_int_idx on test_ten_numeric_int (id1, id2);
  insert into test_ten_numeric_int (
    select
      abs(hashint4(i::int4)),
      abs(hashint4(i::int4) # hashint4(j::int4)) % :numrows
    from
      generate_series(1, (:numrows / :rows_per_group)) s(i),
      generate_series(1, :rows_per_group) j);
  vacuum (freeze, verbose, analyze) test_ten_numeric_int;

  -- Fifteen
  \set rows_per_group 15
  DROP TABLE IF EXISTS test_fifteen_numeric_int;
  CREATE unlogged TABLE test_fifteen_numeric_int (id1 numeric, id2 int);
  CREATE INDEX t_fifteen_numeric_int_idx on test_fifteen_numeric_int (id1, id2);
  insert into test_fifteen_numeric_int (
    select
      abs(hashint4(i::int4)),
      abs(hashint4(i::int4) # hashint4(j::int4)) % :numrows
    from
      generate_series(1, (:numrows / :rows_per_group)) s(i),
      generate_series(1, :rows_per_group) j);
  vacuum (freeze, verbose, analyze) test_fifteen_numeric_int;

  -- Seventeen
  \set rows_per_group 17
  DROP TABLE IF EXISTS test_seventeen_numeric_int;
  CREATE unlogged TABLE test_seventeen_numeric_int (id1 numeric, id2 int);
  CREATE INDEX t_seventeen_numeric_int_idx on test_seventeen_numeric_int (id1, id2);
  insert into test_seventeen_numeric_int (
    select
      abs(hashint4(i::int4)),
      abs(hashint4(i::int4) # hashint4(j::int4)) % :numrows
    from
      generate_series(1, (:numrows / :rows_per_group)) s(i),
      generate_series(1, :rows_per_group) j);
  vacuum (freeze, verbose, analyze) test_seventeen_numeric_int;

  -- Twenty
  \set rows_per_group 20
  drop table if exists test_twenty_numeric_int;
  create unlogged table test_twenty_numeric_int (id1 numeric, id2 int);
  create index t_twenty_numeric_int_idx on test_twenty_numeric_int (id1, id2);
  insert into test_twenty_numeric_int (
    select
      abs(hashint4(i::int4)),
      abs(hashint4(i::int4) # hashint4(j::int4)) % :numrows
    from
      generate_series(1, (:numrows / :rows_per_group)) s(i),
      generate_series(1, :rows_per_group) j);
  vacuum (freeze, verbose, analyze) test_twenty_numeric_int;

  -- Twenty-five
  \set rows_per_group 25
  drop table if exists test_twentyfive_numeric_int;
  create unlogged table test_twentyfive_numeric_int (id1 numeric, id2 int);
  create index t_twentyfive_numeric_int_idx on test_twentyfive_numeric_int (id1, id2);
  insert into test_twentyfive_numeric_int (
    select
      abs(hashint4(i::int4)),
      abs(hashint4(i::int4) # hashint4(j::int4)) % :numrows
    from
      generate_series(1, (:numrows / :rows_per_group)) s(i),
      generate_series(1, :rows_per_group) j);
  vacuum (freeze, verbose, analyze) test_twentyfive_numeric_int;

  -- fifty
  \set rows_per_group 50
  drop table if exists test_fifty_numeric_int;
  create unlogged table test_fifty_numeric_int (id1 numeric, id2 int);
  create index t_fifty_numeric_int_idx on test_fifty_numeric_int (id1, id2);
  insert into test_fifty_numeric_int (
    select
      abs(hashint4(i::int4)),
      abs(hashint4(i::int4) # hashint4(j::int4)) % :numrows
    from
      generate_series(1, (:numrows / :rows_per_group)) s(i),
      generate_series(1, :rows_per_group) j);
  vacuum (freeze, verbose, analyze) test_fifty_numeric_int;

  -- five hundred
  \set rows_per_group 500
  drop table if exists test_five_hundred_numeric_int;
  create unlogged table test_five_hundred_numeric_int (id1 numeric, id2 int);
  create index t_five_hundred_numeric_int_idx on test_five_hundred_numeric_int  (id1, id2);
  insert into test_five_hundred_numeric_int (
    select
      abs(hashint4(i::int4)),
      abs(hashint4(i::int4) # hashint4(j::int4)) % :numrows
    from
      generate_series(1, (:numrows / :rows_per_group)) s(i),
      generate_series(1, :rows_per_group) j);
  vacuum (freeze, verbose, analyze) test_five_hundred_numeric_int;
\endif

---------
-- One --
---------
\echo 'SELECT * FROM test_one_numeric_int WHERE id2 = 1:'
SELECT * FROM test_one_numeric_int WHERE id2 = 1;
SELECT * FROM test_one_numeric_int WHERE id2 = 1;
SELECT * FROM test_one_numeric_int WHERE id2 = 1;
SELECT * FROM test_one_numeric_int WHERE id2 = 1;
SELECT * FROM test_one_numeric_int WHERE id2 = 1;
SELECT * FROM test_one_numeric_int WHERE id2 = 1;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_one_numeric_int WHERE id2 = 1;


\echo 'SELECT * FROM test_one_numeric_int WHERE id2 = 501:'
SELECT * FROM test_one_numeric_int WHERE id2 = 501;
SELECT * FROM test_one_numeric_int WHERE id2 = 501;
SELECT * FROM test_one_numeric_int WHERE id2 = 501;
SELECT * FROM test_one_numeric_int WHERE id2 = 501;
SELECT * FROM test_one_numeric_int WHERE id2 = 501;
SELECT * FROM test_one_numeric_int WHERE id2 = 501;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_one_numeric_int WHERE id2 = 501;


\echo 'SELECT * FROM test_one_numeric_int WHERE id2 = 900:'
SELECT * FROM test_one_numeric_int WHERE id2 = 900;
SELECT * FROM test_one_numeric_int WHERE id2 = 900;
SELECT * FROM test_one_numeric_int WHERE id2 = 900;
SELECT * FROM test_one_numeric_int WHERE id2 = 900;
SELECT * FROM test_one_numeric_int WHERE id2 = 900;
SELECT * FROM test_one_numeric_int WHERE id2 = 900;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_one_numeric_int WHERE id2 = 900;


\echo 'SELECT * FROM test_one_numeric_int WHERE id2 IN (0, 1, 900):'
SELECT * FROM test_one_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_one_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_one_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_one_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_one_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_one_numeric_int WHERE id2 IN (0, 1, 900);
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_one_numeric_int WHERE id2 IN (0, 1, 900);

---------------------
-- One, sequential --
---------------------
\echo 'SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 1:'
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 1;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 1;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 1;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 1;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 1;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 1;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 1;


\echo 'SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 501:'
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 501;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 501;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 501;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 501;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 501;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 501;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 501;


\echo 'SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 900:'
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 900;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 900;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 900;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 900;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 900;
SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 900;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_one_numeric_int_sequential WHERE id2 = 900;


\echo 'SELECT * FROM test_one_numeric_int_sequential WHERE id2 IN (0, 1, 900):'
SELECT * FROM test_one_numeric_int_sequential WHERE id2 IN (0, 1, 900);
SELECT * FROM test_one_numeric_int_sequential WHERE id2 IN (0, 1, 900);
SELECT * FROM test_one_numeric_int_sequential WHERE id2 IN (0, 1, 900);
SELECT * FROM test_one_numeric_int_sequential WHERE id2 IN (0, 1, 900);
SELECT * FROM test_one_numeric_int_sequential WHERE id2 IN (0, 1, 900);
SELECT * FROM test_one_numeric_int_sequential WHERE id2 IN (0, 1, 900);
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_one_numeric_int_sequential WHERE id2 IN (0, 1, 900);

----------
-- Five --
----------
\echo 'SELECT * FROM test_five_numeric_int WHERE id2 = 1:'
SELECT * FROM test_five_numeric_int WHERE id2 = 1;
SELECT * FROM test_five_numeric_int WHERE id2 = 1;
SELECT * FROM test_five_numeric_int WHERE id2 = 1;
SELECT * FROM test_five_numeric_int WHERE id2 = 1;
SELECT * FROM test_five_numeric_int WHERE id2 = 1;
SELECT * FROM test_five_numeric_int WHERE id2 = 1;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_five_numeric_int WHERE id2 = 1;


\echo 'SELECT * FROM test_five_numeric_int WHERE id2 = 501:'
SELECT * FROM test_five_numeric_int WHERE id2 = 501;
SELECT * FROM test_five_numeric_int WHERE id2 = 501;
SELECT * FROM test_five_numeric_int WHERE id2 = 501;
SELECT * FROM test_five_numeric_int WHERE id2 = 501;
SELECT * FROM test_five_numeric_int WHERE id2 = 501;
SELECT * FROM test_five_numeric_int WHERE id2 = 501;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_five_numeric_int WHERE id2 = 501;


\echo 'SELECT * FROM test_five_numeric_int WHERE id2 = 900:'
SELECT * FROM test_five_numeric_int WHERE id2 = 900;
SELECT * FROM test_five_numeric_int WHERE id2 = 900;
SELECT * FROM test_five_numeric_int WHERE id2 = 900;
SELECT * FROM test_five_numeric_int WHERE id2 = 900;
SELECT * FROM test_five_numeric_int WHERE id2 = 900;
SELECT * FROM test_five_numeric_int WHERE id2 = 900;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_five_numeric_int WHERE id2 = 900;


\echo 'SELECT * FROM test_five_numeric_int WHERE id2 IN (0, 1, 900):'
SELECT * FROM test_five_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_five_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_five_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_five_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_five_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_five_numeric_int WHERE id2 IN (0, 1, 900);
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_five_numeric_int WHERE id2 IN (0, 1, 900);

---------
-- Ten --
---------
\echo 'SELECT * FROM test_ten_numeric_int WHERE id2 = 1:'
SELECT * FROM test_ten_numeric_int WHERE id2 = 1;
SELECT * FROM test_ten_numeric_int WHERE id2 = 1;
SELECT * FROM test_ten_numeric_int WHERE id2 = 1;
SELECT * FROM test_ten_numeric_int WHERE id2 = 1;
SELECT * FROM test_ten_numeric_int WHERE id2 = 1;
SELECT * FROM test_ten_numeric_int WHERE id2 = 1;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_ten_numeric_int WHERE id2 = 1;


\echo 'SELECT * FROM test_ten_numeric_int WHERE id2 = 501:'
SELECT * FROM test_ten_numeric_int WHERE id2 = 501;
SELECT * FROM test_ten_numeric_int WHERE id2 = 501;
SELECT * FROM test_ten_numeric_int WHERE id2 = 501;
SELECT * FROM test_ten_numeric_int WHERE id2 = 501;
SELECT * FROM test_ten_numeric_int WHERE id2 = 501;
SELECT * FROM test_ten_numeric_int WHERE id2 = 501;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_ten_numeric_int WHERE id2 = 501;


\echo 'SELECT * FROM test_ten_numeric_int WHERE id2 = 900:'
SELECT * FROM test_ten_numeric_int WHERE id2 = 900;
SELECT * FROM test_ten_numeric_int WHERE id2 = 900;
SELECT * FROM test_ten_numeric_int WHERE id2 = 900;
SELECT * FROM test_ten_numeric_int WHERE id2 = 900;
SELECT * FROM test_ten_numeric_int WHERE id2 = 900;
SELECT * FROM test_ten_numeric_int WHERE id2 = 900;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_ten_numeric_int WHERE id2 = 900;


\echo 'SELECT * FROM test_ten_numeric_int WHERE id2 IN (0, 1, 900):'
SELECT * FROM test_ten_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_ten_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_ten_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_ten_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_ten_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_ten_numeric_int WHERE id2 IN (0, 1, 900);
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_ten_numeric_int WHERE id2 IN (0, 1, 900);


-------------
-- Fifteen --
-------------
\echo 'SELECT * FROM test_fifteen_numeric_int WHERE id2 = 1:'
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 1;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 1;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 1;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 1;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 1;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 1;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_fifteen_numeric_int WHERE id2 = 1;


\echo 'SELECT * FROM test_fifteen_numeric_int WHERE id2 = 501:'
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 501;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 501;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 501;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 501;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 501;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 501;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_fifteen_numeric_int WHERE id2 = 501;


\echo 'SELECT * FROM test_fifteen_numeric_int WHERE id2 = 900:'
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 900;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 900;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 900;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 900;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 900;
SELECT * FROM test_fifteen_numeric_int WHERE id2 = 900;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_fifteen_numeric_int WHERE id2 = 900;


\echo 'SELECT * FROM test_fifteen_numeric_int WHERE id2 IN (0, 1, 900):'
SELECT * FROM test_fifteen_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_fifteen_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_fifteen_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_fifteen_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_fifteen_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_fifteen_numeric_int WHERE id2 IN (0, 1, 900);
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_fifteen_numeric_int WHERE id2 IN (0, 1, 900);

---------------
-- Seventeen --
---------------
\echo 'SELECT * FROM test_seventeen_numeric_int WHERE id2 = 1:'
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 1;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 1;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 1;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 1;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 1;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 1;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_seventeen_numeric_int WHERE id2 = 1;


\echo 'SELECT * FROM test_seventeen_numeric_int WHERE id2 = 501:'
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 501;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 501;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 501;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 501;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 501;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 501;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_seventeen_numeric_int WHERE id2 = 501;


\echo 'SELECT * FROM test_seventeen_numeric_int WHERE id2 = 900:'
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 900;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 900;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 900;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 900;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 900;
SELECT * FROM test_seventeen_numeric_int WHERE id2 = 900;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_seventeen_numeric_int WHERE id2 = 900;


\echo 'SELECT * FROM test_seventeen_numeric_int WHERE id2 IN (0, 1, 900):'
SELECT * FROM test_seventeen_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_seventeen_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_seventeen_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_seventeen_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_seventeen_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_seventeen_numeric_int WHERE id2 IN (0, 1, 900);
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_seventeen_numeric_int WHERE id2 IN (0, 1, 900);

------------
-- Twenty --
------------
\echo 'SELECT * FROM test_twenty_numeric_int WHERE id2 = 1:'
SELECT * FROM test_twenty_numeric_int WHERE id2 = 1;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 1;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 1;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 1;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 1;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 1;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_twenty_numeric_int WHERE id2 = 1;


\echo 'SELECT * FROM test_twenty_numeric_int WHERE id2 = 501:'
SELECT * FROM test_twenty_numeric_int WHERE id2 = 501;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 501;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 501;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 501;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 501;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 501;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_twenty_numeric_int WHERE id2 = 501;


\echo 'SELECT * FROM test_twenty_numeric_int WHERE id2 = 900:'
SELECT * FROM test_twenty_numeric_int WHERE id2 = 900;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 900;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 900;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 900;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 900;
SELECT * FROM test_twenty_numeric_int WHERE id2 = 900;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_twenty_numeric_int WHERE id2 = 900;


\echo 'SELECT * FROM test_twenty_numeric_int WHERE id2 IN (0, 1, 900):'
SELECT * FROM test_twenty_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_twenty_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_twenty_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_twenty_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_twenty_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_twenty_numeric_int WHERE id2 IN (0, 1, 900);
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_twenty_numeric_int WHERE id2 IN (0, 1, 900);

-----------------
-- Twenty-five --
-----------------
\echo 'SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 1:'
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 1;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 1;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 1;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 1;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 1;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 1;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 1;


\echo 'SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 501:'
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 501;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 501;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 501;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 501;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 501;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 501;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 501;


\echo 'SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 900:'
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 900;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 900;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 900;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 900;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 900;
SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 900;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_twentyfive_numeric_int WHERE id2 = 900;


\echo 'SELECT * FROM test_twentyfive_numeric_int WHERE id2 IN (0, 1, 900):'
SELECT * FROM test_twentyfive_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_twentyfive_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_twentyfive_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_twentyfive_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_twentyfive_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_twentyfive_numeric_int WHERE id2 IN (0, 1, 900);
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_twentyfive_numeric_int WHERE id2 IN (0, 1, 900);

-----------
-- Fifty --
-----------
\echo 'SELECT * FROM test_fifty_numeric_int WHERE id2 = 1:'
SELECT * FROM test_fifty_numeric_int WHERE id2 = 1;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 1;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 1;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 1;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 1;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 1;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_fifty_numeric_int WHERE id2 = 1;


\echo 'SELECT * FROM test_fifty_numeric_int WHERE id2 = 501:'
SELECT * FROM test_fifty_numeric_int WHERE id2 = 501;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 501;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 501;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 501;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 501;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 501;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_fifty_numeric_int WHERE id2 = 501;


\echo 'SELECT * FROM test_fifty_numeric_int WHERE id2 = 900:'
SELECT * FROM test_fifty_numeric_int WHERE id2 = 900;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 900;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 900;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 900;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 900;
SELECT * FROM test_fifty_numeric_int WHERE id2 = 900;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_fifty_numeric_int WHERE id2 = 900;


\echo 'SELECT * FROM test_fifty_numeric_int WHERE id2 IN (0, 1, 900):'
SELECT * FROM test_fifty_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_fifty_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_fifty_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_fifty_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_fifty_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_fifty_numeric_int WHERE id2 IN (0, 1, 900);
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_fifty_numeric_int WHERE id2 IN (0, 1, 900);

------------------
-- Five-hundred --
------------------
\echo 'SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 1:'
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 1;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 1;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 1;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 1;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 1;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 1;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 1;


\echo 'SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 501:'
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 501;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 501;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 501;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 501;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 501;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 501;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 501;


\echo 'SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 900:'
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 900;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 900;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 900;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 900;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 900;
SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 900;
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_five_hundred_numeric_int WHERE id2 = 900;


\echo 'SELECT * FROM test_five_hundred_numeric_int WHERE id2 IN (0, 1, 900):'
SELECT * FROM test_five_hundred_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_five_hundred_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_five_hundred_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_five_hundred_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_five_hundred_numeric_int WHERE id2 IN (0, 1, 900);
SELECT * FROM test_five_hundred_numeric_int WHERE id2 IN (0, 1, 900);
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM test_five_hundred_numeric_int WHERE id2 IN (0, 1, 900);
