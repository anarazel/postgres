drop table if exists small_sales_mdam_paper;
create unlogged table small_sales_mdam_paper
(
  dept int4,
  sdate date,
  item_class serial,
  store int4,
  item int4,
  total_sales numeric
);
create index small_mdam_idx on small_sales_mdam_paper(dept, sdate, item_class, store);

insert into small_sales_mdam_paper (dept, sdate, item_class, store, total_sales)
-- total_sales is pretty much just a filler column:
-- omit "item" (which is serial column):
select
  dept,
  '1995-01-01'::date + sdate,
  item_class,
  store,
  (random() * 500.0) as total_sales
from
  -- 10 departments (like in the NOT IN() example, I guess):
  generate_series(1, 10) dept,
  -- 45 days, starting on Jan 1 of 95:
  generate_series(1, 45) sdate,
  -- Arbitrarily assuming 75 total for my standard MDAM table, let's make it 20:
  generate_series(1, 20) item_class,
  -- Arbitrarily assuming 300 total for my standard MDAM table, let's make it 50:
  generate_series(1, 50) store;

/*
:ea select
  sdate,
  item_class,
  store,
  sum(total_sales)
from
  small_sales_mdam_paper
where

  sdate between '1995-01-01' and '1995-01-05'
  and item_class in (5, 10, 15)
  and store in (25, 45)
group by
  sdate,
  item_class,
  store;
*/
