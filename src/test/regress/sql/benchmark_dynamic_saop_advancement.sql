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

---------------------------------------------------
-- ORDER BY column comes first, SAOPs after that --
---------------------------------------------------
set client_min_messages=error;
drop table if exists docs_testcase;
reset client_min_messages;
select setseed(0.12345); -- Need deterministic test case
create unlogged table docs_testcase
(
  id serial,
  type text default 'pdf' not null,
  status text not null,
  sender_reference text not null,
  sent_at timestamptz,
  created_at timestamptz default '2000-01-01' not null
);
create index mini_idx on docs_testcase using btree(sent_at desc NULLS last, sender_reference, status);
insert into docs_testcase(type, status, sender_reference, sent_at)
select
  ('{pdf,doc,raw}'::text[]) [ceil(random() * 3)],
  ('{sent,draft,suspended}'::text[]) [ceil(random() * 3)],
  ('{Custom,Client}'::text[]) [ceil(random() * 2)] || '/' || floor(random() * 2000),
  ('2000-01-01'::timestamptz - interval '2 years' * random())::timestamptz
from
  generate_series(1, 100000) g;
vacuum analyze docs_testcase;

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;
set enable_sort to off;

select * from docs_testcase
where
  status in ('draft', 'sent') and
  sender_reference in ('Custom/1175', 'Client/362', 'Custom/280')
order by
  sent_at desc NULLS last
limit 20;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- need "costs off" here
select * from docs_testcase
where
  status in ('draft', 'sent') and
  sender_reference in ('Custom/1175', 'Client/362', 'Custom/280')
order by
  sent_at desc NULLS last
limit 20;

-- Index-only scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to on;
set enable_indexscan to off;

select sent_at, status, sender_reference from docs_testcase
where
  status in ('draft', 'sent') and
  sender_reference in ('Custom/1175', 'Client/362', 'Custom/280')
order by
  sent_at desc NULLS last
limit 20;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- need "costs off" here
select sent_at, status, sender_reference from docs_testcase
where
  status in ('draft', 'sent') and
  sender_reference in ('Custom/1175', 'Client/362', 'Custom/280')
order by
  sent_at desc NULLS last
limit 20;

-------------------------------
-- James Coleman's test case --
-------------------------------

-- Per https://www.postgresql.org/message-id/flat/CAAaqYe-SsHgXKXPpjn7WCTUnB_RQSxXjpSaJd32aA%3DRquv0AgQ%40mail.gmail.com,
-- though I'm going to use my own index definition for this

set client_min_messages=error;
drop table if exists coleman;
reset client_min_messages;
select setseed(0.12345); -- Need deterministic test case
create unlogged table coleman(
  bar_fk integer,
  created_at timestamptz
);

-- Original index (commented out):
-- create index index_coleman_on_bar_fk_and_created_at on coleman(bar_fk, created_at);

-- my preferred index:
create index index_coleman_on_created_and_at_bar_fk on coleman(created_at, bar_fk);

insert into coleman(bar_fk, created_at)
select i % 1000, '2000-01-01'::timestamptz -(random() * '5 years'::interval)
from generate_series(1, 500000) t(i);

VACUUM (freeze,analyze) coleman;

-- Index-only scan:
select *
from coleman
where bar_fk in (1, 2, 3)
order by created_at
limit 50;
-- 76 buffer hits total for parity with master:
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- need "costs off" here
select *
from coleman
where bar_fk in (1, 2, 3)
order by created_at
limit 50;

alter table coleman add column noise int4; -- no more index only scans

-- Index scan:
select *
from coleman
where bar_fk in (1, 2, 3)
order by created_at
limit 50;
-- 125 buffer hits for patch, master does 16713 hits!:
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- need "costs off" here
select *
from coleman
where bar_fk in (1, 2, 3)
order by created_at
limit 50;
