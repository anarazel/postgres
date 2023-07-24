-- > Tomas Vondra:
-- got a crash in the btree-random test, here's a backtrace + reproducer
--
--> Peter:
-- I actually saw that crash myself, before starting any of this work on
-- redesigning the yield mechanism.  But never fixed it because I couldn't
-- repro it in isolation. Think it's due to the refactoring that made
-- change-in-direction handling just reset (not end) the read stream.
--
--> Peter:
-- I think that I can fix it by moving the assertion. I didn't do that because
-- I wasn't yet sure why it'd be necessary. Bet I can debug this fairly
-- easily.
--
-- > Peter:
-- I think that when I saw it the generated repro didn't work because the
-- original crash relied on concurrent activity.
--
--> Tomas Vondra:
-- not sure, it worked for me without any concurrent activity
--
--> Peter:
-- I'm not surprised that that'd also be possible. Anyway, not important now,
-- I'll just fix it.

drop table if exists xs_pause_assert;

create unlogged table xs_pause_assert (a bigint, b bigint, c bigint, d bigint, e bigint, f bigint, g bigint) with (fillfactor = 92);
create index on xs_pause_assert (a, b, c, d, e, f, g) with (fillfactor = 19, deduplicate_items = off);

insert into xs_pause_assert
select (i / 479), (i / 251), (i / 317), (i / 251), (i / 67), (i / 19), (i / 157)
from generate_series(1, 100000) s(i)
order by i + mod(i::bigint * 57990, 24), md5(i::text);

vacuum freeze xs_pause_assert;
analyze xs_pause_assert;


set enable_seqscan = off;
set enable_bitmapscan = off;
set enable_indexonlyscan = off;
set cursor_tuple_fraction = 1.0;
begin;

declare c_1 scroll cursor for
  select *
  from xs_pause_assert
  order by a asc, b asc, c asc, d asc, e asc, f asc, g asc;

fetch forward 100809 from c_1;
select pg_buffercache_evict_all();
fetch backward 39 from c_1;
fetch backward 97335 from c_1;
fetch backward 43 from c_1;
fetch forward 70 from c_1;
commit;
