set enable_indexonlyscan to off;
set enable_seqscan to off;
set enable_indexscan to off;
set enable_bitmapscan to on;

set client_min_messages=error;
-- set skipscan_skipsupport_enabled=false;
drop table if exists scan_key_test;
drop table if exists scan_key_unsupported_test;
drop table if exists scan_key_lowest_highest_opclass_func_test;
drop table if exists opfamily_skipscan_test_table;
drop table if exists opfamily_skipscan_test_table_wide;
drop operator family if exists test_skipscan_family using btree cascade;
drop function if exists my_int8_skip_sort(int8,int8) cascade;
reset client_min_messages;

create unlogged table scan_key_test(
  a int,
  b int,
  c int,
  d int,
  e int,
  f int,
  g int,
  h int
);

create index scan_key_test_idx on scan_key_test(a, b, c, d, e, f, g, h);

set client_min_messages=log;
set log_btree_verbosity=1;

-- Simplest possible (no skipping, just first column):
select * from scan_key_test where a = 1;

-- Skip column 'a' only:
select * from scan_key_test where b = 1;

-- Skip columns 'a' and 'b' only:
select * from scan_key_test where c = 1;

-- Skip columns 'a' and 'b' only -- don't convert c to range skip array:
select * from scan_key_test where c > 1;

-- Skip columns 'a' and 'b' only -- don't convert c to range skip array:
select * from scan_key_test where c >= 1;

-- Skip columns 'a' and 'b' only -- don't convert c to range skip array:
select * from scan_key_test where c < 1;

-- (July 3) Example taken from https://www.postgresql.org/docs/current/indexes-multicolumn.html
select * from scan_key_test where a = 5 and b >= 42 and c < 77;

------------------------
-- a, b, c test cases --
------------------------

-- True skip columns 'a' only -- carry over b, and don't convert c to range skip array:
select * from scan_key_test where b = 1 and c <= 1;

-- True skip columns 'a' only -- carry over b, and don't convert c to range skip array:
select * from scan_key_test where b = 1 and c > 1;

-- True skip columns 'a' only -- carry over b, and don't convert c to range skip array:
select * from scan_key_test where b = 1 and c >= 1;

-- True skip columns 'a' only -- carry over b, and don't convert c to range skip array:
select * from scan_key_test where b = 1 and c < 1;

-- True skip columns 'a' only -- carry over b, and don't convert c to range skip array:
select * from scan_key_test where b = 1 and c <= 1;

------------------------------------------
-- a, b, c test cases, but now with GUC --
------------------------------------------

-- Similar to last few tests, in that "a" always gets a skip scan attribute.
-- But dissimilar in that we keep inequalities as-is:
set skipscan_prefix_cols = 1;

-- Skip columns 'a' only -- carry over b, and carry over c scan key with <= strategy:
select * from scan_key_test where b = 1 and c <= 1;

-- Skip columns 'a' only -- carry over b, and carry over c scan key with > strategy:
select * from scan_key_test where b = 1 and c > 1;

-- Skip columns 'a' only -- carry over b, and carry over c scan key with >= strategy:
select * from scan_key_test where b = 1 and c >= 1;

-- Skip columns 'a' only -- carry over b, and carry over c scan key with < strategy:
select * from scan_key_test where b = 1 and c < 1;

-- Skip columns 'a' only -- carry over b, and carry over c scan key with <= strategy:
select * from scan_key_test where b = 1 and c <= 1;

-- Increment GUC to 2:
set skipscan_prefix_cols = 2;
-- Same queries again, but (since b=1 in all cases) get the same output scan
-- keys as previous skipscan_prefix_cols=1 variants of these same tests
-- (the right to put a skip scan key on "b" isn't what's missing here):
select * from scan_key_test where b = 1 and c <= 1;
select * from scan_key_test where b = 1 and c > 1;
select * from scan_key_test where b = 1 and c >= 1;
select * from scan_key_test where b = 1 and c < 1;
select * from scan_key_test where b = 1 and c <= 1;

