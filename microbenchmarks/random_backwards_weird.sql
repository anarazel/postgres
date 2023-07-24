drop table if exists t;
drop table if exists t_randomized;
set synchronize_seqscans=off;
create unlogged table t (a bigint, b text) with (fillfactor = 20);
create unlogged table t_randomized (a bigint, b text) with (fillfactor = 20);

insert into t
select a, b
from (select
    r,
    a,
    b,
    generate_series(0, 32 - 1) as p
  from (
    select
      row_number() over () as r,
      a,
      b
    from (
      select
        i as a,
        md5(i::text) as b
      from
        generate_series(1, 312500) s(i)
      order by
        (i + 1 *(random() - 0.5))) foo) bar) baz
order by ((r * 32 + p) + 8 *(random() - 0.5));
create index t_pk  on t(a ASC) with (deduplicate_items=off);

insert into t_randomized
select a, b
from (select
    r,
    a,
    b,
    generate_series(0, 32 - 1) as p
  from (
    select
      row_number() over () as r,
      a,
      b
    from (
      select
        i as a,
        md5(i::text) as b
      from
        generate_series(1, 312500) s(i)
      order by
        (i + 1 *(random() - 0.5))) foo) bar) baz
order by ((r * 32 + p) + 8 *(random() - 0.5));
create index t_randomized_pk on t_randomized(a ASC) with (deduplicate_items=off);
create index randomizer on t_randomized (hashint8(a));
cluster t_randomized using randomizer;

vacuum freeze;
analyze;
checkpoint;

\echo '#### Now run queries_random_backwards_weird.sql for query ####'
