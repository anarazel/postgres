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

drop table if exists outer_tab;
drop table if exists primscanmarkcov_table;
reset client_min_messages;

create unlogged table outer_tab(
  a int,
  b int
);
create index outer_tab_idx on outer_tab(a, b) with (deduplicate_items = off);

create unlogged table primscanmarkcov_table(
  a int,
  b int
);
create index interesting_coverage_idx on primscanmarkcov_table(a, b) with (deduplicate_items = off);

insert into outer_tab             select 1, i from generate_series(1530, 1780) i;
insert into primscanmarkcov_table select 1, i from generate_series(1530, 1780) i;

insert into outer_tab             select 1, 1550 from generate_series(1, 200) i;
insert into primscanmarkcov_table select 1, 1551 from generate_series(1, 200) i;

vacuum analyze outer_tab;
vacuum analyze primscanmarkcov_table ;

----------------------------------------------------------------------------------------------
-- One duplicate value per leaf page must not visit too many extra leaf pages speculatively --
----------------------------------------------------------------------------------------------

set client_min_messages=error;
drop table if exists duplicate_test;
reset client_min_messages;

create unlogged table duplicate_test(dup int4);
create index on duplicate_test (dup);
insert into duplicate_test
select val from generate_series(1, 18) val,
                generate_series(1,100000) dups_per_val;
vacuum analyze duplicate_test; -- Be tidy
