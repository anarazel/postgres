set log_btree_verbosity=0;

-- See also: microbenchmarks/parallel_index_scan_testcase_medium.sql
SELECT 2_500_000 AS row_count
-- SELECT 50_000 AS row_count

\gset

create table parallel_index_scan_medium
(
  c1 int,
  c2 int,
  c3 timestamptz
);

set max_parallel_workers_per_gather=0;

select (select not exists(select * from pg_class where relname = 'parallel_index_scan_medium') or (select (count(*) != :'row_count') from parallel_index_scan_medium where c3 = '2000-01-01')) as load_data
       \gset

\if :load_data
  set client_min_messages=error;
  drop table if exists parallel_index_scan_medium;
  reset client_min_messages;

  create unlogged table parallel_index_scan_medium
  (
    c1 int,
    c2 text,
    c3 timestamptz,
    c4 varchar(20),
    c5 float
  );

  create index parallel_index_scan_medium_idx on parallel_index_scan_medium(c3, c4, c5);

  insert into parallel_index_scan_medium
    select
      x,
      'c2_' || x,
      '2000-01-01'::date + y,
      'xyz' || (x % 5),
      (x % 5)
    from
      generate_series(1, :'row_count') x,
      generate_series(0,30) y;

  -- Now do one with a c1 that's all NULLs:
  insert into parallel_index_scan_medium
  select c1, c2, NULL, c4, c5 from parallel_index_scan_medium where c3 = '2000-01-01';

  vacuum (freeze, analyze) parallel_index_scan_medium;
\endif

show port;

set parallel_setup_cost=000.1;
set parallel_tuple_cost=000.1;
set min_parallel_table_scan_size=1;
set min_parallel_index_scan_size=1;
set enable_seqscan=off;
set enable_bitmapscan=off;
set work_mem='1GB';
-- set enable_indexonlyscan=off;
-- set enable_indexscan=on;

set log_btree_verbosity=1;
set parallel_leader_participation=on;
-- set skipscan_prefix_cols = 0;
set max_parallel_workers_per_gather=0;
set max_parallel_workers_per_gather=6;

-- EXPLAIN (ANALYZE, BUFFERS)
select count(*) as zebra_count,
c3
,c4
,c5
from parallel_index_scan_medium

where

-- c3 between '2000-01-21' and '2000-01-31' and

-- c3 in (
--   '2000-01-01',
--   '2000-01-03',
--   '2000-01-05',
--   '2000-01-07',
--   '2000-01-09',
--   '2000-01-11',
--   '2000-01-13',
--   '2000-01-15',
--   '2000-01-17',
--   '2000-01-19',
--   '2000-01-21',
--   '2000-01-23',
--   '2000-01-25',
--   '2000-01-27',
--   '2000-01-29',
--   '2000-01-31'
-- )

-- and

c4 in ('xyz0', 'xyz2', 'xyz4')

and c5 in (1,2,3,4,5)
group by
c3
, c4
, c5
order by c3, c4, c5
;
show max_parallel_workers_per_gather;

--order by c3 desc, c4 desc, c5 desc;
--;
