\pset pager off
set client_min_messages='notice';
set enable_seqscan=off;
set enable_bitmapscan=off;

drop table if exists high_low_high_card;
create unlogged table high_low_high_card(
  skippy int4,
  key int4,
  type text
);
create index on high_low_high_card(skippy, key);

insert into high_low_high_card
select
  (abs(hashint4(i % 5)) + 250) % (10000 + 250),
  abs(hashint4(i + 42)) % 100_000,
  'fat'
from
  generate_series(1, 100000) i;

with card as (
  select
    i skippy,
    abs(hashint4(i + j)) % 10000,
    'skinny'
  from
    generate_series(1, 10000) i,
    generate_series(1, 10) j
),
oth as (
  select
    *
  from
    card c
  where
    not exists (
      select
        *
      from
        high_low_high_card h
      where
        c.skippy = h.skippy)
)
insert into high_low_high_card
select
  *
from
  oth;
vacuum analyze high_low_high_card;

\echo 'PATCH (skipscan_prefix_cols=32), select * from high_low_high_card where key = 40 order by skippy, key query:'
SET skipscan_prefix_cols=32;
select * from high_low_high_card where key = 40 order by skippy, key;
select * from high_low_high_card where key = 40 order by skippy, key;
select * from high_low_high_card where key = 40 order by skippy, key;
\echo 'Patch ~1.8ms, master ~3.3ms ^^^\n\n\n'


\echo 'MASTER (skipscan_prefix_cols=0), select * from high_low_high_card where key = 40 order by skippy, key query:'
SET skipscan_prefix_cols=0;
select * from high_low_high_card where key = 40 order by skippy, key;
select * from high_low_high_card where key = 40 order by skippy, key;
select * from high_low_high_card where key = 40 order by skippy, key;
\echo 'Patch ~1.8ms, master ~3.3ms ^^^\n\n\n'


\echo 'PATCH (skipscan_prefix_cols=32), select * from high_low_high_card where key = 37 order by skippy, key query:'
SET skipscan_prefix_cols=32;
select * from high_low_high_card where key = 37 order by skippy, key;
select * from high_low_high_card where key = 37 order by skippy, key;
select * from high_low_high_card where key = 37 order by skippy, key;
\echo 'Patch ~1.8ms, master ~3.3ms ^^^\n\n\n'


\echo 'MASTER (skipscan_prefix_cols=0), select * from high_low_high_card where key = 37 order by skippy, key query:'
SET skipscan_prefix_cols=0;
select * from high_low_high_card where key = 37 order by skippy, key;
select * from high_low_high_card where key = 37 order by skippy, key;
select * from high_low_high_card where key = 37 order by skippy, key;
\echo 'Patch ~1.8ms, master ~3.3ms ^^^\n\n\n'


\echo 'PATCH (skipscan_prefix_cols=32), select * from high_low_high_card where key = 38 order by skippy, key query:'
SET skipscan_prefix_cols=32;
select * from high_low_high_card where key = 38 order by skippy, key;
select * from high_low_high_card where key = 38 order by skippy, key;
select * from high_low_high_card where key = 38 order by skippy, key;
\echo 'Patch ~1.8ms, master ~3.3ms ^^^\n\n\n'


\echo 'MASTER (skipscan_prefix_cols=0), select * from high_low_high_card where key = 38 order by skippy, key query:'
SET skipscan_prefix_cols=0;
select * from high_low_high_card where key = 38 order by skippy, key;
select * from high_low_high_card where key = 38 order by skippy, key;
select * from high_low_high_card where key = 38 order by skippy, key;
\echo 'Patch ~1.8ms, master ~3.3ms ^^^\n\n\n'


\echo 'PATCH (skipscan_prefix_cols=32), select * from high_low_high_card where key = 13 order by skippy, key query:'
SET skipscan_prefix_cols=32;
select * from high_low_high_card where key = 13 order by skippy, key;
select * from high_low_high_card where key = 13 order by skippy, key;
select * from high_low_high_card where key = 13 order by skippy, key;
\echo 'Patch ~1.85ms, master ~3.4ms ^^^\n\n\n'


\echo 'MASTER (skipscan_prefix_cols=0), select * from high_low_high_card where key = 13 order by skippy, key query:'
SET skipscan_prefix_cols=0;
select * from high_low_high_card where key = 13 order by skippy, key;
select * from high_low_high_card where key = 13 order by skippy, key;
select * from high_low_high_card where key = 13 order by skippy, key;
\echo 'Patch ~1.85ms, master ~3.4ms ^^^\n\n\n'


\echo 'PATCH (skipscan_prefix_cols=32), select * from high_low_high_card where key = 15 order by skippy, key query:'
SET skipscan_prefix_cols=32;
select * from high_low_high_card where key = 15 order by skippy, key;
select * from high_low_high_card where key = 15 order by skippy, key;
select * from high_low_high_card where key = 15 order by skippy, key;
\echo 'Patch ~1.8ms, master ~3.3ms ^^^\n\n\n'


\echo 'MASTER (skipscan_prefix_cols=0), select * from high_low_high_card where key = 15 order by skippy, key query:'
SET skipscan_prefix_cols=0;
select * from high_low_high_card where key = 15 order by skippy, key;
select * from high_low_high_card where key = 15 order by skippy, key;
select * from high_low_high_card where key = 15 order by skippy, key;
\echo 'Patch ~1.8ms, master ~3.3ms ^^^\n\n\n'