-- Increment GUC to 3:
set skipscan_prefix_cols = 3;
-- Same queries again, but (since c is attnum 3) we get the maximum number of
-- possibly-useful skip arrays -- even the "c" inequality scan keys get
-- converted to range style skip scan keys (we're back to the GUC-less
-- behavior, which is the default behavior for the patch):
--
-- (XXX Update June 21) Actually, c no longer gets converted to a skip array
select * from scan_key_test where b = 1 and c <= 1;
select * from scan_key_test where b = 1 and c > 1;
select * from scan_key_test where b = 1 and c >= 1;
select * from scan_key_test where b = 1 and c < 1;
select * from scan_key_test where b = 1 and c <= 1;

-- Increment GUC to 4:
set skipscan_prefix_cols = 4;
-- Let's be absolutely sure that further increases make no difference to the
-- behavior of skip array preprocessing:
select * from scan_key_test where b = 1 and c <= 1;
select * from scan_key_test where b = 1 and c > 1;
select * from scan_key_test where b = 1 and c >= 1;
select * from scan_key_test where b = 1 and c < 1;
select * from scan_key_test where b = 1 and c <= 1;

-- Disable all skipping, even on attribute a/attnum 1:
set skipscan_prefix_cols = 0;
select * from scan_key_test where b = 1 and c <= 1;
select * from scan_key_test where b = 1 and c > 1;
select * from scan_key_test where b = 1 and c >= 1;
select * from scan_key_test where b = 1 and c < 1;
select * from scan_key_test where b = 1 and c <= 1;

reset skipscan_prefix_cols;

----------------------
-- end of GUC tests --
----------------------

-- Skip middle column, carry over a and c:
select * from scan_key_test where a = 1 and c = 1;

-- Skip middle columns, carry over a and d:
select * from scan_key_test where a = 1 and d = 1;

-- Skip middle columns, carry over a and e:
select * from scan_key_test where a = 1 and e = 1;

-- Skip middle column, carry over a and c:
select * from scan_key_test where a = 1 and c >= 1;

-- Skip middle columns, carry over a and d:
select * from scan_key_test where a = 1 and d >= 1;

-- Skip middle columns, carry over a and e:
select * from scan_key_test where a = 1 and e >= 1;

-- Skip column 'a' only, no skip range on b (b inequality is output):
select * from scan_key_test where b between 1 and 42;

-- Skip column 'a' and 'b' only, no skip range on c:
select * from scan_key_test where c between 1 and 42;

-- Skip column 'b' only, range on c:
select * from scan_key_test where a = 1 and c between 1 and 3;

-- Range of 'a', skip column 'b' only, range on c:
select * from scan_key_test where a between 1 and 3 and c between 1 and 3;

-- Range of 'a', range on 'b', 'c' gets output as an inequality sk:
select * from scan_key_test where a between 1 and 3 and b between 1 and 3 and c between 1 and 3;

-- Range of 'a', range on 'b', range on c, 'd' gets output as an inequality sk:
select * from scan_key_test where a between 1 and 3 and b between 1 and 3 and c between 1 and 3 and d between 1 and 3;

-- Range on 'a' only, 'd' gets output as an inequality sk:
select * from scan_key_test where a between 1 and 3 and d between 1 and 3;

-- skips 'a', range on b, point on c:
select * from scan_key_test where b between 1 and 3 and c = 1;

-- Range of 'a', range on d, = on e:
select * from scan_key_test where a between 1 and 3 and d between 1 and 3 and e = 1;

-- skips 'a', range on b, point on c, = on e:
select * from scan_key_test where b between 1 and 3 and c = 1 and e = 1;

-- skips 'b' only, 'c' gets output as an inequality sk:
select * from scan_key_test where a= 1 and b >= 5 and c >= 4;

-- skips 'b' only, 'c' gets output as an inequality sk:
select * from scan_key_test where a= 1 and c >= 4;

-- skips 'b' only:
select * from scan_key_test where a= 1 and c = 4 and d = 5;

