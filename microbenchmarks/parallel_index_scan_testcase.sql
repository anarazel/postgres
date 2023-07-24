-- encourage use of parallel plans
set parallel_setup_cost=000.1;
set parallel_tuple_cost=000.1;
set min_parallel_table_scan_size=1;
set min_parallel_index_scan_size=1;
set enable_seqscan=off;
set enable_bitmapscan=off;
-- Index-only scan for this
set enable_indexonlyscan=on;
set log_btree_verbosity=1;

SELECT 20 AS row_count
\gset

create unlogged table parallel_index_scan_medium
(
  c1 int
);

select (select not exists(select * from pg_class where relname = 'parallel_index_scan') or (select (count(*) / 31) != :'row_count' from parallel_index_scan_medium)) as load_data
       \gset

\if :load_data
  set client_min_messages=error;
  drop table if exists parallel_index_scan;
  reset client_min_messages;

  create unlogged table parallel_index_scan
  (
    c1 int,
    c2 text,
    c3 date,
    c4 varchar(20),
    c5 float
  );

  create index parallel_index_scan_idx on parallel_index_scan(c3, c4, c5);

  insert into parallel_index_scan
    select
      x,
      'c2_' || x,
      '2000-01-01'::date + y,
      'xyz',
      1.1
    from
      generate_series(1, :'row_count') x,
      generate_series(0,30) y;

  vacuum analyze parallel_index_scan_medium;
\endif

set log_btree_verbosity=0;
show port;
set max_parallel_workers_per_gather=1;

/*
select count(*) as first_count, c3 c3_six_rows
from parallel_index_scan
where c3 in (
  '2000-01-01',
  '2000-01-03',
  '2000-01-04',
  '2000-01-05',
  '2000-01-07',
  '2000-01-09',
  '1111-11-11'
)
group by c3;
EXPLAIN (ANALYZE, BUFFERS)
select count(*) as first_count, c3 c3_six_rows
from parallel_index_scan
where c3 in (
  '2000-01-01',
  '2000-01-03',
  '2000-01-04',
  '2000-01-05',
  '2000-01-07',
  '2000-01-09',
  '1111-11-11'
)
group by c3;

select count(*) as second_count, c3 c3_ten_rows
from parallel_index_scan
where c3 in (
  '2000-01-01',
  '2000-01-02',
  '2000-01-03',
  '2000-01-04',
  '2000-01-05',
  '2000-01-06',
  '2000-01-07',
  '2000-01-08',
  '2000-01-09',
  '2000-01-10'
)
group by c3;
EXPLAIN (ANALYZE, BUFFERS)
select count(*) as second_count, c3 c3_ten_rows
from parallel_index_scan
where c3 in (
  '2000-01-01',
  '2000-01-02',
  '2000-01-03',
  '2000-01-04',
  '2000-01-05',
  '2000-01-06',
  '2000-01-07',
  '2000-01-08',
  '2000-01-09',
  '2000-01-10'
)
group by c3;

select count(*) as third_count, c3 c3_five_rows
from parallel_index_scan
where c3 in (
  '2000-01-01',
  '2000-01-03',
  '2000-01-05',
  '2000-01-07',
  '2000-01-09'
)
group by c3;
EXPLAIN (ANALYZE, BUFFERS)
select count(*) as third_count, c3 c3_five_rows
from parallel_index_scan
where c3 in (
  '2000-01-01',
  '2000-01-03',
  '2000-01-05',
  '2000-01-07',
  '2000-01-09'
)
group by c3;
*/

select count(*) as zebra_count, c3 c3_sixteen_rows
from parallel_index_scan
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
group by c3;

select count(*) as zebra_count_backwards, c3 c3_sixteen_rows
from parallel_index_scan
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
group by c3 order by c3 desc;

select count(*) as solid_count, c3 c3_sixteen_rows
from parallel_index_scan
where c3 in (
  '2000-01-01',
  '2000-01-02',
  '2000-01-03',
  '2000-01-04',
  '2000-01-05',
  '2000-01-06',
  '2000-01-07',
  '2000-01-08',
  '2000-01-09',
  '2000-01-10',
  '2000-01-11',
  '2000-01-12',
  '2000-01-13',
  '2000-01-14',
  '2000-01-15',
  '2000-01-16',
  '2000-01-17',
  '2000-01-18',
  '2000-01-19',
  '2000-01-20',
  '2000-01-21',
  '2000-01-22',
  '2000-01-23',
  '2000-01-24',
  '2000-01-25',
  '2000-01-26',
  '2000-01-27',
  '2000-01-28',
  '2000-01-29',
  '2000-01-30',
  '2000-01-31'
)
group by c3;
