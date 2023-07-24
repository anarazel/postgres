-- Test case taken from:
--
-- https://postgr.es/m/aa55adf3-6466-4324-92e6-5ef54e7c3918@iki.fi
--
-- He used this query:
--
-- set enable_seqscan=off; set max_parallel_workers_per_gather=0;
-- select count(*) from skiptest where b=1;
--

-- Setup:
drop table if exists skiptest;
drop table if exists skiptest_small;

create unlogged table skiptest as
select g / 10 as a, g % 10 as b
from generate_series(1, 10_000_000) g;

create unlogged table skiptest_small as
select g / 10 as a, g % 10 as b
from generate_series(1, 10_000) g;

vacuum freeze skiptest;
vacuum freeze skiptest_small;
create index on skiptest (a, b);
create index on skiptest_small (a, b);