-- skips 'b' range condition only:
 select * from scan_key_test where a= 1 and b between 1 and 2 and c = 4 and d = 5;

-- skips 'b' and e only:
select * from scan_key_test where a= 1 and c = 4 and d = 5 and f = 1;

-- Very complicated variant #1:
select * from scan_key_test where a= 1 and c = 5 and f = 4 and g between 1 and 3 and h = 1;

-- Very complicated variant #2:
select * from scan_key_test where c = 5 and f = 4 and g between 1 and 3 and h = 1;

create unlogged table scan_key_unsupported_test(
  a int,
  b int,
  c text,
  d int,
  e text,
  f int,
  g text,
  h int
);

create index scan_key_unsupported_test_idx on scan_key_unsupported_test(a, b, c, d, e, f, g, h);

-- Skip columns 'a' and 'b' successfully, in spite of not supporting text for
-- skipping:
select * from scan_key_unsupported_test where c = 'foo';

-- Skip columns 'a' and 'b' and 'd' successfully, in spite of not supporting text for
-- skipping:
select * from scan_key_unsupported_test where c = 'foo' and e = 'bar';

-- Skip columns 'a' and 'b' successfully, but not c or even d (d is int but
-- doesn't matter because it's after c, a text column):
--
-- (XXX June 18 UPDATE): Actually, why should we be skipping anything?  We're
-- at least slightly better off not adding any skip attributes here, since
-- they're only worth a damn when they can make skipping possible -- that
-- means that at least one non-skip/input scan key has to become required as a
-- result of our adding skip arrays.  Here we can't use "e" to skip because
-- that's made unsafe by the lack of support for text on column "c".  So we
-- add no skip attributes whatsoever.
--
-- (XXX July 6 UPDATE): Actually, we now support text as a skippable type, so
-- this no longer applies at all.  Skip everything.
select * from scan_key_unsupported_test where e = 'bar';

-- (June 22)
-- Same again, but this time it's an inequality instead of an equality:
--
-- (XXX July 6 UPDATE): Actually, we now support text as a skippable type, so
-- this no longer applies at all.  Skip everything.
select * from scan_key_unsupported_test where e > 'bar';

-- (June 22)
-- Addition of the "c" allows us to produce required scan keys for all
-- attributes (a through to e):
select * from scan_key_unsupported_test where c = 'fizz' and e = 'bar';

-- (June 22)
-- This will be no less true if it's an inequality on e instead:
select * from scan_key_unsupported_test where c = 'fizz' and e >= 'bar';

-- (June 22)
-- But it does matter if the inequality is on "c", as far as the later
-- attributes go (the "c >=" scan key can be marked required in one direction
-- only, no scan key on "d", non-required scan key on "e"):
--
-- (UPDATE July 12): Actually, now we support text, so required scan key up to
-- and including on 'e' here
select * from scan_key_unsupported_test where c >= 'fizz' and e = 'bar';

-- (June 18)
-- Same again, just with an extra non-skip scan key for "d":
--
-- (UPDATE July 12): Actually, now we support text, so required scan key up to
-- and including on 'e' here
select * from scan_key_unsupported_test where d = 5 and e = 'bar';

-- (June 18)
-- Same again, just with an extra non-skip scan key for "a":
--
-- (UPDATE July 12): Actually, now we support text, so required scan key up to
-- and including on 'e' here
select * from scan_key_unsupported_test where a = 666 and e = 'bar';

-- Skip columns 'a' successfully, but not c or even d (d is int but
-- doesn't matter because it's after c, a text column):
--
-- (XXX July 12 UPDATE): Actually, we now support text as a skippable type, so
-- this no longer applies at all.  Skip everything.
select * from scan_key_unsupported_test where b = 22 and e = 'bar';

-- Skip columns 'a' and 'b' successfully, in spite of not supporting text for
-- skipping, but no range skip scan key for c:
select * from scan_key_unsupported_test where c between 'a' and 'z';

