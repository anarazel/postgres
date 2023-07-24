-- This test case involves parallel index scan locking up

--set parallel_leader_participation to off;

set work_mem='100MB';
set effective_cache_size='24GB';
set random_page_cost=2.0;
set track_io_timing to off;
set enable_seqscan to off;
set client_min_messages=error;
set vacuum_freeze_min_age = 0;
set cursor_tuple_fraction=1.000;
create extension if not exists pageinspect; -- just to have it
reset client_min_messages;

-- Set log_btree_verbosity to 1 without depending on having that patch
-- applied (HACK, just sets commit_siblings instead when we don't have that
-- patch available):
select set_config((select coalesce((select name from pg_settings where name = 'log_btree_verbosity'), 'commit_siblings')), '1', false);

set parallel_setup_cost=000.1;
set parallel_tuple_cost=000.1;
set min_parallel_table_scan_size=1;
set min_parallel_index_scan_size=1;
set max_parallel_workers_per_gather=2;
set enable_bitmapscan to off;
set enable_memoize to on;
set enable_hashjoin to off;
--set statement_timeout='10s';
set enable_mergejoin to off;
set enable_material to off;
set debug_parallel_query=on;
set client_min_messages=error;

set enable_mergejoin = off;
set enable_nestloop = on;

prepare nestloopjoin_qry as
with range_ints as ( select i from generate_series(1530, 1780) i)
select
  count(*), buggy.a, buggy.b from
outer_tab o
  inner join
primscanmarkcov_table buggy
  on o.a = buggy.a and o.b = buggy.b
where
  o.a = 1     and     o.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])  and
  buggy.a = 1 and buggy.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])
group by buggy.a, buggy.b
order by buggy.a, buggy.b;

execute nestloopjoin_qry;

----------------------------------------------------------------------------------------------
-- One duplicate value per leaf page must not visit too many extra leaf pages speculatively --
----------------------------------------------------------------------------------------------

-- Index-only scan:
set enable_indexonlyscan to on;

-- Showed problem with leader scanning way too many pages uselessly:
select count(*), dup from duplicate_test
where dup = any ('{3,12,15,18}')
group by dup order by dup;

-- Original:
select count(*), dup from duplicate_test
where dup = any ('{3,6,9,12,15,18}')
group by dup order by dup;
