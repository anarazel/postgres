set work_mem='100MB';
set effective_io_concurrency=100;
set effective_cache_size='24GB';
set maintenance_io_concurrency=100;
set random_page_cost=2.0;
set track_io_timing to off;
set enable_seqscan to off;
set client_min_messages=error;
set vacuum_freeze_min_age = 0;
create extension if not exists pageinspect; -- just to have it
reset client_min_messages;

-- Set log_btree_verbosity to 1 without depending on having that patch
-- applied (HACK, just sets commit_siblings instead when we don't have that
-- patch available):
select set_config((select coalesce((select name from pg_settings where name = 'log_btree_verbosity'), 'commit_siblings')), '1', false);

---------------------------------------
-- James Coleman's test case variant --
---------------------------------------

-- Per https://www.postgresql.org/message-id/flat/CAAaqYe-SsHgXKXPpjn7WCTUnB_RQSxXjpSaJd32aA%3DRquv0AgQ%40mail.gmail.com,
-- though I'm going to use my own index definition for this

set client_min_messages=error;
drop table if exists coleman_regress;
reset client_min_messages;
select setseed(0.12345); -- Need deterministic test case
create unlogged table coleman_regress(
  bar_fk integer,
  created_at timestamptz
);

create index index_coleman_regress on coleman_regress(created_at, bar_fk);

insert into coleman_regress(bar_fk, created_at)
select i % 1000, '2000-01-01'::timestamptz -(random() * '5 years'::interval)
from generate_series(1, 500000) t(i);

VACUUM (freeze,analyze) coleman_regress;

-- This cannot really skip, but must not pay too high a cost in CPU cycles for
-- being open to the possibility of skipping:
set skipscan_prefix_cols = 32;
select *
from coleman_regress
where bar_fk = 1
order by created_at
limit 15;
-- 76 buffer hits total for parity with master:
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- need "costs off" here
select *
from coleman_regress
where bar_fk = 1
order by created_at
limit 15;

-- Same again, with skipping disabled using GUC (representative of master):
set skipscan_prefix_cols = 0;
select *
from coleman_regress
where bar_fk = 1
order by created_at
limit 15;
-- 76 buffer hits total for parity with master:
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- need "costs off" here
select *
from coleman_regress
where bar_fk = 1
order by created_at
limit 15;
