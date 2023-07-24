set log_btree_verbosity=0;

-- Notes:
--
-- 2_500_000 is required for patch to beat master on simple case with
-- group aggregate that groups on just the date alone, and has no lower-order
-- scan keys in qual.  We can win even with only one parallel worker there,
-- provided the index isn't very small.  Plus we can win convincingly when we
-- have (say) 10 parallel workers.
--
-- The picture is more mixed with query when it has 3 grouping columns and 3
-- scan keys in qual -- even for 2_500_000 index.  We can still win when there
-- are a few workers on 2_500_000 sized index, but we lose by a little but when
-- there's only 1 -- think 2764.579 ms for patch vs 2138.537 ms on master
-- (with horse server).  I deem that to be acceptable.  With (say) 5 parallel
-- workers (verified we're getting that many on master too using EXPLAIN ANALYZE),
-- we get 597.089 ms for patch vs 735.284 ms with master.  You can go even
-- further with that (say by adding 10 workers) and patch is more than twice
-- as fast at some point.


-- SELECT 50_000 AS row_count
-- SELECT 250_000 AS row_count
SELECT 2_500_000 AS row_count

\gset

create unlogged table parallel_index_scan_medium
(
  c1 int
);

set max_parallel_workers_per_gather=0;

select (select not exists(select * from pg_class where relname = 'parallel_index_scan_medium') or (select (count(*) / 31) != :'row_count' from parallel_index_scan_medium)) as load_data
       \gset

\if :load_data
  set client_min_messages=error;
  drop table if exists parallel_index_scan_medium;
  reset client_min_messages;

  create unlogged table parallel_index_scan_medium
  (
    c1 int,
    c2 text,
    c3 date,
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
set max_parallel_workers_per_gather=10;

-- EXPLAIN (ANALYZE, BUFFERS)
select count(*) as zebra_count,
c3
-- ,c4
-- ,c5
from parallel_index_scan_medium
where c3 in (
  '2000-01-01',
  '2000-01-03',
  '2000-01-05',
  '2000-01-07',
  '2000-01-09',
  '2000-01-11',
  '2000-01-13',
  '2000-01-15',
  '2000-01-17',
  '2000-01-19',
  '2000-01-21',
  '2000-01-23',
  '2000-01-25',
  '2000-01-27',
  '2000-01-29',
  '2000-01-31'
)
-- and c4 in ('xyz0', 'xyz2', 'xyz4')
-- and c5 in (1,2,3,4,5)
group by
c3
-- , c4
-- , c5
;
show max_parallel_workers_per_gather;

--order by c3 desc, c4 desc, c5 desc;
--;
