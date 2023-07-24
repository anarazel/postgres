drop table if exists sales_mdam_paper;
create unlogged table sales_mdam_paper
(
  dept int4,
  sdate date,
  item_class serial,
  store int4,
  item int4,
  total_sales numeric
);

-- 900 hundred million rows in total here:
insert into sales_mdam_paper (dept, sdate, item_class, store, total_sales)
select
  dept,
  '1995-01-01'::date + sdate,
  item_class,
  store,
  (random() * 500.0) as total_sales
from
  -- "So let us assume that the values for the column dept in the table range from 1 through 100":
  generate_series(1, 100) dept,
  -- 400 days, starting on Jan 2 of 95 (arbitrary):
  generate_series(1, 400) sdate,
  -- Highest item_class in paper is 50, so arbitrarily assume 75 total:
  generate_series(1, 75) item_class,
  -- Highest store in paper is 250, so arbitrarily assume 300 total:
  generate_series(1, 300) store;

-- Duration of INSERT on my workstation:
--
-- INSERT 0 900000000
-- Time: 900564.138 ms (15:00.564)
--
-- pg@regression:5432 [2574594]=# \dt+ sales_mdam_paper
--                                         List of relations
-- ┌────────┬──────────────────┬───────┬───────┬─────────────┬───────────────┬───────┬─────────────┐
-- │ Schema │       Name       │ Type  │ Owner │ Persistence │ Access method │ Size  │ Description │
-- ├────────┼──────────────────┼───────┼───────┼─────────────┼───────────────┼───────┼─────────────┤
-- │ public │ sales_mdam_paper │ table │ pg    │ unlogged    │ heap          │ 51 GB │ ∅           │
-- └────────┴──────────────────┴───────┴───────┴─────────────┴───────────────┴───────┴─────────────┘
-- (1 row)

-- Build index, using the column order from the paper:
create index mdam_idx on sales_mdam_paper(dept, sdate, item_class, store);

-- Duration of index build on my workstation:
--
-- CREATE INDEX
-- Time: 271098.310 ms (04:31.098)
--
-- pg@regression:5432 [2574594]=# \di+ mdam_idx
--                                              List of relations
-- ┌────────┬──────────┬───────┬───────┬──────────────────┬─────────────┬───────────────┬───────┬─────────────┐
-- │ Schema │   Name   │ Type  │ Owner │      Table       │ Persistence │ Access method │ Size  │ Description │
-- ├────────┼──────────┼───────┼───────┼──────────────────┼─────────────┼───────────────┼───────┼─────────────┤
-- │ public │ mdam_idx │ index │ pg    │ sales_mdam_paper │ unlogged    │ btree         │ 26 GB │ ∅           │
-- └────────┴──────────┴───────┴───────┴──────────────────┴─────────────┴───────────────┴───────┴─────────────┘
-- (1 row)

------------------------------------------
-- "Intervening Range Predicates" query --
------------------------------------------
-- select
--   dept,
--   sdate,
--   sum(total_sales)
-- from
--   sales_mdam_paper
-- where
-- dept = 10 and
--   sdate between '1995-06-01' and '1995-06-30'
--   and item_class = 20
--   and store = 250
-- group by dept, sdate
-- order by dept, sdate;

-----------------------------------
-- "Missing Key Predicate" query --
-----------------------------------
-- select
--   dept,
--   sdate,
--   sum(total_sales)
-- from
--   sales_mdam_paper
-- where
--   sdate between '1995-06-01' and '1995-06-30'
--   and item_class = 20
--   and store = 250
-- group by dept, sdate
-- order by dept, sdate;

--------------------------------------------------------------------------------
-- Fully skip over all distinct dept and sdate columns variant (not in paper) --
--------------------------------------------------------------------------------