-- Skip columns 'a' and 'b' successfully, in spite of not supporting text for
-- skipping, but no range skip scan key for c, nor a skip scan key for d:
--
-- (UPDATE July 12): Actually, now we support text, so required scan key up to
-- and including on 'e' here
select * from scan_key_unsupported_test where c between 'a' and 'z' and e = 'bar';

-------------------------------------
-- Opfamily dec/inc function tests --
-------------------------------------

-- Show off the lowest and highest elements according to opclass support
-- functions for all currently supported types
create unlogged table scan_key_lowest_highest_opclass_func_test(
  a boolean,
  b date,
  c int2,
  d int4,
  e int8,
  f oid,
  g uuid,
  h "char",
  noskipone text,
  noskiptwo text
);

create index show_low_elems on scan_key_lowest_highest_opclass_func_test(a asc nulls last,
                                                                         b asc nulls last,
                                                                         c asc nulls last,
                                                                         d asc nulls last,
                                                                         e asc nulls last,
                                                                         f asc nulls last,
                                                                         g asc nulls last,
                                                                         h asc nulls last,
                                                                         noskipone asc nulls last,
                                                                         noskiptwo asc nulls last);
select * from scan_key_lowest_highest_opclass_func_test where noskipone = 'foo';

drop index show_low_elems;
create index show_high_elems on scan_key_lowest_highest_opclass_func_test(a desc nulls last,
                                                                          b desc nulls last,
                                                                          c desc nulls last,
                                                                          d desc nulls last,
                                                                          e desc nulls last,
                                                                          f desc nulls last,
                                                                          g desc nulls last,
                                                                          h desc nulls last,
                                                                          noskipone desc nulls last,
                                                                          noskiptwo desc nulls last);
select * from scan_key_lowest_highest_opclass_func_test where noskipone = 'foo';

-- (June 18)
--
-- Verify behavior around RowCompare scan keys.  They're not supported, but we
-- should be able to do everything short of generating a skip array for them.
-- It should be just like a regular inequality that happens to lack skip
-- support; we want and expect backfilling to still take place, where
-- possible.
select
  *
from
  scan_key_lowest_highest_opclass_func_test
where
  b = '1995-01-01' -- Distraction
  and
  (d, e) > (3, 44); -- can't skip row compare, but doesn't matter because it comes last anyway
select
  *
from
  scan_key_lowest_highest_opclass_func_test
where
  (d, e) > (3, 44); -- can't skip row compare, but doesn't matter because it comes last anyway
select
  *
from
  scan_key_lowest_highest_opclass_func_test
where
  b = '1995-01-01' -- Distraction
  and
  (d, e) > (3, 44) -- limits what we can do now
  and
  e = 123; -- cannot be marked required
select
  *
from
  scan_key_lowest_highest_opclass_func_test
where
  (d, e) > (3, 44) -- limits what we can do now
  and
  f = 123; -- cannot be marked required, either

select
  *
from
  scan_key_lowest_highest_opclass_func_test
where
  noskiptwo = 'foo'; -- can't skip "noskipone" so shouldn't get any skip arrays XXX Not anymore

select
  *
from
  scan_key_lowest_highest_opclass_func_test
where
  a = true -- A distraction that shouldn't change picture for later columns
  and
  noskiptwo = 'foo'; -- can't skip "noskipone" so shouldn't get any skip arrays XXX Not anymore

select
  *
from
  scan_key_lowest_highest_opclass_func_test
where
  a between false and true -- A distraction that shouldn't change picture for later columns
  and
  noskiptwo = 'foo'; -- can't skip "noskipone" so shouldn't get any skip arrays XXX Not anymore

-- (UPDATE June 21) Part of the challenge here (which I missed at first) was
-- making sure that we don't convert the "c" inequalities to a skip array --
-- it's not just the scan keys after that that shouldn't be transformed here.
select
  *
from
  scan_key_lowest_highest_opclass_func_test