-- pg@regression:5432 [2911894]=# EXPLAIN (ANALYZE, WAL, VERBOSE, SUMMARY, BUFFERS, SERIALIZE)
-- select
-- dept,
-- sdate,
-- item_class,
-- store,
-- sum(total_sales)
-- from
-- sales_mdam_paper
-- where
-- item_class in (20, 35, 50)
-- and store in (200, 250)
-- group by dept, sdate, item_class, store
-- order by dept, sdate, item_class, store;
-- ┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
-- │                                                                         QUERY PLAN                                                                         │
-- ├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
-- │ GroupAggregate  (cost=0.57..13885222.76 rows=101182 width=48) (actual time=0.018..708.726 rows=240000 loops=1)                                             │
-- │   Output: dept, sdate, item_class, store, sum(total_sales)                                                                                                 │
-- │   Group Key: sales_mdam_paper.dept, sales_mdam_paper.sdate, sales_mdam_paper.item_class, sales_mdam_paper.store                                            │
-- │   Buffers: shared hit=807301                                                                                                                               │
-- │   ->  Index Scan using mdam_idx on public.sales_mdam_paper  (cost=0.57..13882692.36 rows=101250 width=48) (actual time=0.015..609.007 rows=240000 loops=1) │
-- │         Output: dept, sdate, item_class, store, item, total_sales                                                                                          │
-- │         Index Cond: ((sales_mdam_paper.item_class = ANY ('{20,35,50}'::integer[])) AND (sales_mdam_paper.store = ANY ('{200,250}'::integer[])))            │
-- │         Buffers: shared hit=807301                                                                                                                         │
-- │ Planning:                                                                                                                                                  │
-- │   Buffers: shared hit=1                                                                                                                                    │
-- │ Planning Time: 0.075 ms                                                                                                                                    │
-- │ Serialization: time=52.940 ms  output=12847kB  format=text                                                                                                 │
-- │ Execution Time: 777.687 ms                                                                                                                                 │
-- └────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
-- (13 rows)

-----------------------------------------------------------------------------
-- Headline: Patch performance (with prewarmed cache) for "IN Lists" query --
-----------------------------------------------------------------------------

-- EXPLAIN (ANALYZE, WAL, VERBOSE, SUMMARY, BUFFERS, SERIALIZE)
-- select
--   dept,
--   sdate,
--   item_class,
--   store,
--   sum(total_sales)
-- from
--   sales_mdam_paper
-- where
--   sdate between '1995-06-01' and '1995-06-30'
--   and item_class in (20, 35, 50)
--   and store in (200, 250)
-- group by dept, sdate, item_class, store
-- order by dept, sdate, item_class, store;
-- ┌───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
-- │                                                                                                                      QUERY PLAN                                                                                                                       │
-- ├───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
-- │ GroupAggregate  (cost=0.57..36541.85 rows=17688 width=48) (actual time=0.017..46.270 rows=18000 loops=1)                                                                                                                                              │
-- │   Output: dept, sdate, item_class, store, sum(total_sales)                                                                                                                                                                                            │
-- │   Group Key: sales_mdam_paper.dept, sales_mdam_paper.sdate, sales_mdam_paper.item_class, sales_mdam_paper.store                                                                                                                                       │
-- │   Buffers: shared hit=60537                                                                                                                                                                                                                           │
-- │   ->  Index Scan using mdam_idx on public.sales_mdam_paper  (cost=0.57..36099.63 rows=17690 width=27) (actual time=0.014..38.569 rows=18000 loops=1)                                                                                                  │
-- │         Output: dept, sdate, item_class, store, item, total_sales                                                                                                                                                                                     │
-- │         Index Cond: ((sales_mdam_paper.sdate >= '1995-06-01'::date) AND (sales_mdam_paper.sdate <= '1995-06-30'::date) AND (sales_mdam_paper.item_class = ANY ('{20,35,50}'::integer[])) AND (sales_mdam_paper.store = ANY ('{200,250}'::integer[]))) │
-- │         Buffers: shared hit=60537                                                                                                                                                                                                                     │
-- │ Planning:                                                                                                                                                                                                                                             │
-- │   Buffers: shared hit=1                                                                                                                                                                                                                               │
-- │ Planning Time: 0.067 ms                                                                                                                                                                                                                               │
-- │ Serialization: time=4.066 ms  output=964kB  format=text                                                                                                                                                                                               │
-- │ Execution Time: 51.493 ms                                                                                                                                                                                                                             │
-- └───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
-- (13 rows)

--------------------------------------------------------------------------------
-- Patch performance with same query, while only skipping using "dept" column --
--------------------------------------------------------------------------------

-- pg@regression:5432 [2574594]=# set skipscan_prefix_cols = 1; -- temporary debug GUC
-- SET
-- pg@regression:5432 [2574594]=# EXPLAIN (ANALYZE, WAL, VERBOSE, SUMMARY, BUFFERS, SERIALIZE)
-- select
--   dept,
--   sdate,
--   item_class,
--   store,
--   sum(total_sales)
-- from
--   sales_mdam_paper
-- where
--   sdate between '1995-06-01' and '1995-06-30'
--   and item_class in (20, 35, 50)
--   and store in (200, 250)
-- group by dept, sdate, item_class, store
-- order by dept, sdate, item_class, store;
-- ┌───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
-- │                                                                                                                      QUERY PLAN                                                                                                                       │
-- ├───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
-- │ GroupAggregate  (cost=0.57..36541.85 rows=17688 width=48) (actual time=0.024..3275.078 rows=18000 loops=1)                                                                                                                                            │
-- │   Output: dept, sdate, item_class, store, sum(total_sales)                                                                                                                                                                                            │
-- │   Group Key: sales_mdam_paper.dept, sales_mdam_paper.sdate, sales_mdam_paper.item_class, sales_mdam_paper.store                                                                                                                                       │
-- │   Buffers: shared hit=274768                                                                                                                                                                                                                          │
-- │   ->  Index Scan using mdam_idx on public.sales_mdam_paper  (cost=0.57..36099.63 rows=17690 width=27) (actual time=0.020..3266.190 rows=18000 loops=1)                                                                                                │
-- │         Output: dept, sdate, item_class, store, item, total_sales                                                                                                                                                                                     │
-- │         Index Cond: ((sales_mdam_paper.sdate >= '1995-06-01'::date) AND (sales_mdam_paper.sdate <= '1995-06-30'::date) AND (sales_mdam_paper.item_class = ANY ('{20,35,50}'::integer[])) AND (sales_mdam_paper.store = ANY ('{200,250}'::integer[]))) │
-- │         Buffers: shared hit=274768                                                                                                                                                                                                                    │
-- │ Planning:                                                                                                                                                                                                                                             │
-- │   Buffers: shared hit=1                                                                                                                                                                                                                               │
-- │ Planning Time: 0.095 ms                                                                                                                                                                                                                               │
-- │ Serialization: time=4.693 ms  output=964kB  format=text                                                                                                                                                                                               │
-- │ Execution Time: 3281.059 ms                                                                                                                                                                                                                           │
-- └───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
-- (13 rows)

--------------------------------------------------------------------------------------
-- Patch performance with same query, no skipping at all (representative of master) --
--------------------------------------------------------------------------------------

-- pg@regression:5432 [2574594]=# set skipscan_prefix_cols = 0; -- temporary debug GUC
-- SET
-- pg@regression:5432 [2574594]=# EXPLAIN (ANALYZE, WAL, VERBOSE, SUMMARY, BUFFERS, SERIALIZE)
-- select
--   dept,
--   sdate,
--   item_class,
--   store,
--   sum(total_sales)
-- from
--   sales_mdam_paper
-- where
--   sdate between '1995-06-01' and '1995-06-30'
--   and item_class in (20, 35, 50)
--   and store in (200, 250)
-- group by dept, sdate, item_class, store
-- order by dept, sdate, item_class, store;
-- select
--   dept,
--   sdate,
--   item_class,
--   store,
--   sum(total_sales)
-- from
--   sales_mdam_paper
-- where
--   sdate between '1995-06-01' and '1995-06-30'
--   and item_class in (20, 35, 50)
--   and store in (200, 250)
-- group by dept, sdate, item_class, store
-- order by dept, sdate, item_class, store;
-- ┌───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
-- │                                                                                                                      QUERY PLAN                                                                                                                       │
-- ├───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
-- │ GroupAggregate  (cost=0.57..36541.85 rows=17688 width=48) (actual time=101.639..32399.167 rows=18000 loops=1)                                                                                                                                         │
-- │   Output: dept, sdate, item_class, store, sum(total_sales)                                                                                                                                                                                            │
-- │   Group Key: sales_mdam_paper.dept, sales_mdam_paper.sdate, sales_mdam_paper.item_class, sales_mdam_paper.store                                                                                                                                       │
-- │   Buffers: shared hit=94668 read=3371611 written=676147                                                                                                                                                                                               │
-- │   I/O Timings: shared read=8714.998 write=3360.723                                                                                                                                                                                                    │
-- │   ->  Index Scan using mdam_idx on public.sales_mdam_paper  (cost=0.57..36099.63 rows=17690 width=27) (actual time=101.626..32387.234 rows=18000 loops=1)                                                                                             │
-- │         Output: dept, sdate, item_class, store, item, total_sales                                                                                                                                                                                     │
-- │         Index Cond: ((sales_mdam_paper.sdate >= '1995-06-01'::date) AND (sales_mdam_paper.sdate <= '1995-06-30'::date) AND (sales_mdam_paper.item_class = ANY ('{20,35,50}'::integer[])) AND (sales_mdam_paper.store = ANY ('{200,250}'::integer[]))) │
-- │         Buffers: shared hit=94668 read=3371611 written=676147                                                                                                                                                                                         │
-- │         I/O Timings: shared read=8714.998 write=3360.723                                                                                                                                                                                              │
-- │ Planning:                                                                                                                                                                                                                                             │
-- │   Buffers: shared hit=1                                                                                                                                                                                                                               │
-- │ Planning Time: 0.101 ms                                                                                                                                                                                                                               │
-- │ Serialization: time=5.535 ms  output=964kB  format=text                                                                                                                                                                                               │
-- │ Execution Time: 32406.373 ms                                                                                                                                                                                                                          │
-- └───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
-- (15 rows)