where
  a = true -- A distraction that shouldn't change picture for later columns
  and
  b = '1995-01-01' -- Another distraction
  and
  c between -1000 and 10000 -- Another distraction
  and
  noskiptwo = 'foo'; -- can't skip "noskipone" so shouldn't get any skip arrays XXX Not anymore

-- (UPDATE June 21) Part of the challenge here (which I missed at first) was
-- making sure that we don't convert the "c" inequalities to a skip array --
-- it's not just the scan keys after that that shouldn't be transformed here.
select
  *
from
  scan_key_lowest_highest_opclass_func_test
where
  a = true -- A distraction that shouldn't change picture for later columns
  and
  b = '1995-01-01' -- Another distraction
  and
  c between -1000 and 10000 -- Another distraction
  and
  noskiptwo between 'foo' and 'fooz'; -- can't skip "noskipone", doesn't matter that this isn't = anymore

select
  *
from
  scan_key_lowest_highest_opclass_func_test
where
  a = true -- A distraction that shouldn't change picture for later columns
  and
  b = '1995-01-01' -- Another distraction
  and
  noskiptwo = 'foo'; -- can't skip "noskipone" so shouldn't get any skip arrays XXX Not anymore

select
  *
from
  scan_key_lowest_highest_opclass_func_test
where
  noskipone = 'fizz' -- This time we should add skip arrays for "a" - "h" due to the presence of this
  and
  noskiptwo = 'foo'; -- This is still here (same as last test), but doesn't matter now

-------------------------------
-- Incomplete opfamily tests --
-------------------------------

-- Same-type procs:
create function my_int8_skip_sort(int8,int8) returns int language sql
  as $$ select case when $1 = $2 then 0 when $1 > $2 then 1 else -1 end; $$;

create operator family test_skipscan_family using btree;

create operator class test_skipscan_family_int8_ops for type int8 using btree family test_skipscan_family as
  operator 1 < (int8,int8),
  operator 2 <= (int8,int8),
  -- Omit this: operator 3 = (int8,int8),
  operator 4 >= (int8,int8),
  operator 5 > (int8,int8),
  function 1 my_int8_skip_sort(int8,int8)
  ;

create unlogged table opfamily_skipscan_test_table(foo int8, bar int8);
create index on opfamily_skipscan_test_table(foo test_skipscan_family_int8_ops, bar);
insert into opfamily_skipscan_test_table
  values (365, 365),
         (366, 366),
         (367, 367),
         (32767, 32767),
         (8589934591, 8589934591),
         (8589934592, 8589934592);
vacuum analyze opfamily_skipscan_test_table; -- Be tidy

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Test how gracefully we handle having a missing = operator for opfamily:
select * from opfamily_skipscan_test_table where bar = 366::bigint;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from opfamily_skipscan_test_table where bar = 366::bigint;

-- Wide variant adds test coverage for an issue in v14, pointed out by
-- Masahiro Ikeda here:
-- https://postgr.es/m/51d00219180323d121572e1f83ccde2a@oss.nttdata.com
create unlogged table opfamily_skipscan_test_table_wide(
  prefixa int8,
  prefixb int8,
  prefixc int8,
  foo int8,
  bar int8
);
create index on opfamily_skipscan_test_table_wide(prefixa, prefixb, prefixc, foo test_skipscan_family_int8_ops, bar);
insert into opfamily_skipscan_test_table_wide
  values (365, 365, 365, 365, 365),
         (366, 366, 366, 366, 366),
         (367, 367, 367, 367, 367),
         (32767, 32767, 32767, 32767, 32767),
         (8589934591, 8589934591, 8589934591, 8589934591, 8589934591),
         (8589934592, 8589934592, 8589934592, 8589934592, 8589934592);
vacuum analyze opfamily_skipscan_test_table_wide; -- Be tidy

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Test how gracefully we handle having a missing = operator for opfamily:
select * from opfamily_skipscan_test_table_wide where bar = 366::bigint;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from opfamily_skipscan_test_table_wide where bar = 366::bigint;
