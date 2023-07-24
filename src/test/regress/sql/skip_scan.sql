set work_mem='100MB';
set effective_cache_size='24GB';
set random_page_cost=2.0;
set track_io_timing to off;
set enable_seqscan to off;
set client_min_messages=error;
-- set skipscan_skipsupport_enabled=false;
-- set skipscan_prefix_cols=0;
set vacuum_freeze_min_age = 0;
set cursor_tuple_fraction=1.000;

-- Set aio stuff explicitly:
set io_combine_limit = 16;
set effective_io_concurrency = 100;

create extension if not exists pageinspect; -- just to have it
-- set statement_timeout='4s';
reset client_min_messages;

-- Set log_btree_verbosity to 1 without depending on having that patch
-- applied (HACK, just sets commit_siblings instead when we don't have that
-- patch available):
select set_config((select coalesce((select name from pg_settings where name = 'log_btree_verbosity'), 'commit_siblings')), '1', false);

-- Establish if this server is master or the patch -- want to skip stress
-- tests if it's the latter
--
-- Reminder: Don't vary the database state between master and patch (just the
-- tests run, which must be read-only)
select (setting = '5432') as testing_patch from pg_settings where name = 'port'
       \gset

-- Quick sanity check, to make it obvious when you forgot to initdb correctly:
-- Shows the available skip support routines in the database
select
  amp.oid as skip_proc_oid,
  amp.amproc::regproc as proc,
  opf.opfname as opfamily_name,
  opc.opcname as opclass_name,
  opc.opcintype::regtype as opcintype
from pg_am as am
join pg_opclass as opc on opc.opcmethod = am.oid
join pg_opfamily as opf on opc.opcfamily = opf.oid
join pg_amproc as amp on amp.amprocfamily = opf.oid and
    amp.amproclefttype = opc.opcintype and amp.amprocnum = 6
where am.amname = 'btree'
order by 1, 2, 3, 4;

set client_min_messages=error;
drop table if exists fuzz_skip_scan;
reset client_min_messages;
\getenv abs_srcdir PG_ABS_SRCDIR
\set filename :abs_srcdir '/data/fuzz_table.sql'
\i :filename;

-- Force index scan
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Forward can test case for bug where we neglected to make NULL tuple values always advance skip arrays
--
-- Recall that this is the initial bug that Mark Dilger found, where we don't
-- return tuples with NULLs.  It was fixed by commit b75fedca.
select *
from fuzz_skip_scan
where b = 12 and d < 16
order by a, b, c, d;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where b = 12 and d < 16
order by a, b, c, d;

-- Test case associated with failure to call _bt_start_array_keys() query #1:
select *
from fuzz_skip_scan
where
  a in (9, 10) and b < 3 and c > 42
order by a desc, b desc, c desc, d desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where
  a in (9, 10) and b < 3 and c > 42
order by a desc, b desc, c desc, d desc;

-- Test case associated with failure to call _bt_start_array_keys() query #2:
select *
from fuzz_skip_scan
where b in (12, 13) and c >= 59 and d < 1887
order by a desc, b desc, c desc, d desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where b in (12, 13) and c >= 59 and d < 1887
order by a desc, b desc, c desc, d desc;

-- Test case associated with failure to call _bt_start_array_keys() query #3:
select *
from fuzz_skip_scan
where
  b in (7, 8) and c is not null and d < 2348
order by a desc, b desc, c desc, d desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where
  b in (7, 8) and c is not null and d < 2348
order by a desc, b desc, c desc, d desc;

-- Test case associated with failure to call _bt_start_array_keys() query #4
-- (only forwards scan test, uses alternative NULLS FIRST index):
select *
from fuzz_skip_scan
where b in (19, 20) and c >= 80 and d < 230
order by a nulls first, b nulls first, c nulls first, d nulls first;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where b in (19, 20) and c >= 80 and d < 230
order by a nulls first, b nulls first, c nulls first, d nulls first;

-- Test case associated with segfault/assertion failure:
--
-- fix for this bug:
/*
diff --git a/src/backend/access/nbtree/nbtutils.c b/src/backend/access/nbtree/nbtutils.c
index cb6b74912..2fbbbc31f 100644
--- a/src/backend/access/nbtree/nbtutils.c
+++ b/src/backend/access/nbtree/nbtutils.c
@@ -1826,7 +1826,7 @@ _bt_advance_array_keys(IndexScanDesc scan, BTReadPageState *pstate,

        if (_bt_check_compare(scan, dir, tuple, tupnatts, tupdesc, false,
-                             false, &continuescan,
+                             !sktrig_required, &continuescan,
                              &nsktrig) &&
            !so->scanBehind)
        {
*/
select *
from fuzz_skip_scan
where
  c is not null and d >= 8600 and d <= 8640
order by a desc, b desc, c desc, d desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where
  c is not null and d >= 8600 and d <= 8640
order by a desc, b desc, c desc, d desc;

-- 2026-01-09 14:24
--
-- With prefetching enabled, this got an assertion failure:
--
-- TRAP: failed Assert("ScanDirectionIsForward(dir) ? !(cur->sk_flags & SK_BT_MAXVAL) : !(cur->sk_flags & SK_BT_MINVAL)"), File: "../source/src/backend/access/nbtree/nbtreadpage.c", Line: 1968
--
-- Tomas:
--
-- Anyway, I fixed that by properly setting the "finished" flag, and also resetting it when the direction changes.
-- But it all these ad hoc flags feel a bit wonky / loosely defined.

select
  *
from
  fuzz_skip_scan
where
  c >= 36
  and c <= 53
  and d = 3515
order by a, b, c, d;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  *
from
  fuzz_skip_scan
where
  c >= 36
  and c <= 53
  and d = 3515
order by a, b, c, d;

-- Test coverage for _bt_set_startikey IS NOT NULL path:
/*
index 91ff52868..7783fc29f 100644
--- a/src/backend/access/nbtree/nbtutils.c
+++ b/src/backend/access/nbtree/nbtutils.c
@@ -2577,8 +2577,8 @@ _bt_set_startikey(IndexScanDesc scan, BTReadPageState *pstate)
                 Assert(key->sk_flags & SK_SEARCHNOTNULL);

-                if (firstnull || lastnull)
-                    break;      /* unsafe */
+                // if (firstnull || lastnull)
+                //  break;      /* unsafe */
*/
select count(*)
from fuzz_skip_scan
where c is not null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*)
from fuzz_skip_scan
where c is not null;

-- Test coverage for _bt_set_startikey IS NULL path:
select *
from fuzz_skip_scan
where a >= 7 and a <= 7 and b is null
order by a desc, b desc, c desc, d desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF)
select *
from fuzz_skip_scan
where a >= 7 and a <= 7 and b is null
order by a desc, b desc, c desc, d desc;

-- More _bt_set_startikey test coverage, for simple = keys (should return no
-- rows, buggy code can return a row):
select *
from fuzz_skip_scan
where a = 12 and b = 9 and c is not null and d >= 4730 and d <= 4780
order by a desc, b desc, c desc, d desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where a = 12 and b = 9 and c is not null and d >= 4730 and d <= 4780
order by a desc, b desc, c desc, d desc;

-- 2025-06-08 10:51
-- Test case showing that I was wrong to remove forcenonrequired argument from
-- _bt_check_rowcompare function, since it leads to an assertion failure here:
select a, b, c, d
from fuzz_skip_scan
where b is not null and (c, d) < (60, 0)
order by a, b, c, d
limit 200 offset 80_000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b, c, d
from fuzz_skip_scan
where b is not null and (c, d) < (60, 0)
order by a, b, c, d
limit 200 offset 80_000;


-- 2025-06-11 17:21
-- Similar though crashier test case to the above forcenonrequired test case
--
-- This test case will actually segfault on a release build due to pstate NULL
-- pointer dereference inside _bt_advance_array_keys.  Keep it around, just in
-- case.
select *
from fuzz_skip_scan
where a = 19 and (c, d) >=(54, 5976) and c <= 58
order by a desc, b desc, c desc, d desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where a = 19 and (c, d) >=(54, 5976) and c <= 58
order by a desc, b desc, c desc, d desc;

-- 2025-06-18 17:46
-- RowCompares probably don't need to disable forcenonrequired when we reach
-- them within _bt_set_startikey, after all.  However, that requires better
-- _bt_first row compare initial positioning logic in the presence of NULL row
-- members.  We have good tests for that elsewhere.  This test exists to prove
-- that it is also necessary to make the relevant "NULL row member/row
-- element" code within _bt_check_rowcompare more particular about the attno
-- of the required scan key that it gets to via "subkey--"; there cannot be a
-- "index attribute gap" between it and the NULL row member.
--
-- Here the gap is between "b" and "d" row members.  Since we have a skip
-- array on "a", and since we're now going to allow forcenonrequired with row
-- compares, this test case will spin ceaselessly without those
-- _bt_set_startikey changes playing their part in avoiding confusion:
select *
from fuzz_skip_scan
where (b, d) >= (6, null) and (b, d) <= (7, null)
order by a desc, b desc, c desc, d desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where (b, d) >= (6, null) and (b, d) <= (7, null)
order by a desc, b desc, c desc, d desc;

-- 2025-06-19 21:10
--
-- Following v2 of RowCompare patch, add this test to prove that we need to be
-- careful about stopping the scan on a NULL for a scan key not marked
-- required in the current scan direction (unless dealing with the first row
-- member's scan key)
select *
from fuzz_skip_scan
where (a, b, c) >= (4, 12, 64) and (a, b, c) <= (7, null, 0) and d = 4904
order by a, b, c, d;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where (a, b, c) >= (4, 12, 64) and (a, b, c) <= (7, null, 0) and d = 4904
order by a, b, c, d;

-- 2025-06-19 21:15
--
-- Essentially tests the same thing as last time, except now we show it with a
-- backwards scan, instead of a forwards scan (which necessitates use of the
-- alternative NULLS FIRST index, since it's only wrong that way around)
select *
from fuzz_skip_scan
where (a, b) > (7, 11) and (a, b) < (11, 0)
  and c is not null
  and d = 3765
order by a desc NULLS last, b desc NULLS last, c desc NULLS last, d desc NULLS last;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where (a, b) > (7, 11) and (a, b) < (11, 0)
  and c is not null
  and d = 3765
order by a desc NULLS last, b desc NULLS last, c desc NULLS last, d desc NULLS last;

-- 2025-06-20 17:22
--
-- It comes to light on Friday (day after Juneteenth) that use of
-- forcenonrequired mode with a RowCompare key isn't actually made safe by my
-- RowCompare patch; at least not when we mix in potentially redundant or
-- contradictory scalar keys.
--
-- The following are a few samples of this -- "can't forcenonrequired with
-- contradictory inequality" tests

-- "can't forcenonrequired with contradictory inequality" test # 1
select *
from fuzz_skip_scan
where
(b, d) > (7, 10000) and b > 15 and (b, d) <= (8, 4766) and d < 5861
order by a, b, c, d;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where
(b, d) > (7, 10000) and b > 15 and (b, d) <= (8, 4766) and d < 5861
order by a, b, c, d;

-- "can't forcenonrequired with contradictory inequality" test # 2
select *
from fuzz_skip_scan
where
a <= 9 and a is not null and (b, d) > (4, 3634) and b > 8 and (b, d) < (9, 0) and c = 23 and d >= 4909
order by a NULLS first, b NULLS first, c NULLS first, d NULLS first;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where
a <= 9 and a is not null and (b, d) > (4, 3634) and b > 8 and (b, d) < (9, 0) and c = 23 and d >= 4909
order by a NULLS first, b NULLS first, c NULLS first, d NULLS first;

-- "can't forcenonrequired with contradictory inequality" test # 3
select *
from fuzz_skip_scan
where a > 0 and a is not null and (b, c, d) >= (8, null, null) and b < 4 and (b, c, d) <= (11, 61, 2787) and c > 15
order by a desc NULLS last, b desc NULLS last, c desc NULLS last, d desc NULLS last;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where a > 0 and a is not null and (b, c, d) >= (8, null, null) and b < 4 and (b, c, d) <= (11, 61, 2787) and c > 15
order by a desc NULLS last, b desc NULLS last, c desc NULLS last, d desc NULLS last;

-- 2025-06-20 17:52
--
-- "ERROR:  pageNum 730 already visited" with patch enabled that does that,
-- this is a forwards scan with more contradictory inequalities:
select *
from fuzz_skip_scan
where a > 0 and a is not null and (b, c, d) >=(8, null, null)
  and b < 4 and (b, c, d) <= (11, 61, 2787)
  and c > 15
order by
  a NULLS first,
  b NULLS first,
  c NULLS first,
  d NULLS first;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where a > 0 and a is not null and (b, c, d) >=(8, null, null)
  and b < 4 and (b, c, d) <= (11, 61, 2787)
  and c > 15
order by
  a NULLS first,
  b NULLS first,
  c NULLS first,
  d NULLS first;

-- 2025-06-21 12:30
--
-- "However, on a forwards scan, we must keep going, because we may have
-- initially positioned to the start of the index". -- verifies that we didn't
-- get this point wrong in _bt_check_rowcompare (just have it for paranoia's sake)
select *
from fuzz_skip_scan
where a = 6 and b <= 9 and (c, d) >= (79, 7218) and (c, d) <= (83, 0)
order by a NULLS first, b NULLS first, c NULLS first, d NULLS first;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where a = 6 and b <= 9 and (c, d) >= (79, 7218) and (c, d) <= (83, 0)
order by a NULLS first, b NULLS first, c NULLS first, d NULLS first;

-- 2025-06-21 12:30
--
-- "However, on a backwards scan, we must keep going, because we may have
-- initially positioned to the end of the index". -- verifies that we didn't
-- get this point wrong in _bt_check_rowcompare (just have this test for
-- paranoia's sake)
select *
from fuzz_skip_scan
where a = 8 and (c, d) >= (53, 8221) and c < 86 and (c, d) <= (54, null)
order by a desc, b desc, c desc, d desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from fuzz_skip_scan
where a = 8 and (c, d) >= (53, 8221) and c < 86 and (c, d) <= (54, null)
order by a desc, b desc, c desc, d desc;

-- Reset
set enable_bitmapscan to on;
set enable_indexonlyscan to on;

--
-- Heikki CREATE INDEX regression stress test query (miniaturized) --
--
-- Test case taken from: https://postgr.es/m/aa55adf3-6466-4324-92e6-5ef54e7c3918@iki.fi
--
-- set enable_seqscan=off; set max_parallel_workers_per_gather=0;

-- Setup:
set client_min_messages=error;
drop table if exists heikki_skiptest_small;
reset client_min_messages;

-- First do retail insert version of his query, where suffix truncation is
-- effective:
create unlogged table heikki_skiptest_small (a int, b int);
create index heikki_skiptest_small_idx on heikki_skiptest_small (a, b);

insert into heikki_skiptest_small
select g / 10 as a, g % 10 as b
from generate_series(1, 10_000) g;
vacuum freeze heikki_skiptest_small;

-- Forwards:
select count(*)
from heikki_skiptest_small
where b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*)
from heikki_skiptest_small
where b = 1;

-- Backwards:
select a, b
from heikki_skiptest_small
where b = 1
order by a desc, b desc
limit 1 offset 20_000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b
from heikki_skiptest_small
where b = 1
order by a desc, b desc
limit 1 offset 20_000;

-- Okay, now the actual adversarial case, which requires that suffix
-- truncation wasn't very effective -- REINDEX to get that:
reindex index heikki_skiptest_small_idx;

-- Now repeat exactly the same queries as first time around:

-- Forwards:
select count(*)
from heikki_skiptest_small
where b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*)
from heikki_skiptest_small
where b = 1;

-- Backwards:
select a, b
from heikki_skiptest_small
where b = 1
order by a desc, b desc
limit 1 offset 20_000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b
from heikki_skiptest_small
where b = 1
order by a desc, b desc
limit 1 offset 20_000;

----------------------
-- Misc multi tests --
----------------------
set client_min_messages=error;
drop table if exists multi_test_skip;
reset client_min_messages;

create unlogged table multi_test_skip(
  a int,
  b int,
  c int
);

create index multi_test_skip_idx on multi_test_skip(a, b);

insert into multi_test_skip
select
  j,
  case when i < 14 then
    0
  else
    1
  end
from
  generate_series(1, 14) i,
  generate_series(1, 400) j
order by
  j,
  i;
vacuum analyze multi_test_skip;

set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Harder case
select a, b from multi_test_skip where a in (123, 182, 183) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b from multi_test_skip where a in (123, 182, 183) and b in (1,2);

-- Hard case
select a, b from multi_test_skip where a in (182, 183, 184) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b from multi_test_skip where a in (182, 183, 184) and b in (1,2);

select a, b from multi_test_skip where a in (3,4,5) and b > 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b from multi_test_skip where a in (3,4,5) and b > 0;

set enable_indexscan to on;

-- Backwards scan:
select a, b from multi_test_skip where a in (3,4,5) and b > 0
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b from multi_test_skip where a in (3,4,5) and b > 0
order by a desc, b desc;

set enable_indexscan to off;

-- (July 24, day before taking the train to Cambridge)
--
-- Make sure that skip array stores no more than a single inequality in a
-- cross-type scenario where we cannot prove which type/operator (the > operator
-- or the >= operator) is the redundant of the two lower bound operators.
--
-- The bug here was that we senselessly clobbered the skip array's
-- array.lower_bound field.  We must avoid behaving as if we can roll more
-- than a single lower/upper bound inequality into a skip array.
select
  a, b from
  multi_test_skip
where
  a > 395 and a >= 390::int8 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  a, b from
  multi_test_skip
where
  a > 395 and a >= 390::int8 and b = 1;

-- (July 24, day before taking the train to Cambridge)
--
-- This test shouldn't get confused about which is the most restrictive scan
-- key -- it's important that we not start the scan on a leaf page to the left
-- of the tree needlessly:
--
-- (This behvior might be acceptable with an incomplete opfamily, but that's
-- not why we have here)
select
  a, b from
  multi_test_skip
where
  a > 1 and a >= 400::int8 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 2 buffer hits, not 8
select
  a, b from
  multi_test_skip
where
  a > 1 and a >= 400::int8 and b = 1;

-- Variant 1:
select
  a, b from
  multi_test_skip
where
  a > 399 and a >= 400::int8 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  a, b from
  multi_test_skip
where
  a > 399 and a >= 400::int8 and b = 1;

-- Variant 2:
select
  a, b from
  multi_test_skip
where
  a > 399::int8 and a >= 400 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  a, b from
  multi_test_skip
where
  a > 399::int8 and a >= 400 and b = 1;

-- Variant 3:
select
  a, b from
  multi_test_skip
where
  a >= 400::int8 and a > 399 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  a, b from
  multi_test_skip
where
  a >= 400::int8 and a > 399 and b = 1;

-- Variant 4:
select
  a, b from
  multi_test_skip
where
  a >= 400 and a > 399::int8 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  a, b from
  multi_test_skip
where
  a >= 400 and a > 399::int8 and b = 1;

-- Variant 5:
select
  a, b from
  multi_test_skip
where
  a > 399 and a >= 400::int8 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  a, b from
  multi_test_skip
where
  a > 399 and a >= 400::int8 and b = 1;

-- Variant 6:
select
  a, b from
  multi_test_skip
where
  a > 399 and a >= 399::int8 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  a, b from
  multi_test_skip
where
  a > 399 and a >= 399::int8 and b = 1;

-- Variant 7:
select
  a, b from
  multi_test_skip
where
  a >= 399 and a > 399::int8 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  a, b from
  multi_test_skip
where
  a >= 399 and a > 399::int8 and b = 1;

-- Variant 8:
select
  a, b from
  multi_test_skip
where
  a <= 3 and a < 3::int8 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  a, b from
  multi_test_skip
where
  a <= 3 and a < 3::int8 and b = 1;

-- Variant 9:
select
  a, b from
  multi_test_skip
where
  a <= 2 and a < 3::int8 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  a, b from
  multi_test_skip
where
  a <= 2 and a < 3::int8 and b = 1;

-- Variant 10:
select
  a, b from
  multi_test_skip
where
  a <= 2::int8 and a < 3 and b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  a, b from
  multi_test_skip
where
  a <= 2::int8 and a < 3 and b = 1;

-- Redundant test:
select a, b
from multi_test_skip
where
  a in (1, 99, 182, 183, 184)
  and a > 183;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b
from multi_test_skip
where
  a in (1, 99, 182, 183, 184)
  and a > 183;
-- Redundant test, flip order:
select a, b
from multi_test_skip
where
  a > 183
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select a, b
from multi_test_skip
where
  a > 183
  and a in (1, 99, 182, 183, 184);

select a, b
from multi_test_skip
where
  a in (180, 345)
  and a in (230, 300);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select a, b
from multi_test_skip
where
  a in (180, 345)
  and a in (230, 300);

insert into multi_test_skip
select
  NULL,
  j
from
  generate_series(1, 10) j
order by j;
vacuum analyze multi_test_skip;

select a, b
from multi_test_skip
where b = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b
from multi_test_skip
where b = 1;

-- Reset
set enable_indexonlyscan to on;
set enable_indexscan to on;

-- (June 6)
-- Backwards scan, visibly breaks when I was refactoring code in
-- _bt_advance_array_keys so that it dealt with out-of-bounds skip array case more
-- like conventional array case (i.e. by using result/beyond_end_advance
-- variables directly, not just taking instructions from _bt_binsrch_array_skey
-- skip array logic):
select a, b from multi_test_skip where a in (3,4,5) and b < 1
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF)
select a, b from multi_test_skip where a in (3,4,5) and b < 1
order by a desc, b desc;

drop index multi_test_skip_idx;

-- Test that roll over logic doesn't increment sk_datum plus remove NULL sk
-- marking when it should just do the latter.  Bug found in NULLS FIRST case
-- after returning from pgConf.dev.
--
-- (June 3)
create index multi_test_skip_idx_nulls_first on multi_test_skip(a nulls first, b);

-- Insert INT_MIN value that had better not be overlooked here:
insert into multi_test_skip
select -2147483648, 1;
vacuum analyze multi_test_skip;

select a, b
from multi_test_skip
where b = 1
order by a nulls first, b limit 5;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b
from multi_test_skip
where b = 1
order by a nulls first, b limit 5;

drop index multi_test_skip_idx_nulls_first;

-- (June 9)
-- DESC NULLS LAST
create index multi_test_skip_idx_desc_nulls_last on multi_test_skip(a desc nulls last, b);

select a, b
from multi_test_skip
where b = 1
order by a desc nulls last, b limit 5;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b
from multi_test_skip
where b = 1
order by a desc nulls last, b limit 5;

select a, b
from multi_test_skip
where b = 1
order by a desc nulls last, b limit 5 offset 397;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b
from multi_test_skip
where b = 1
order by a desc nulls last, b limit 5 offset 397;

drop index multi_test_skip_idx_desc_nulls_last;

create index multi_test_skip_desc_idx on multi_test_skip(a desc, b desc);

-- Backwards scan:
select a, b from multi_test_skip where a in (3,4,5) and b > 0
order by a, b;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b from multi_test_skip where a in (3,4,5) and b > 0
order by a, b;

-- Forwards scan:
select a, b from multi_test_skip where a in (3,4,5) and b > 0
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b from multi_test_skip where a in (3,4,5) and b > 0
order by a desc, b desc;

-- Backwards scan:
select a, b from multi_test_skip where a in (3,4,5) and b < 1
order by a, b;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b from multi_test_skip where a in (3,4,5) and b < 1
order by a, b;

-- Forwards scan:
select a, b from multi_test_skip where a in (3,4,5) and b < 1
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b from multi_test_skip where a in (3,4,5) and b < 1
order by a desc, b desc;

-- (August 13)
--
-- Add test coverage for this code path:
--
-- @@ -1924,7 +1927,10 @@ _bt_binsrch_skiparray_skey(FmgrInfo *orderproc,
--          if (array->null_elem)
--              *set_elem_result = 0;   /* NULL "=" NULL */
--          else if (cur->sk_flags & SK_BT_NULLS_FIRST)
-- +        {
-- +            elog(WARNING, "fff");   <-- fff indicates covered path
--              *set_elem_result = -1;  /* NULL "<" NOT_NULL */
-- +        }
--          else
--              *set_elem_result = 1;   /* NULL ">" NOT_NULL */
create index multi_test_skip_idx_nulls_first_nulls_first
on
multi_test_skip(a asc nulls first, b asc nulls first, c);

insert into multi_test_skip
select
  3,
  case when i < 14 then
    NULL
  else
    1
  end
from
  generate_series(1, 14) i,
  generate_series(1, 400) j
order by
  j,
  i;
vacuum analyze multi_test_skip;

select a, b, c
from multi_test_skip
where b < 1 and c is not null
order by a desc nulls last, b desc nulls last;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF)
select a, b, c
from multi_test_skip
where b < 1 and c is not null
order by a desc nulls last, b desc nulls last;

drop index multi_test_skip_idx_nulls_first_nulls_first;

----------------
-- UUID tests --
----------------

-- (June 5)
--
-- UUID is notable for being a pass-by-reference type that can use the
-- increment/decrement stuff in roughly the same way as types like integer and
-- date.  It's a good way of making things like memory management and copying
-- of datums work, because I can do that stuff without first figuring out how
-- to do next value probes that are expected to be required by more popular
-- pass-by-reference types such as text.
--
-- Another notable thing about UUID is that it's unlikely that rolling over and
-- incrementing the existing UUID value will result in a UUID value that
-- actually finds a match in the index -- that makes it a lot closer to types
-- like text than to discrete types like integer.  With types like integer,
-- we somewhat expect that incrementing/decrementing the current value will
-- actually result in a value that finds matches in the index.
set client_min_messages=error;
DROP TABLE if exists uuid_tests;
reset client_min_messages;

create unlogged table uuid_tests
(
  skippy uuid,
  predval int4
);
create index uuid_tests_idx on uuid_tests(skippy, predval);

insert into uuid_tests
select '00000000-0000-0000-0000-000000000001', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select '10000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select '20000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select '30000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select '40000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select '50000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select '60000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select '70000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select '80000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select '90000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select 'A0000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select 'B0000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select 'C0000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select 'D0000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select 'E0000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select 'F0000000-0000-0000-0000-000000000000', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFE', j from
  generate_series(1, 5000) j;
insert into uuid_tests
select NULL, j from
  generate_series(1, 5000) j;
vacuum analyze uuid_tests;

-- Basic skip scan test case for UUID:
select skippy, predval
from uuid_tests
where predval = 777 order by skippy, predval;
-- The number of descents of the index significantly exceeds the number of
-- distinct "skippy" values, since we effectively probe for the next UUID
-- value by incrementing here.  Even though explicit probes aren't really used,
-- it more or less looks like they're used in practice.
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select skippy, predval
from uuid_tests
where predval = 777 order by skippy, predval;

-- Basic skip scan test case for UUID, backwards scan:
select skippy, predval
from uuid_tests
where predval = 777 order by skippy desc, predval desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select skippy, predval
from uuid_tests
where predval = 777 order by skippy desc, predval desc;

-- (July 16) Same again ("Basic skip scan test case for UUID, backwards
-- scan"), but no skip support this time around.

-- This caught a silly bug, fixed like so:
-- diff --git a/src/backend/access/nbtree/nbtutils.c b/src/backend/access/nbtree/nbtutils.c
-- index 4b7b55a9f..d75b3e54a 100644
-- --- a/src/backend/access/nbtree/nbtutils.c
-- +++ b/src/backend/access/nbtree/nbtutils.c
-- @@ -2475,7 +2475,7 @@ _bt_scankey_skip_increment(Relation rel, ScanDirection dir,
--               * This saves a useless primitive index scan that would otherwise
--               * try to locate a value before NULL.
--               */
-- -            if (sk_isnull && !(skey->sk_flags & SK_BT_NULLS_FIRST))
-- +            if (sk_isnull && (skey->sk_flags & SK_BT_NULLS_FIRST))
--                  goto rollover;
--
-- Test:
set skipscan_skipsupport_enabled=false;
select skippy, predval
from uuid_tests
where predval = 777 order by skippy desc, predval desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select skippy, predval
from uuid_tests
where predval = 777 order by skippy desc, predval desc;
reset skipscan_skipsupport_enabled;

-- SAOP skip scan test case for UUID:
select count(*)
from uuid_tests
where predval in (333, 4000, 4500, 5000);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*)
from uuid_tests
where predval in (333, 4000, 4500, 5000);

-- Equivalent-ish range scan formulation (expected to do same accesses, and
-- give same answer):
select count(*)
from uuid_tests
where skippy between '00000000-0000-0000-0000-000000000000' and 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF'
and
predval in (333, 4000, 4500, 5000);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- COSTS OFF added to get stable test output
select count(*)
from uuid_tests
where skippy between '00000000-0000-0000-0000-000000000000' and 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF'
and
predval in (333, 4000, 4500, 5000);

-- Equivalent-ish range scan formulation using > and < operators (expected to do same accesses, and
-- give same answer) -- stresses preprocessing with pass-by-reference types:
--
-- (UPDATE July 22) This arguably regressed a bit when we went from setting
-- low_elem and high_elem during preprocessing to always directly using the
-- inequalities to fix cross-type range bugs.  One extra primitive index scan.
select count(*)
from uuid_tests
where skippy > '00000000-0000-0000-0000-000000000000' and skippy < 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF'
and
predval in (333, 4000, 4500, 5000);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- COSTS OFF added to get stable test output
select count(*)
from uuid_tests
where skippy > '00000000-0000-0000-0000-000000000000' and skippy < 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF'
and
predval in (333, 4000, 4500, 5000);

-- For good luck, more inequality stuff designed to stress preprocessing code
-- (note that these are boundary cases):
--
-- (UPDATE July 22) This arguably regressed a bit when we went from setting
-- low_elem and high_elem during preprocessing to always directly using the
-- inequalities to fix cross-type range bugs.  One extra primitive index scan.
prepare uuid_good_luck as
select count(*)
from uuid_tests
where skippy > '0FFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF' and skippy < 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFE'
and
predval in (333, 4000, 4500, 5000);

execute uuid_good_luck;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- COSTS OFF added to get stable test output
execute uuid_good_luck;
deallocate uuid_good_luck;

-----------------------------
-- create_index NULL tests --
-----------------------------

set client_min_messages=error;
DROP TABLE if exists onek_skipscan;
DROP TABLE if exists onek_with_null;
reset client_min_messages;

CREATE unlogged TABLE onek_skipscan (
	unique1		int4,
	unique2		int4,
	two			int4,
	four		int4,
	ten			int4,
	twenty		int4,
	hundred		int4,
	thousand	int4,
	twothousand	int4,
	fivethous	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name
);

\getenv abs_srcdir PG_ABS_SRCDIR
\set filename :abs_srcdir '/data/onek.data'
COPY onek_skipscan FROM :'filename';
VACUUM ANALYZE onek_skipscan;

SET enable_indexscan = ON;
SET enable_bitmapscan = ON;

-- First with skip support
set skipscan_skipsupport_enabled=true;
CREATE unlogged TABLE onek_with_null AS SELECT unique1, unique2 FROM onek_skipscan ;
INSERT INTO onek_with_null (unique1,unique2) VALUES (NULL, -1), (NULL, NULL);
CREATE UNIQUE INDEX onek_with_null_unique2_unique1 ON onek_with_null (unique2,unique1);

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
 /* 1 */ SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
DROP INDEX onek_with_null_unique2_unique1;
CREATE UNIQUE INDEX onek_with_null_unique2desc_unique1 ON onek_with_null (unique2 desc,unique1);
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
 /* 2 */ SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IN (-1, 0, 1);
DROP INDEX onek_with_null_unique2desc_unique1;
CREATE UNIQUE INDEX onek_with_null_unique2descnullslast_unique1 ON onek_with_null (unique2 desc nulls last,unique1);
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
 /* 3 */ SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
DROP INDEX onek_with_null_unique2descnullslast_unique1;
CREATE UNIQUE INDEX onek_with_null_unique2ascnullsfirst_unique1 ON onek_with_null (unique2  nulls first,unique1);
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
 /* 4 */ SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
DROP INDEX onek_with_null_unique2ascnullsfirst_unique1;
-- Check initial-positioning logic too
CREATE UNIQUE INDEX onek_with_null_unique2 ON onek_with_null (unique2);
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;
SELECT unique1, unique2 FROM onek_with_null
  ORDER BY unique2 LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 >= 0
  ORDER BY unique2 LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null
  ORDER BY unique2 DESC LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 >= -1
  ORDER BY unique2 DESC LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 < 999
  ORDER BY unique2 DESC LIMIT 2;

-- Now without skip support
DROP TABLE onek_with_null;
set skipscan_skipsupport_enabled=false;
CREATE unlogged TABLE onek_with_null AS SELECT unique1, unique2 FROM onek_skipscan ;
INSERT INTO onek_with_null (unique1,unique2) VALUES (NULL, -1), (NULL, NULL);
CREATE UNIQUE INDEX onek_with_null_unique2_unique1 ON onek_with_null (unique2,unique1);

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
 /* 5 */ SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
DROP INDEX onek_with_null_unique2_unique1;
CREATE UNIQUE INDEX onek_with_null_unique2desc_unique1 ON onek_with_null (unique2 desc,unique1);
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
 /* 6 */ SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IN (-1, 0, 1);
DROP INDEX onek_with_null_unique2desc_unique1;
CREATE UNIQUE INDEX onek_with_null_unique2descnullslast_unique1 ON onek_with_null (unique2 desc nulls last,unique1);
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
 /* 7 */ SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
DROP INDEX onek_with_null_unique2descnullslast_unique1;
CREATE UNIQUE INDEX onek_with_null_unique2ascnullsfirst_unique1 ON onek_with_null (unique2  nulls first,unique1);
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
 /* 8 */ SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;
DROP INDEX onek_with_null_unique2ascnullsfirst_unique1;
-- Check initial-positioning logic too
CREATE UNIQUE INDEX onek_with_null_unique2 ON onek_with_null (unique2);
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;
SELECT unique1, unique2 FROM onek_with_null
  ORDER BY unique2 LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 >= 0
  ORDER BY unique2 LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null
  ORDER BY unique2 DESC LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 >= -1
  ORDER BY unique2 DESC LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 < 999
  ORDER BY unique2 DESC LIMIT 2;
reset skipscan_skipsupport_enabled;

RESET enable_indexscan;
RESET enable_bitmapscan;

--------------------------------
-- MDAM paper small test case --
--------------------------------
set client_min_messages=error;
drop table if exists sales_mdam_paper_small;
reset client_min_messages;

create unlogged table sales_mdam_paper_small
(
  dept int4,
  sdate date,
  item_class serial,
  store int4,
  item int4,
  total_sales numeric
);
create index mdam_small_idx on sales_mdam_paper_small(dept, sdate, item_class, store);
vacuum analyze sales_mdam_paper_small;

-- Load data
insert into sales_mdam_paper_small (dept, sdate, item_class, store, total_sales)
select
  dept,
  '1995-01-01'::date + sdate,
  item_class,
  store,
  (random() * 500.0) as total_sales
from
  generate_series(1, 1) dept,
  generate_series(1, 4) sdate,
  generate_series(1, 8) item_class,
  generate_series(1, 3) store;

-- Mixes range arrays with conventional SAOPs, leading to confusion about
-- boundary conditions:
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate between '1995-01-04' and '1995-01-05'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate between '1995-01-04' and '1995-01-05'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;

-- Non-skip-support, repeat "Mixes range arrays with conventional SAOPs,
-- leading to confusion about boundary conditions":
set skipscan_skipsupport_enabled=false;
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate between '1995-01-04' and '1995-01-05'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate between '1995-01-04' and '1995-01-05'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;
reset skipscan_skipsupport_enabled;

-- Same again, but this time we use different operators/constants to get the
-- same effective date range as original BETWEEN version:
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate > '1995-01-03' and sdate <= '1995-01-05'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate > '1995-01-03' and sdate <= '1995-01-05'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;

-- Ditto:
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate > '1995-01-03' and sdate < '1995-01-06'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate > '1995-01-03' and sdate < '1995-01-06'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;

-- range on date has no lower bound (or lower bound is -inf):
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate < '1995-01-04'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate < '1995-01-04'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;

-- range on date has no upper bound (or upper bound is +inf):
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate > '1995-01-04'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  sdate > '1995-01-04'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;

-- Now don't omit dept key, without changing rows returned:
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  dept between 0 and 100
  and sdate between '1995-01-04' and '1995-01-05'
  and item_class = 3
  and store = 2
order by dept, sdate, item_class, store;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  dept between 0 and 100
  and sdate between '1995-01-04' and '1995-01-05'
  and item_class = 3
  and store = 2
order by dept, sdate, item_class, store;
-- Now don't omit dept key, without changing rows returned (matches original
-- query by including conventional SAOPs to make it harder to get right):
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  dept between 0 and 100
  and sdate between '1995-01-04' and '1995-01-05'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  ctid, dept, sdate, item_class, store
from sales_mdam_paper_small
where
  dept between 0 and 100
  and sdate between '1995-01-04' and '1995-01-05'
  and item_class in (1, 3, 5)
  and store in (2, 3)
order by dept, sdate, item_class, store;

-----------------------
-- tenk1 test cases  --
-----------------------
set client_min_messages=error;
drop table if exists tenk1_skipscan;
reset client_min_messages;
CREATE UNLOGGED TABLE tenk1_skipscan (
	unique1		int4,
	unique2		int4,
	two			int4,
	four		int4,
	ten			int4,
	twenty		int4,
	hundred		int4,
	thousand	int4,
	twothousand	int4,
	fivethous	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name
);
ALTER TABLE tenk1_skipscan SET (autovacuum_enabled=off);

\getenv abs_srcdir PG_ABS_SRCDIR
\set filename :abs_srcdir '/data/tenk.data'
COPY tenk1_skipscan FROM :'filename';
VACUUM ANALYZE tenk1_skipscan;

CREATE INDEX tenk1_skipscan_four_unique1 ON tenk1_skipscan (four, unique1);

prepare tenk1_four_skipscan as
SELECT four, unique1 FROM tenk1_skipscan
 WHERE unique1 = 444;

execute tenk1_four_skipscan;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- master 30 hits
execute tenk1_four_skipscan;
deallocate tenk1_four_skipscan;

prepare tenk1_four_skipscan_with_saop as
select four, unique1 from tenk1_skipscan where unique1 in (4444, 4445) limit 3;

execute tenk1_four_skipscan_with_saop;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- master 31 hits
execute tenk1_four_skipscan_with_saop;
deallocate tenk1_four_skipscan_with_saop;

-- Challenge here is to not do significantly worse than master branch's
-- traditional full index scan, since skipping isn't going to work here:
drop index tenk1_skipscan_four_unique1;
CREATE INDEX tenk1_skipscan_hundred_unique1 ON tenk1_skipscan (hundred, unique1);
prepare tenk1_fallback_to_regular_fullscan as
SELECT hundred, unique1 FROM tenk1_skipscan
 WHERE unique1 = 444;

execute tenk1_fallback_to_regular_fullscan;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- master 30 hits
execute tenk1_fallback_to_regular_fullscan;
deallocate tenk1_fallback_to_regular_fullscan;

drop index tenk1_skipscan_hundred_unique1;
CREATE INDEX tenk1_skipscan_two_four_twenty ON tenk1_skipscan (two, four, twenty);

-- This test case caught sloppiness in adding new "input" skip scan keys for
-- index attributes that already had = strategy scan keys:
set enable_hashagg=off; -- XXX 2024-12-18 force sort-based agg, to get stable test output
select distinct four, twenty from tenk1_skipscan
where four in (1, 2) and twenty in (1, 2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select distinct four, twenty from tenk1_skipscan
where four in (1, 2) and twenty in (1, 2);
set enable_indexonlyscan=on;

-- Redundant attributes test related to bug where equality input keys
-- spuriously get their own skip input key:
select distinct two, four, twenty, hundred
from tenk1_skipscan
where
  four in (0, 1)
  and four in (1, 2)
  and twenty = 1
order by two, four, twenty;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select distinct two, four, twenty, hundred
from tenk1_skipscan
where
  four in (0, 1)
  and four in (1, 2)
  and twenty = 1
order by two, four, twenty;

drop index tenk1_skipscan_two_four_twenty;
create index on tenk1_skipscan (two, four, twenty, hundred);

prepare tenk1_two_four_twenty_hundred_inequal as
select count(*), two, four, twenty, hundred
from tenk1_skipscan
where
  four in (1, 2, 3)
  and four = 1
  and twenty in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17)
  and hundred < 50
group by two, four, twenty, hundred
order by two, four, twenty, hundred;
execute tenk1_two_four_twenty_hundred_inequal;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute tenk1_two_four_twenty_hundred_inequal;
deallocate tenk1_two_four_twenty_hundred_inequal;

-------------------------------------------------------------------------------
-- upper range tenk1 test case based on aggregates.out from regression tests --
-------------------------------------------------------------------------------
create index on tenk1_skipscan (unique1, unique2);

-- Closest match for regression test:
select unique1
from tenk1_skipscan
where unique1 > 2147483647
limit 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select unique1
from tenk1_skipscan
where unique1 > 2147483647
limit 3;

-- Same again, this time force a skip array on unique1, so that now we can
-- try to increment > to >=, but get overflow (this will make the scan qual
-- unsatisfiable, unlike the prior no-skip-array example):
--
-- XXX (November 15 2024) This used to work like this, then I stopped making
-- overflow make the qual unsatisfiable in order to make cross-type "> to >="
-- transformations work, and finally just today I decided that cross-type
-- transformations weren't going to happen -- and so we might as well have
-- this small thing in return (once you assume that cross-type support isn't
-- happening, which now seems inevitable, then it also seems easy to do this
-- marginal optimization too).
select unique1
from tenk1_skipscan
where unique1 > 2147483647
and unique2 = 101010 -- Forces unique1 to have skip array, so it gets skip array preprocessing
limit 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- no buffer hits this time
select unique1
from tenk1_skipscan
where unique1 > 2147483647
and unique2 = 101010 -- Forces unique1 to have skip array, so it gets skip array preprocessing
limit 3;

-- (November 15 2024)
-- Same again, this time decrement < to <= instead (this will make the scan qual
-- unsatisfiable, too):
select unique1
from tenk1_skipscan
where unique1 < (-2147483648)::int4
and unique2 = 101010
limit 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- no buffer hits this time
select unique1
from tenk1_skipscan
where unique1 < (-2147483648)::int4
and unique2 = 101010
limit 3;

-- Variant:
select unique1
from tenk1_skipscan
where unique1 >= 2147483647
and unique2 = 101010 -- Forces unique1 to have skip array, so it gets skip array preprocessing
limit 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select unique1
from tenk1_skipscan
where unique1 >= 2147483647
and unique2 = 101010 -- Forces unique1 to have skip array, so it gets skip array preprocessing
limit 3;

-- Variant:
select unique1
from tenk1_skipscan
where unique1 > 2147483648
limit 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select unique1
from tenk1_skipscan
where unique1 > 2147483648
limit 3;

-- Same again, this time force a skip array on unique1, so that now we can
-- try and fail to increment > to >=:
select unique1
from tenk1_skipscan
where unique1 > 2147483648
and unique2 = 101010 -- Forces unique1 to have skip array, so it gets skip array preprocessing
limit 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select unique1
from tenk1_skipscan
where unique1 > 2147483648
and unique2 = 101010 -- Forces unique1 to have skip array, so it gets skip array preprocessing
limit 3;

-- Variant:
select unique1
from tenk1_skipscan
where unique1 < (-2147483649)
limit 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select unique1
from tenk1_skipscan
where unique1 < (-2147483649)
limit 3;

-- Same again, this time force a skip array on unique1, so that now we can
-- try and fail to increment < to <=:
select unique1
from tenk1_skipscan
where unique1 < (-2147483649)
and unique2 = 101010 -- Forces unique1 to have skip array, so it gets skip array preprocessing
limit 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- no buffer hits this time
select unique1
from tenk1_skipscan
where unique1 < (-2147483649)
and unique2 = 101010 -- Forces unique1 to have skip array, so it gets skip array preprocessing
limit 3;

-- SAOP project regression test that once failed 32-bit platforms including CI:
prepare floatbyref_preproc as
select
  unique1
from
  tenk1_skipscan
where
  unique1 < 3
  and unique1 <(-1)::bigint;

-- Note: This doesn't fail reliably when run on horse server (with "#define
-- USE_FLOAT8_BYVAL" commented out), though using ../coredump-run.sh seems to
-- help it to fail
execute floatbyref_preproc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute floatbyref_preproc;
deallocate floatbyref_preproc;

--------------------------------------------------------------------------------------------------
-- (November 15) Cross-type opclasses from datetime_ops opfamily "> to >=" transformation tests --
--------------------------------------------------------------------------------------------------

set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

set client_min_messages=error;
drop table if exists timestamp_date_crosstype_test;
reset client_min_messages;

create unlogged table timestamp_date_crosstype_test(
  t timestamp,
  i int4
);

create index timestamp_date_crosstype_test_idx on timestamp_date_crosstype_test (t, i);

insert into timestamp_date_crosstype_test
select ('1995-01-01'::date + i)::timestamp + interval '1 second', i
from generate_series(0, 100) i;
vacuum analyze timestamp_date_crosstype_test;

-- This had better not increment "t > 1995-01-01" into "t >= 1995-01-02",
-- given that the underlying column is actually a timestamp, not a date:
prepare crosstype_test_qry as
select *
from timestamp_date_crosstype_test
where
  t > '1995-01-01'::date and i = 0;

execute crosstype_test_qry;

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute crosstype_test_qry;

deallocate crosstype_test_qry;

-- Same again, but timestamptz this time (just for good luck):
set client_min_messages=error;
drop table if exists timestamptz_date_crosstype_test;
reset client_min_messages;
create unlogged table timestamptz_date_crosstype_test(
  t timestamptz,
  i int4
);

create index timestamptz_date_crosstype_test_idx on timestamptz_date_crosstype_test (t, i);

insert into timestamptz_date_crosstype_test
select ('1995-01-01'::date + i)::timestamptz + interval '1 second', i
from generate_series(0, 100) i;
vacuum analyze timestamptz_date_crosstype_test;

prepare crosstype_test_qry as
select *
from timestamptz_date_crosstype_test
where
  t > '1995-01-01'::date and i = 0;

execute crosstype_test_qry;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute crosstype_test_qry;

deallocate crosstype_test_qry;

--------------------------------------------------------------------------
-- (July 9) Terminate scan promptly when name column lacks skip support --
--------------------------------------------------------------------------

create index name_no_skip_support on tenk1_skipscan (string4, tenthous);

set enable_bitmapscan to on;
set enable_indexonlyscan to on;
set enable_indexscan to on;

-- This query shouldn't have to access more than one leaf page (the leftmost),
-- since string4 < 'AAAAxx' condition matches tuples that are before any
-- actual extant tuples from the index:
prepare just_scan_leftmost_page as
select string4, tenthous
from tenk1_skipscan
where string4 < 'AAAAxx' and tenthous = 21;

execute just_scan_leftmost_page;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- Expect only 2 buffer hits
execute just_scan_leftmost_page;
deallocate just_scan_leftmost_page;

-- Forwards scan
prepare terminate_name_ontime_inequality as
SELECT string4, tenthous FROM tenk1_skipscan
 WHERE string4 < 'OOOOxx' and tenthous in (131, 997, 5997, 992);

-- When first working on range skip scans + no-skip-support, had this one
-- remaining failure to terminate the scan when it reached the leaf page where
-- we naturally expect it to end.  The usual familiar assertion failure (with
-- a lack of any wrong answers to queries) proved as much:

-- TRAP: failed Assert("!_bt_tuple_before_array_skeys(scan, dir, tuple, tupdesc, tupnatts, false, 0, NULL)"), File: "../source/src/backend/access/nbtree/nbtutils.c", Line: 3063, PID: 497118
-- [0x55627e45e524] _bt_advance_array_keys: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtutils.c:3062
-- [0x55627e45d20a] _bt_checkkeys: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtutils.c:5070
-- [0x55627e450d7f] _bt_readpage: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtsearch.c:2288
-- [0x55627e4500fb] _bt_first: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtsearch.c:1955
-- [0x55627e449e74] btgettuple: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtree.c:259
-- [0x55627e436625] index_getnext_tid: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/index/indexam.c:590
execute terminate_name_ontime_inequality;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute terminate_name_ontime_inequality;
deallocate terminate_name_ontime_inequality;

-- Backwards scan
prepare terminate_name_ontime_inequality_backwards as
SELECT string4, tenthous FROM tenk1_skipscan
 WHERE string4 < 'OOOOxx' and tenthous in (131, 997, 5997, 992) order by string4 desc, tenthous desc;

execute terminate_name_ontime_inequality_backwards;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute terminate_name_ontime_inequality_backwards;
deallocate terminate_name_ontime_inequality_backwards;

drop index name_no_skip_support;

-- (July 12) This is like the create_view.sql failure in the regression tests
-- (seen once skipping was enabled on catalog indexes)
--
-- This fails due to a simple lack of cross-type support in places where we
-- need to use non-input-opclass-type-typed inequality sk_argument values.
create index stringu1_tenthous_idx on tenk1_skipscan (stringu1, tenthous);

-- Simplest version:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  -- Original: stringu1 like 'A_%' and tenthous in (5174, 7384, 9438)
  stringu1 >= 'A'::text and stringu1 < 'B'::text and tenthous in (5174, 7384, 9438)
order by stringu1, tenthous;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Same, but this time as a backwards scan:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  -- Original: stringu1 like 'A_%' and tenthous in (5174, 7384, 9438)
  stringu1 >= 'A'::text and stringu1 < 'B'::text and tenthous in (5174, 7384, 9438)
order by stringu1 desc, tenthous desc;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Other end of index this time:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  -- Original: stringu1 like 'Z_%' and tenthous in (25, 8787, 571)
  stringu1 >= 'Z'::text and stringu1 < '['::text and tenthous in (25, 8787, 571)
order by stringu1, tenthous;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Same, but this time as a backwards scan:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  -- Original: stringu1 like 'Z_%' and tenthous in (25, 8787, 571)
  stringu1 >= 'Z'::text and stringu1 < '['::text and tenthous in (25, 8787, 571)
order by stringu1 desc, tenthous desc;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- (July 16) Variants with mixed inequalities:

-- > and < variants:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > '@'::text and stringu1 < 'B'::text and tenthous in (5174, 7384, 9438)
order by stringu1, tenthous;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Same, but this time as a backwards scan:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > '@'::text and stringu1 < 'B'::text and tenthous in (5174, 7384, 9438)
order by stringu1 desc, tenthous desc;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Other end of index this time:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > 'Y'::text and stringu1 < '['::text and tenthous in (25, 8787, 571)
order by stringu1, tenthous;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Same, but this time as a backwards scan:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > 'Y'::text and stringu1 < '['::text and tenthous in (25, 8787, 571)
order by stringu1 desc, tenthous desc;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- > and <= variants:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > '@'::text and stringu1 <= 'AZZZZZZ'::text and tenthous in (5174, 7384, 9438)
order by stringu1, tenthous;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Same, but this time as a backwards scan:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > '@'::text and stringu1 <= 'AZZZZZZ'::text and tenthous in (5174, 7384, 9438)
order by stringu1 desc, tenthous desc;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Other end of index this time:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > 'Y'::text and stringu1 <= 'Z{'::text and tenthous in (25, 8787, 571)
order by stringu1, tenthous;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Same, but this time as a backwards scan:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > 'Y'::text and stringu1 <= 'Z{'::text and tenthous in (25, 8787, 571)
order by stringu1 desc, tenthous desc;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- (July 16) This time with DESC index cols, to make sure that we have
-- backwards scan coverage (using non-cross-type comparator accidentally fails
-- to fail without this extra dimension):
-- instead:
drop index stringu1_tenthous_idx;
create index stringu1_tenthous_desc_idx on tenk1_skipscan (stringu1 desc, tenthous desc);

-- > and <= variants:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > '@'::text and stringu1 <= 'AZZZZZZ'::text and tenthous in (5174, 7384, 9438)
order by stringu1, tenthous;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Same, but this time as a backwards scan:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > '@'::text and stringu1 <= 'AZZZZZZ'::text and tenthous in (5174, 7384, 9438)
order by stringu1 desc, tenthous desc;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Other end of index this time:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > 'Y'::text and stringu1 <= 'Z{'::text and tenthous in (25, 8787, 571)
order by stringu1, tenthous;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

-- Same, but this time as a backwards scan:
prepare like_test as
select stringu1, tenthous from tenk1_skipscan
where
  stringu1 > 'Y'::text and stringu1 <= 'Z{'::text and tenthous in (25, 8787, 571)
order by stringu1 desc, tenthous desc;
execute like_test;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute like_test;
deallocate like_test;

---------------------------
-- Wisconsin table tests --
---------------------------

set client_min_messages=error;
drop table if exists wisconsin;
reset client_min_messages;

create unlogged table wisconsin
(
unique1 int4,
unique2 int4,
two int4,
four int4,
ten int4,
twenty int4,
onepercent int4,
tenpercent int4,
twentypercent int4,
fiftypercent int4,
unique3 int4,
evenonepercent int4,
oddonepercent int4,
stringu1 text,
stringu2 text,
string4 text
);

\set filename :abs_srcdir '/data/wisconsin.csv'
COPY wisconsin FROM :'filename' with (format csv, encoding 'win1252', header false, null $$$$, quote $$'$$); -- Fix the syntax highlighting: '

insert into wisconsin(unique1, unique2, two, four, ten, twenty, onepercent)
select
  5555,
  5555,
  2147483646,
  2147483646,
  2147483646,
  2147483646,
  2147483646;
insert into wisconsin(unique1, unique2, two, four, ten, twenty, onepercent)
select
  5555,
  5555,
  2147483647,
  2147483647,
  2147483647,
  2147483647,
  2147483647;
insert into wisconsin(unique1, unique2, two, four, ten, twenty, onepercent)
select
  5555,
  5555,
(-2147483647),
(-2147483647),
(-2147483647),
(-2147483647),
(-2147483647);
insert into wisconsin(unique1, unique2, two, four, ten, twenty, onepercent)
select
  5555,
  5555,(-2147483648),
(-2147483648),
(-2147483648),
(-2147483648),
(-2147483648);
vacuum analyze wisconsin;

set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Two:
create index two_idx on wisconsin (two, unique1);

select two, unique1 from wisconsin where unique1 = 5555;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits
select two, unique1 from wisconsin where unique1 = 5555;

drop index two_idx;

-- Four:
create index four_idx on wisconsin (four, unique1);

-- Point lookup:
select four, unique1 from wisconsin where unique1 = 5555;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits
select four, unique1 from wisconsin where unique1 = 5555;

-- SAOP:
select four, unique1 from wisconsin where unique1 in (1, 5555, 100000, 200000);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits
select four, unique1 from wisconsin where unique1 in (1, 5555, 100000, 200000);

drop index four_idx;

-- Ten:
create index ten_idx on wisconsin (ten, unique1);

-- Point lookup:
select ten, unique1 from wisconsin where unique1 = 5555;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits
select ten, unique1 from wisconsin where unique1 = 5555;

-- Range instead of skip attribute on "ten":
prepare range_instead as
select ten, unique1 from wisconsin where ten between -10000 and 4 and unique1 = 5555;

execute range_instead;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF)
execute range_instead;

-- Range instead of skip attribute on "ten", backwards scan:
set enable_bitmapscan to off;
set enable_indexscan to on;
prepare range_instead_backwards as
select ten, unique1 from wisconsin where ten between -10000 and 4 and unique1 = 5555 order by ten desc, unique1 desc;

execute range_instead_backwards;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF)
execute range_instead_backwards;

set enable_bitmapscan to on;
set enable_indexscan to off;

-- SAOP:
select ten, unique1 from wisconsin where unique1 in (1, 5555, 100000, 200000);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits
select ten, unique1 from wisconsin where unique1 in (1, 5555, 100000, 200000);

-- Contradictory qual:
-- XXX consider adding preprocessing to detect this case.
select ten, unique1 from wisconsin where unique1 between 101 and 100;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits, patch 0 hits, since patch detects >= and <= related contradictoriness
select ten, unique1 from wisconsin where unique1 between 101 and 100;

-- Contradictory qual, > and < strategies:
-- XXX consider adding preprocessing to detect this case.
select ten, unique1 from wisconsin where unique1 > 100 and unique1 < 100;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select ten, unique1 from wisconsin where unique1 > 100 and unique1 < 100;

-- Contradictory qual, > and < strategies, many operators:
-- XXX consider adding preprocessing to detect this case.
select ten, unique1 from wisconsin
where
unique1 > 1 and
unique1 < 400 and
unique1 > 90 and
unique1 > 100 and
unique1 < 100
;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select ten, unique1 from wisconsin
where
unique1 > 1 and
unique1 < 400 and
unique1 > 90 and
unique1 > 100 and
unique1 < 100
;

-- Just one constant generate by skip array (100):
select ten, unique1 from wisconsin where unique1 between 100 and 100;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select ten, unique1 from wisconsin where unique1 between 100 and 100;

-- Just one constant generate by skip array (100), > and < strategies:
select unique1 from wisconsin where unique1 > 99 and unique1 < 101;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select unique1 from wisconsin where unique1 > 99 and unique1 < 101;

-- Just one constant generate by skip array (100), > and < strategies, many operators:
select unique1 from wisconsin
where
unique1 > 1 and
unique1 < 400 and
unique1 > 90 and
unique1 > 99 and
unique1 < 101
;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select unique1 from wisconsin
where
unique1 > 1 and
unique1 < 400 and
unique1 > 90 and
unique1 > 99 and
unique1 < 101
;

drop index ten_idx;

-- Four, ten:
create index four_ten_idx on wisconsin (four, ten, unique1);

-- Point lookup, skips two cols (four and ten):
select four, ten, unique1 from wisconsin where unique1 = 5555;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 1152 hits
select four, ten, unique1 from wisconsin where unique1 = 5555;

-- Point lookup, skips one col (four), range on other col after that (ten):
select four, ten, unique1 from wisconsin where ten between -10000 and 4 and unique1 = 5555;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 1152 hits
select four, ten, unique1 from wisconsin where ten between -10000 and 4 and unique1 = 5555;

-- Should be able to handle BETWEEN ranges with same value for >= and <=:
prepare handle_between as
select four, ten, unique1 from wisconsin where four between 0 and 0 and unique1 = 5555;

execute handle_between;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF)
execute handle_between;

-- Missing predicate is in "intermediate" column (ten) here:
select four, ten, unique1 from wisconsin where four = 0 and unique1 = 5555;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- master 290 hits, patch 33 hits
select four, ten, unique1 from wisconsin where four = 0 and unique1 = 5555;

-- Missing predicate is in "intermediate" column (ten) here, plus we use a
-- SAOP for unique1 this time around:
select four, ten, unique1 from wisconsin where four = 0 and unique1 in (41, 5555, 299118, 300000);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- master 290 hits, patch 33 hits
select four, ten, unique1 from wisconsin where four = 0 and unique1 in (41, 5555, 299118, 300000);

-- SAOP:
select four, ten, unique1 from wisconsin where unique1 in (1, 5555, 100000, 200000);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- master 1152 hits
select four, ten, unique1 from wisconsin where unique1 in (1, 5555, 100000, 200000);

-- backwards scans:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Simple backwards scan (failed once, simplified from next test case):
select four, ten, unique1
from wisconsin
where unique1 = 1
order by four desc, ten desc, unique1 desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- master 1156 hits
select four, ten, unique1
from wisconsin
where unique1 = 1
order by four desc, ten desc, unique1 desc;

-- SAOP backwards scan:
select four, ten, unique1
from wisconsin
where unique1 in (1, 5555, 100000, 200000)
order by four desc, ten desc, unique1 desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- master 1156 hits
select four, ten, unique1
from wisconsin
where unique1 in (1, 5555, 100000, 200000)
order by four desc, ten desc, unique1 desc;

-- One omitted attribute (four) followed by two SAOPs
select four, ten, unique1
from wisconsin
where
  ten in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9)
  and unique1 in (41, 5555, 299118, 300000);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF)
select four, ten, unique1
from wisconsin
where
  ten in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9)
  and unique1 in (41, 5555, 299118, 300000);

drop index four_ten_idx;

-- SAOP, DESC index, forward scan (forward relative to DESC direction):
create index four_desc_ten_desc_idx on wisconsin (four desc, ten desc, unique1 desc);
select four, ten, unique1
from wisconsin
where unique1 in (1, 5555, 100000, 200000)
order by four desc, ten desc, unique1 desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- master 1156 hits
select four, ten, unique1
from wisconsin
where unique1 in (1, 5555, 100000, 200000)
order by four desc, ten desc, unique1 desc;

-- SAOP, DESC index, backward scan (backward relative to DESC direction):
select four, ten, unique1
from wisconsin
where unique1 in (1, 5555, 100000, 200000)
order by four, ten, unique1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF) -- master 1156 hits
select four, ten, unique1
from wisconsin
where unique1 in (1, 5555, 100000, 200000)
order by four, ten, unique1;

drop index four_desc_ten_desc_idx;

set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Twenty:
create index twenty_idx on wisconsin (twenty, unique1);

-- Point lookup:
select twenty, unique1 from wisconsin where unique1 = 5555;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits
select twenty, unique1 from wisconsin where unique1 = 5555;

-- SAOP:
select twenty, unique1 from wisconsin where unique1 in (1, 5555, 100000, 200000);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits
select twenty, unique1 from wisconsin where unique1 in (1, 5555, 100000, 200000);

drop index twenty_idx;

-- Hundred/onepercent:
create index onepercent_idx on wisconsin (onepercent, unique1);

-- Point lookup:
select onepercent, unique1 from wisconsin where unique1 = 5555;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits
select onepercent, unique1 from wisconsin where unique1 = 5555;

-- SAOP:
select onepercent, unique1 from wisconsin where unique1 in (1, 5555, 100000, 200000);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits
select onepercent, unique1 from wisconsin where unique1 in (1, 5555, 100000, 200000);

-- (June 14, day after going into JFK 27 to have lunch (later dinner) with
-- jkatz)
--
-- Test case showing bug in _bt_advance_skip_array_increment when we wrap
-- around a scan key whose sk_argument is already INT_MAX.  We should
-- "increment" its scan key to NULL, the true final value.  The bug that we
-- saw here involved not returning the row with a NULL oncepercent and a
-- unique1 value of -666.  We'd actually skip straight past NULL, wrapping
-- back around to INT_MIN again, which was just wrong.
insert into wisconsin(unique1, unique2, two, four, ten, twenty, onepercent)
select 1, 1, 1, 1, 1, 1, 2147483647 from generate_series(1, 1500) i;
insert into wisconsin(unique1, unique2, two, four, ten, twenty, onepercent)
select 1, 1, 1, 1, 1, 1, (-2147483648)::int4 from generate_series(1, 1500) i;

insert into wisconsin(unique1, unique2, two, four, ten, twenty, onepercent)
select -666, 1, 1, 1, 1, 1, 2147483647;
insert into wisconsin(unique1, unique2, two, four, ten, twenty, onepercent)
select -666, 1, 1, 1, 1, 1, (-2147483648)::int4;

insert into wisconsin(unique1, unique2, two, four, ten, twenty, onepercent)
select 1, 1, 1, 1, 1, 1, NULL from generate_series(1, 1500) i;
insert into wisconsin(unique1, unique2, two, four, ten, twenty, onepercent)
select -666, 1, 1, 1, 1, 1, NULL;
vacuum analyze wisconsin;

-- Force index scan
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Main test
select onepercent, unique1 from wisconsin where unique1 = -666;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select onepercent, unique1 from wisconsin where unique1 = -666;

-- Same again, backwards scan
select onepercent, unique1 from wisconsin where unique1 = -666
order by onepercent desc, unique1 desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select onepercent, unique1 from wisconsin where unique1 = -666
order by onepercent desc, unique1 desc;

drop index onepercent_idx;

-- Hundred/onepercent, nulls first, repeat last two test cases:
create index onepercent_nulls_first_idx on wisconsin (onepercent desc nulls first, unique1 desc nulls first);

-- Main test
select onepercent, unique1 from wisconsin where unique1 = -666;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select onepercent, unique1 from wisconsin where unique1 = -666;

-- Same again, backwards scan (this variant was broken briefly, when I only
-- had forward scan handling code in _bt_advance_skip_array_increment)
select onepercent, unique1 from wisconsin where unique1 = -666
order by onepercent, unique1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select onepercent, unique1 from wisconsin where unique1 = -666
order by onepercent, unique1;

drop index onepercent_nulls_first_idx;

set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Four, ten, twenty, unique1 (causes errors about attribute order from
-- _by_preprocess_keys):
create index on wisconsin (four, ten, twenty, unique1);
select four, ten, twenty, unique1
from wisconsin
where
  ten between 1 and 10
  and twenty in (1, 2, 3)
  and unique1 in (84396, 217539, 60814);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits
select four, ten, twenty, unique1
from wisconsin
where
  ten between 1 and 10
  and twenty in (1, 2, 3)
  and unique1 in (84396, 217539, 60814);

-- (January 19 2025) Contradictory scan keys should not confuse _bt_preprocess_array_keys
--
-- _bt_preprocess_array_keys shouldn't get confused about contradictory quals
-- with things like a = key and an inequality key on the same attribute.
--
-- Right now, these test cases trick _bt_preprocess_array_keys into throwing
-- errors such as:
-- ERROR:  missing oprcode for skipping equals operator 2437166984

-- Contradictory
prepare contradictory_wisconsin as
select four, ten, unique1
from wisconsin
where
  four = -1 and four between 0 and 3
  and ten between 2 and 15
  and unique1 in (1, 2490, 7777);

execute contradictory_wisconsin;

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
execute contradictory_wisconsin;

deallocate contradictory_wisconsin;

-- Contradictory
prepare contradictory_wisconsin as
select four, ten, unique1
from wisconsin
where
  four between 0 and 3 and four = -1
  and ten between 2 and 15
  and unique1 in (1, 2490, 7777);

execute contradictory_wisconsin;

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
execute contradictory_wisconsin;

deallocate contradictory_wisconsin;

-- Contradictory
prepare contradictory_wisconsin as
select four, ten, unique1
from wisconsin
where
  four = 4 and four between 0 and 3 and four = -1
  and ten between 2 and 15
  and unique1 in (1, 2490, 7777);

execute contradictory_wisconsin;

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
execute contradictory_wisconsin;

deallocate contradictory_wisconsin;

-- Partially redundant
prepare redundant_wisconsin as
select four, ten, unique1
from wisconsin
where
  four = 1 and four between 0 and 3
  and ten between 2 and 15
  and unique1 in (1, 2490, 7777);

execute redundant_wisconsin;

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
execute redundant_wisconsin;

deallocate redundant_wisconsin;

-- Partially redundant
prepare redundant_wisconsin as
select four, ten, unique1
from wisconsin
where
  four between 0 and 3 and four = 1
  and ten between 2 and 15
  and unique1 in (1, 2490, 7777);

execute redundant_wisconsin;

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
execute redundant_wisconsin;

deallocate redundant_wisconsin;

-------------------------------------------------------------------------------
-- (July 12) Day after going for drinks with jkatz + company in East Village --
-------------------------------------------------------------------------------
drop index wisconsin_four_ten_twenty_unique1_idx;
create index four_ten_unique1_idx on wisconsin (four, ten, unique1);

-- Missing predicate is in "intermediate" column (ten) here:
-- "#define FORCE_NOSKIP_DEBUG + integer wisconsin table" broke with this
-- query at one point, but it's now fine:
prepare force_noskip_debug as
select four, ten, unique1 from wisconsin where four = 0 and unique1 = 5555;

execute force_noskip_debug;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
execute force_noskip_debug;

-----------------------------------------
-- (July 10) Wisconsin numeric variant --
-----------------------------------------

set client_min_messages=error;
drop table if exists wisconsin_numeric;
reset client_min_messages;

create unlogged table wisconsin_numeric
(
unique1 numeric,
unique2 numeric,
two numeric,
four numeric,
ten numeric,
twenty numeric,
onepercent numeric,
tenpercent numeric,
twentypercent numeric,
fiftypercent numeric,
unique3 numeric,
evenonepercent numeric,
oddonepercent numeric,
stringu1 text,
stringu2 text,
string4 text
);

\set filename :abs_srcdir '/data/wisconsin.csv'
COPY wisconsin_numeric FROM :'filename' with (format csv, encoding 'win1252', header false, null $$$$, quote $$'$$); -- Fix the syntax highlighting: '

insert into wisconsin_numeric(unique1, unique2, two, four, ten, twenty, onepercent)
select
  5555,
  5555,
  2147483646,
  2147483646,
  2147483646,
  2147483646,
  2147483646;
insert into wisconsin_numeric(unique1, unique2, two, four, ten, twenty, onepercent)
select
  5555,
  5555,
  2147483647,
  2147483647,
  2147483647,
  2147483647,
  2147483647;
insert into wisconsin_numeric(unique1, unique2, two, four, ten, twenty, onepercent)
select
  5555,
  5555,
(-2147483647),
(-2147483647),
(-2147483647),
(-2147483647),
(-2147483647);
insert into wisconsin_numeric(unique1, unique2, two, four, ten, twenty, onepercent)
select
  5555,
  5555,(-2147483648),
(-2147483648),
(-2147483648),
(-2147483648),
(-2147483648);

set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;
vacuum analyze wisconsin_numeric;

-- Ten:
create index numeric_ten_idx on wisconsin_numeric (ten, unique1);

-- Range instead of skip attribute on "ten":
select ten, unique1 from wisconsin_numeric where ten between -10000 and 4 and unique1 = 5555;
EXPLAIN (ANALYZE, BUFFERS OFF, TIMING OFF, COSTS OFF, SUMMARY OFF)
select ten, unique1 from wisconsin_numeric where ten between -10000 and 4 and unique1 = 5555;

-- Range instead of skip attribute on "ten", backwards scan:
set enable_bitmapscan to off;
set enable_indexscan to on;
select ten, unique1 from wisconsin_numeric where ten between -10000 and 4 and unique1 = 5555 order by ten desc, unique1 desc;
EXPLAIN (ANALYZE, BUFFERS OFF, TIMING OFF, COSTS OFF, SUMMARY OFF)
select ten, unique1 from wisconsin_numeric where ten between -10000 and 4 and unique1 = 5555 order by ten desc, unique1 desc;

-- (July 23, Week of MIT visit, add test coverage)
--
-- Demonstrate how returning false when decrementing a non-skip-support skip
-- array with a >= inequality early due to the current array element already
-- being == the >= inequality's sk_argument.  This optimization is symmetrical
-- to the similar forward scan <= inequality case, which has plenty of code
-- coverage from other tests.
--
-- XXX UPDATE (January 22 2025) This optimization is now disabled, since it now
-- doesn't seem worth the trouble of keeping around a maybe-cross-type ORDER
-- proc and doing all of those extra comparisons just for this case.
select ten, unique1 from wisconsin_numeric where ten between 2 and 3 and unique1 = 5555 order by ten desc, unique1 desc;
EXPLAIN (ANALYZE, BUFFERS OFF, TIMING OFF, COSTS OFF, SUMMARY OFF) -- 13 buffer hits (was 10 with optimization enabled)
select ten, unique1 from wisconsin_numeric where ten between 2 and 3 and unique1 = 5555 order by ten desc, unique1 desc;

set enable_bitmapscan to on;
set enable_indexscan to off;



drop index numeric_ten_idx;

-- Four, ten:
create index numeric_four_ten_idx on wisconsin_numeric (four, ten, unique1);

-- (July 18) Make sure that we don't repeatedly access page 1 due to getting
-- confused about -inf value that lands us before the range of the column "ten"
select four, ten, unique1
from wisconsin_numeric
where ten between -10000 and 1 and unique1 = 113
limit 1; -- Just to avoid distraction of other, later pages (just care about page 1)
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select four, ten, unique1
from wisconsin_numeric
where ten between -10000 and 1 and unique1 = 113
limit 1; -- Just to avoid distraction of other, later pages (just care about page 1)

-- Point lookup, skips one col (four), range on other col after that (ten):
select four, ten, unique1 from wisconsin_numeric where ten between -10000 and 4 and unique1 = 5555;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select four, ten, unique1 from wisconsin_numeric where ten between -10000 and 4 and unique1 = 5555;

drop index numeric_four_ten_idx;

set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

insert into wisconsin_numeric(unique1, unique2, two, four, ten, twenty, onepercent)
select 1, 1, 1, 1, 1, 1, 2147483647 from generate_series(1, 1500) i;
insert into wisconsin_numeric(unique1, unique2, two, four, ten, twenty, onepercent)
select 1, 1, 1, 1, 1, 1, (-2147483648)::int4 from generate_series(1, 1500) i;

insert into wisconsin_numeric(unique1, unique2, two, four, ten, twenty, onepercent)
select -666, 1, 1, 1, 1, 1, 2147483647;
insert into wisconsin_numeric(unique1, unique2, two, four, ten, twenty, onepercent)
select -666, 1, 1, 1, 1, 1, (-2147483648)::int4;

insert into wisconsin_numeric(unique1, unique2, two, four, ten, twenty, onepercent)
select 1, 1, 1, 1, 1, 1, NULL from generate_series(1, 1500) i;
insert into wisconsin_numeric(unique1, unique2, two, four, ten, twenty, onepercent)
select -666, 1, 1, 1, 1, 1, NULL;
vacuum analyze wisconsin_numeric;
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

create index numeric_idx_four_ten_twenty_unique1 on wisconsin_numeric (four, ten, twenty, unique1);

-- Simplest version:
select four, ten, twenty, unique1 from wisconsin_numeric where four in (2 , 3) and ten between 3 and 3 and twenty = 3 and unique1 = 84396;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF)
select four, ten, twenty, unique1 from wisconsin_numeric where four in (2 , 3) and ten between 3 and 3 and twenty = 3 and unique1 = 84396;

-- Simplest version of other, similar bug:
prepare simple_repro as
select four, ten, twenty, unique1 from wisconsin_numeric where four between 2 and 3 and ten between 2 and 3 and twenty = 3 and unique1 = 84396;

execute simple_repro;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF, COSTS OFF)
execute simple_repro;

-- SAOP-only version should return 2 rows:
select four, ten, twenty, unique1 from wisconsin_numeric where ten in (2,3) and twenty in (2, 3) and unique1 in (84396, 60814);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select four, ten, twenty, unique1 from wisconsin_numeric where ten in (2,3) and twenty in (2, 3) and unique1 in (84396, 60814);

-- Equivalent "SAOP, range skip array" version should also return 2 rows:
select four, ten, twenty, unique1 from wisconsin_numeric where ten in (2,3) and twenty between 2 and 3 and unique1 in (84396, 60814);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select four, ten, twenty, unique1 from wisconsin_numeric where ten in (2,3) and twenty between 2 and 3 and unique1 in (84396, 60814);

-- Equivalent "range skip array, SAOP" version should also return 2 rows:
select four, ten, twenty, unique1 from wisconsin_numeric where ten between 2 and 3 and twenty in (2, 3) and unique1 in (84396, 60814);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select four, ten, twenty, unique1 from wisconsin_numeric where ten between 2 and 3 and twenty in (2, 3) and unique1 in (84396, 60814);

-- Equivalent "range skip array, range skip array" version should also return 2 rows:
select four, ten, twenty, unique1 from wisconsin_numeric where ten between 2 and 3 and twenty between 2 and 3 and unique1 in (84396, 60814);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select four, ten, twenty, unique1 from wisconsin_numeric where ten between 2 and 3 and twenty between 2 and 3 and unique1 in (84396, 60814);

-- Four, ten, twenty, unique1 (causes errors about attribute order from
-- _by_preprocess_keys) -- this is the original and more complicated version
-- of the previous tests (I broke it into smaller parts, but kept the
-- original here):
select four, ten, twenty, unique1
from wisconsin_numeric
where
  ten between 1 and 10
  and twenty in (1, 2, 3)
  and unique1 in (84396, 217539, 60814);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF) -- master 822 hits
select four, ten, twenty, unique1
from wisconsin_numeric
where
  ten between 1 and 10
  and twenty in (1, 2, 3)
  and unique1 in (84396, 217539, 60814);

-------------------------------------
-- (June 16) NULLS FIRST test case --
-------------------------------------
set client_min_messages=error;
drop table if exists nulls_first_test;
reset client_min_messages;

create unlogged table nulls_first_test(
  a int,
  b int
);

create index nulls_first_test_idx on nulls_first_test(a nulls first, b);

insert into nulls_first_test(a, b)
select NULL, i from generate_series(1, 3500) i;
insert into nulls_first_test(a, b)
select 2147483647, i from generate_series(1, 3500) i;

set enable_indexscan to on;
set enable_bitmapscan to off;

-- Forwards scan
select * from nulls_first_test where b = 3 order by a nulls first, b;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_first_test where b = 3 order by a nulls first, b;

-- Backwards scan
select * from nulls_first_test where b = 3 order by a desc nulls last, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_first_test where b = 3 order by a desc nulls last, b desc;

-- Now add INT_MIN grouping:
insert into nulls_first_test(a, b)
select (-2147483648)::int4, i from generate_series(1, 3500) i;

-- Repeat forwards scan
select * from nulls_first_test where b = 3 order by a nulls first, b;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_first_test where b = 3 order by a nulls first, b;

-- Repeat backwards scan
select * from nulls_first_test where b = 3 order by a desc nulls last, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_first_test where b = 3 order by a desc nulls last, b desc;

-- (July 7) NULLS FIRST numeric (i.e. no skip support) test case
set client_min_messages=error;
drop table if exists nulls_first_test_numeric;
reset client_min_messages;

create unlogged table nulls_first_test_numeric(
  a numeric,
  b int
);

create index nulls_first_test_numeric_idx on nulls_first_test_numeric(a nulls first, b);

insert into nulls_first_test_numeric(a, b)
select NULL, i from generate_series(1, 3500) i;
insert into nulls_first_test_numeric(a, b)
select 2147483647, i from generate_series(1, 3500) i;

set enable_indexscan to on;
set enable_bitmapscan to off;

-- Forwards scan
select * from nulls_first_test_numeric where b = 3 order by a nulls first, b;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_first_test_numeric where b = 3 order by a nulls first, b;

-- Backwards scan
select * from nulls_first_test_numeric where b = 3 order by a desc nulls last, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_first_test_numeric where b = 3 order by a desc nulls last, b desc;

-- Now add INT_MIN grouping:
insert into nulls_first_test_numeric(a, b)
select (-2147483648)::int4, i from generate_series(1, 3500) i;

-- Repeat forwards scan
select * from nulls_first_test_numeric where b = 3 order by a nulls first, b;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_first_test_numeric where b = 3 order by a nulls first, b;

-- Repeat backwards scan
select * from nulls_first_test_numeric where b = 3 order by a desc nulls last, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_first_test_numeric where b = 3 order by a desc nulls last, b desc;

-- (July 16) Coverage for "treat NULL as final value in backwards scan
-- direction" case (without skip support):
select * from nulls_first_test_numeric where b = 3130 order by a desc nulls last, b;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- This is 15 buffer hits (not 17) due to optimization
select * from nulls_first_test_numeric where b = 3130 order by a desc nulls last, b;

------------------------------------------------------------------------
-- (July 2) Alexander Alekseev failing "char" (not char(1)) test case --
------------------------------------------------------------------------
set client_min_messages=error;
drop table if exists alekseev_test;
reset client_min_messages;
create unlogged table alekseev_test(c "char", n bigint);

select setseed(0.5);
insert into alekseev_test
select chr(ascii('a') + random(0,2)) as c,
random(0, 1_000_000_000) as n
from generate_series(0, 10_000);

create index alekseev_test_idx on alekseev_test using btree(c, n);
vacuum analyze alekseev_test;

-- Force bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Better not have a buggy btree/char_ops skip support function:
select count(*) from alekseev_test where n > 900_000_000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 11 buffer hits
select count(*) from alekseev_test where n > 900_000_000;

-- More selective query:
select c, n from alekseev_test where n = 952_200_397;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where n = 952_200_397;

-- More selective query, low value that exists:
select c, n from alekseev_test where n = 24_759;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where n = 24_759;

-- More selective query, low value that does not exist:
select c, n from alekseev_test where n = -1000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where n = -1000;

-- Extremal elements so that scan visits leftmost and rightmost tuples within
-- each individual "c" grouping:
select c, n from alekseev_test where n in (1, 24759, 999843016);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 8 buffer hits
select c, n from alekseev_test where n in (1, 24759, 999843016);

-- Middling elements in SAOP test:
select c, n
from
  alekseev_test
where
n in (500_048_538,
      500_061_970,
      500_129_489,
      500_143_236,
      500_164_863,
      500_229_159,
      500_255_696,
      500_411_856,
      500_448_495,
      500_630_870);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 10 buffer hits
select c, n
from
  alekseev_test
where
n in (500_048_538,
      500_061_970,
      500_129_489,
      500_143_236,
      500_164_863,
      500_229_159,
      500_255_696,
      500_411_856,
      500_448_495,
      500_630_870);

-- Force index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

---------------------------------
-- "char" backwards scan tests --
---------------------------------

-- Extremal elements so that scan visits leftmost and rightmost tuples within
-- each individual "c" grouping:
select c, n from alekseev_test where n in (1, 24759, 999843016) order by c desc, n desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 12 buffer hits
select c, n from alekseev_test where n in (1, 24759, 999843016) order by c desc, n desc;

-------------------------------------------------------
-- Text tests, which can't use skip support function --
-------------------------------------------------------

-- Make "more selective query" work with text, so we have somewhat of a basis
-- of comparison:
alter table alekseev_test alter column c type text;

-- Force bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

------------------------------------
-- Now repeat queries from before --
------------------------------------

-- "Better not have a buggy btree/char_ops skip support function", but no skip
-- support function this time around (since this is text):
select count(*) from alekseev_test where n > 900_000_000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- parity with "char" test, 11 buffer hits
select count(*) from alekseev_test where n > 900_000_000;

-- More selective query:
select c, n from alekseev_test where n = 952_200_397;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where n = 952_200_397;

-- More selective query, low value that exists:
select c, n from alekseev_test where n = 24_759;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where n = 24_759;

-- More selective query, low value that does not exist:
select c, n from alekseev_test where n = -1000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where n = -1000;

-- Extremal elements so that scan visits leftmost and rightmost tuples within
-- each individual "c" grouping:
select c, n from alekseev_test where n in (1, 24759, 999843016);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- parity with "char" test, 8 buffer hits
select c, n from alekseev_test where n in (1, 24759, 999843016);

-- Middling elements in SAOP test:
select c, n
from
  alekseev_test
where
n in (500_048_538,
      500_061_970,
      500_129_489,
      500_143_236,
      500_164_863,
      500_229_159,
      500_255_696,
      500_411_856,
      500_448_495,
      500_630_870);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 14 buffer hits vs 10 for "char" due to not naturally needing to visit extremal n values
select c, n
from
  alekseev_test
where
n in (500_048_538,
      500_061_970,
      500_129_489,
      500_143_236,
      500_164_863,
      500_229_159,
      500_255_696,
      500_411_856,
      500_448_495,
      500_630_870);

-- Force index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-------------------------------
-- text backwards scan tests --
-------------------------------

-- Extremal elements so that scan visits leftmost and rightmost tuples within
-- each individual "c" grouping:
select c, n from alekseev_test where n in (1, 24759, 999843016) order by c desc, n desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 12 buffer hits
select c, n from alekseev_test where n in (1, 24759, 999843016) order by c desc, n desc;

------------------------------
-- text NULL tests (July 7) --
------------------------------
insert into alekseev_test
select null, n from alekseev_test;

-- More selective query:
select c, n from alekseev_test where n = 952_200_397;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where n = 952_200_397;

-- More selective query, low value that exists:
select c, n from alekseev_test where n = 24_759;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where n = 24_759;

-- More selective query, low value that does not exist:
select c, n from alekseev_test where n = -1000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where n = -1000;

-- Extremal elements so that scan visits leftmost and rightmost tuples within
-- each individual "c" grouping:
select c, n from alekseev_test where n in (1, 24759, 999843016);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- parity with "char" test, 8 buffer hits
select c, n from alekseev_test where n in (1, 24759, 999843016);

-- Same again, but IS NOT NULL inequality used on skip attribute:
select c, n from alekseev_test where c is not null and n in (1, 24759, 999843016);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- parity with "char" test, 8 buffer hits
select c, n from alekseev_test where c is not null and n in (1, 24759, 999843016);

-- Same again, but with regular > inequality used on skip attribute:
select c, n from alekseev_test where c  > 'a' and n in (1, 24759, 999843016);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- parity with "char" test, 8 buffer hits
select c, n from alekseev_test where c  > 'a' and n in (1, 24759, 999843016);

-- Same again, but with regular < inequality used on skip attribute:
select c, n from alekseev_test where c  < 'c' and n in (1, 24759, 999843016);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- parity with "char" test, 8 buffer hits
select c, n from alekseev_test where c  < 'c' and n in (1, 24759, 999843016);

-- Middling elements in SAOP test:
select c, n
from
  alekseev_test
where
n in (500_048_538,
      500_061_970,
      500_129_489,
      500_143_236,
      500_164_863,
      500_229_159,
      500_255_696,
      500_411_856,
      500_448_495,
      500_630_870);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 14 buffer hits vs 10 for "char" due to not naturally needing to visit extremal n values
select c, n
from
  alekseev_test
where
n in (500_048_538,
      500_061_970,
      500_129_489,
      500_143_236,
      500_164_863,
      500_229_159,
      500_255_696,
      500_411_856,
      500_448_495,
      500_630_870);

---------------------------
-- NULL + Backwards scan --
---------------------------

-- Extremal elements so that scan visits leftmost and rightmost tuples within
-- each individual "c" grouping:
select c, n from alekseev_test where n in (1, 24759, 999843016) order by c desc, n desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 12 buffer hits
select c, n from alekseev_test where n in (1, 24759, 999843016) order by c desc, n desc;

-- Same again, but with IS NOT NULL on c:
select c, n from alekseev_test where c is not null and n in (1, 24759, 999843016) order by c desc, n desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 12 buffer hits
select c, n from alekseev_test where c is not null and n in (1, 24759, 999843016) order by c desc, n desc;

-- Same again, but with regular > inequality used on skip attribute:
select c, n from alekseev_test where c  > 'a' and n in (1, 24759, 999843016) order by c desc, n desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- parity with "char" test, 8 buffer hits
select c, n from alekseev_test where c  > 'a' and n in (1, 24759, 999843016) order by c desc, n desc;

-- Same again, but with regular < inequality used on skip attribute:
select c, n from alekseev_test where c  < 'c' and n in (1, 24759, 999843016) order by c desc, n desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- parity with "char" test, 8 buffer hits
select c, n from alekseev_test where c  < 'c' and n in (1, 24759, 999843016) order by c desc, n desc;

-- Almost the same query again, but now it's an <=:
select c, n from alekseev_test where c  <= 'b' and n in (1, 24759, 999843016) order by c desc, n desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where c  <= 'b' and n in (1, 24759, 999843016) order by c desc, n desc;

-- Same exact query again, but now it's a forwards scan:
select c, n from alekseev_test where c  < 'c' and n in (1, 24759, 999843016);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where c  < 'c' and n in (1, 24759, 999843016);

-- Almost the same query again, but now it's an <=:
select c, n from alekseev_test where c  <= 'b' and n in (1, 24759, 999843016);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select c, n from alekseev_test where c  <= 'b' and n in (1, 24759, 999843016);

--
--
-- (June 6) Check behavior with duplicate index column contents
--
-- From create_index.sql, failed when you first added support for skip scan on
-- an ocplass lacking a skip support routine
--
set client_min_messages=error;
drop table if exists dupindexcols;
reset client_min_messages;
CREATE UNLOGGED TABLE dupindexcols AS
  SELECT unique1 as id, stringu2::text as f1 FROM tenk1_skipscan;
CREATE INDEX dupindexcols_i ON dupindexcols (f1, id, f1 text_pattern_ops);
ANALYZE dupindexcols;

SELECT count(*) FROM dupindexcols
  WHERE f1 BETWEEN 'WA' AND 'ZZZ' and id < 1000 and f1 ~<~ 'YX';
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
  SELECT count(*) FROM dupindexcols
    WHERE f1 BETWEEN 'WA' AND 'ZZZ' and id < 1000 and f1 ~<~ 'YX';

-- (July 12) One of the last failing test cases when adding support for
-- non-skip-support opclasses.
--
-- This is based on int tests from Postgres 17 SAOP test suite, which failed
-- when I forced nbtree to not use skip support as a simple smoke test
--
-- This turned out to be an issue with NULLs, fixed like so:

-- diff --git a/src/backend/access/nbtree/nbtutils.c b/src/backend/access/nbtree/nbtutils.c
-- index 2e4cccde9..1c9519283 100644
-- --- a/src/backend/access/nbtree/nbtutils.c
-- +++ b/src/backend/access/nbtree/nbtutils.c
-- @@ -2002,20 +2002,10 @@ _bt_binsrch_skiparray_skey(FmgrInfo *orderproc,
--      {
--          if (tupnull && !array->null_elem)
--          {
-- -            if (ScanDirectionIsForward(dir))
-- -            {
-- -                if (!(cur->sk_flags & SK_BT_NULLS_FIRST))
-- -                    *set_elem_result = 1;
-- -                else
-- -                    *set_elem_result = -1;
-- -            }
-- +            if (!(cur->sk_flags & SK_BT_NULLS_FIRST))
-- +                *set_elem_result = 1;
--              else
-- -            {
-- -                if (!(cur->sk_flags & SK_BT_NULLS_FIRST))
-- -                    *set_elem_result = -1;
-- -                else
-- -                    *set_elem_result = 1;
-- -            }
-- +                *set_elem_result = -1;
--
--              return;
--          }
set client_min_messages=error;
drop table if exists redescend_numeric_test;
reset client_min_messages;
create unlogged table redescend_numeric_test (district numeric, warehouse numeric, orderid numeric, orderline numeric);
create index must_not_full_scan_numeric on redescend_numeric_test (district, warehouse, orderid, orderline) with (fillfactor=30);
insert into redescend_numeric_test
select district, warehouse, orderid, orderline
from
  generate_series(1, 3) district,
  generate_series(1, 5) warehouse,
  generate_series(1, 150) orderid,
  generate_series(1, 10) orderline
order by
district, warehouse, orderid, orderline;
-- prewarm
select count(*) from redescend_numeric_test;
vacuum analyze redescend_numeric_test;
---------------------------------------------------------------------------------

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

set client_min_messages=error;
drop table if exists redescend_numeric_test;
reset client_min_messages;
create unlogged table redescend_numeric_test (district numeric, warehouse numeric, orderid numeric, orderline numeric);
create index must_not_full_scan_numeric_idx on redescend_numeric_test (district, warehouse, orderid, orderline) with (fillfactor=30);
insert into redescend_numeric_test
select district, warehouse, orderid, orderline
from
  generate_series(1, 3) district,
  generate_series(1, 5) warehouse,
  generate_series(1, 150) orderid,
  generate_series(1, 10) orderline
order by
district, warehouse, orderid, orderline;
-- prewarm
select count(*) from redescend_numeric_test;
vacuum analyze redescend_numeric_test;
---------------------------------------------------------------------------------

insert into redescend_numeric_test
select district, NULL, NULL, NULL
from
  generate_series(1, 3) district,
  generate_series(1, 5) want_five_nulls_per_district;

-- (July 16) Suspected that this was a bug, but master is also affected (no
-- skip arrays here).
--
-- The fact that we have to read page 217 twice here is okay, AFAICT, because
-- the second read cannot possibly find matching tuples that are returned to
-- the scan.
--
-- Note: a bunch of later tests also read page 217 twice, in much the same
-- way.  This test memorializes the fact that this issue isn't unique to skip
-- scan.
select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse = 1 and orderid = 1 and orderline in (-1, 2) -- "orderline in (-1, 1)" won't do here
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse = 1 and orderid = 1 and orderline in (-1, 2)
order by district desc, warehouse desc, orderid desc, orderline desc;

-- Simplest possible repro:
select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse <= 1 and orderid = 1 and orderline = 6
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse <= 1 and orderid = 1 and orderline = 6
order by district desc, warehouse desc, orderid desc, orderline desc;

select * from redescend_numeric_test where district in (1,2,3) and warehouse > 4 and orderid > 149;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_numeric_test where district in (1,2,3) and warehouse > 4 and orderid > 149; -- 24 buffer hits patch, 76 buffer hits master

select * from redescend_numeric_test where district in (1,2,3) and warehouse = 5 and orderid > 149;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_numeric_test where district in (1,2,3) and warehouse = 5 and orderid > 149; -- 13 buffer hits (patch + master)

select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse < 2 and orderid < 2 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse < 2 and orderid < 2 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse <= 1 and orderid in (0, 1) and orderline >= any ('{6,7,8}')
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse <= 1 and orderid in (0, 1) and orderline >= any ('{6,7,8}')
order by district desc, warehouse desc, orderid desc, orderline desc;

select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (-1, 6, 8, 1000)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (-1, 6, 8, 1000)
order by district desc, warehouse desc, orderid desc, orderline desc;

select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse <= 1 and orderid < 2 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse <= 1 and orderid < 2 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse < 2 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_numeric_test
where district in (1, 2, 3) and warehouse < 2 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

----------------------------------------------------------------------------
-- Inequalties required in opposite direction only with skip arrays tests --
----------------------------------------------------------------------------

-- (August 6) These tests were added when you finally started to get serious
-- about the need for skip-array-specific heuristics
set client_min_messages=error;
drop table if exists lessthan_skip_test;
reset client_min_messages;

-- Bitmap index scans
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

create unlogged table lessthan_skip_test(skipattr int4, i int4);
create index ltidx on lessthan_skip_test (skipattr , i);
insert into lessthan_skip_test
select val, dups_per_val from generate_series(1, 90) val,
                generate_series(1,50) dups_per_val;

-- ltidx key space shows that we've truncated every pivot tuple's i key column:

-- pg@regression:5432 [309814]=# :leafkeyspace
-- ┌────┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬────────────────────┐
-- │ i  │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │      highkey       │
-- ├────┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼────────────────────┤
-- │  1 │     1 │     1 │    350 │     20 │          0 │   351 │     0 │                 0 │      16 │     1,128 │ (skipattr, i)=(8)  │
-- │  2 │    17 │     1 │    150 │     20 │          0 │   151 │     0 │                 0 │      16 │     5,128 │ (skipattr, i)=(11) │
-- │  3 │    12 │     1 │     50 │     20 │          0 │    51 │     0 │                 0 │      16 │     7,128 │ (skipattr, i)=(12) │
-- │  4 │    11 │     1 │    350 │     20 │          0 │   351 │     0 │                 0 │      16 │     1,128 │ (skipattr, i)=(19) │
-- │  5 │     6 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(24) │
-- │  6 │    18 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(29) │
-- │  7 │    10 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(34) │
-- │  8 │    19 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(39) │
-- │  9 │     4 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(44) │
-- │ 10 │    20 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(49) │
-- │ 11 │    13 │     1 │     50 │     20 │          0 │    51 │     0 │                 0 │      16 │     7,128 │ (skipattr, i)=(50) │
-- │ 12 │     8 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(55) │
-- │ 13 │    21 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(60) │
-- │ 14 │    14 │     1 │     50 │     20 │          0 │    51 │     0 │                 0 │      16 │     7,128 │ (skipattr, i)=(61) │
-- │ 15 │     9 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(66) │
-- │ 16 │    22 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(71) │
-- │ 17 │    15 │     1 │     50 │     20 │          0 │    51 │     0 │                 0 │      16 │     7,128 │ (skipattr, i)=(72) │
-- │ 18 │     7 │     1 │    200 │     20 │          0 │   201 │     0 │                 0 │      16 │     4,128 │ (skipattr, i)=(76) │
-- │ 19 │     5 │     1 │    200 │     20 │          0 │   201 │     0 │                 0 │      16 │     4,128 │ (skipattr, i)=(80) │
-- │ 20 │     2 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(85) │
-- │ 21 │    23 │     1 │    250 │     20 │          0 │   251 │     0 │                 0 │      16 │     3,128 │ (skipattr, i)=(90) │
-- │ 22 │    16 │     1 │     50 │     20 │          0 │    50 │     0 │                 0 │      16 │     7,148 │ ∅                  │
-- └────┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴────────────────────┘
-- (22 rows)
--
-- This is :rootitems, just to avoid test regressions:
select itemoffset, ctid, itemlen, nulls from bt_page_items('ltidx',
  (select fastroot::int4 from bt_metap('ltidx')));

-- 23 index buffer hits (parity with master):
select * from lessthan_skip_test where i = 2;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i = 2;

-- Also 23 index buffer hits (also parity with master):
select * from lessthan_skip_test where i < 2;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i < 2;

-- Problematic case where we also expect 23 index buffer hits, for parity with
-- master:
select * from lessthan_skip_test where i > 46;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i > 46;

-- Another problematic case where we also expect 23 index buffer hits, for
-- parity with master:
select * from lessthan_skip_test where i between 40 and 46;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i between 40 and 46;

-- Another problematic case where we also expect 23 index buffer hits, for
-- parity with master:
select * from lessthan_skip_test where i between 50 and 51;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i between 50 and 51;

-- Another problematic case where master gets 23 index buffer hits:
select * from lessthan_skip_test where i between 51 and 50;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i between 51 and 50;

-- backwards scans:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- 24 index buffer hits on master:
select * from lessthan_skip_test where i = 2 order by skipattr desc, i desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i = 2 order by skipattr desc, i desc;

-- 24 index buffer hits on master:
select * from lessthan_skip_test where i < 2 order by skipattr desc, i desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i < 2 order by skipattr desc, i desc;

-- 203 buffer hits on master:
select * from lessthan_skip_test where i > 46 order by skipattr desc, i desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i > 46 order by skipattr desc, i desc;

-- 365 buffer hits on master:
select * from lessthan_skip_test where i between 40 and 46 order by skipattr desc, i desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i between 40 and 46 order by skipattr desc, i desc;

-- 24 index buffer hits on master:
select * from lessthan_skip_test where i between 50 and 51 order by skipattr desc, i desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i between 50 and 51 order by skipattr desc, i desc;

-- 23 index buffer hits on master:
select * from lessthan_skip_test where i between 51 and 50 order by skipattr desc, i desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from lessthan_skip_test where i between 51 and 50 order by skipattr desc, i desc;

-- (September 22 2024)
--
-- Sunday after Andrew's birthday.  Test case proved that I was right to be
-- paranoid about reusing NEXTPRIOR flag for backwards and forwards scans --
-- it is indeed subtly broken.
--
-- What if the scan changes direction, and "5 + infinitesimal" becomes
-- "5 - infinitesimal" without our intending it?  That wouldn't be obviously
-- broken in most cases, but it would be broken if the scan happened to have a
-- lower-order SAOP array mixed in.
set work_mem = 64;
set enable_sort = off;

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

set client_min_messages=error;
drop table if exists nextprior_duplicate_test;
reset client_min_messages;

create unlogged table nextprior_duplicate_test(dup numeric, dups_per_val numeric);
create index nextprior_test_idx on nextprior_duplicate_test (dup, dups_per_val);
insert into nextprior_duplicate_test(dup, dups_per_val)
select val, dups_per_val from generate_series(1, 20) val,
                generate_series(1,900) dups_per_val;
vacuum analyze nextprior_duplicate_test; -- Be tidy

begin;
declare nextprior_cursor cursor for
select * from nextprior_duplicate_test where dups_per_val in (1, 450) order by dup, dups_per_val;

fetch forward 2 from nextprior_cursor;
fetch forward 2 from nextprior_cursor;
fetch forward 2 from nextprior_cursor;

-- First wrong answer was seen here with initial failing test:
fetch backward 1 from nextprior_cursor;
-- Note: this line hit the familiar "precheck has invalid array keys"
-- assertion failure from the top of _bt_checkkeys, too:
-- TRAP: failed Assert("!_bt_tuple_before_array_skeys(scan, dir, tuple, tupdesc, tupnatts, false, 0, NULL)")

-- Naturally, these were also wrong, since the state of the array keys is now
-- corrupt:
fetch backward 1 from nextprior_cursor;
fetch forward 1 from nextprior_cursor;
fetch forward 1 from nextprior_cursor;
fetch forward 1 from nextprior_cursor;
fetch forward 1 from nextprior_cursor;

/* nextprior_cursor  */ commit;

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
declare nextprior_cursor cursor for
select * from nextprior_duplicate_test where dups_per_val in (1, 450) order by dup, dups_per_val;

-- (September 23) DESC variant
--
-- Same again, but this time use "order by dup desc, dups_per_val desc".  This
-- can independently fail; it adds coverage for _bt_tuple_before_array_skeys
-- code path where the initial scan order was forward, and then becomes
-- backward due to cursor direction changing (original tested the opposite
-- transition, from forward direction to backward direction)
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
declare nextprior_cursor_desc cursor for
select * from nextprior_duplicate_test where dups_per_val in (450, 899)
order by dup desc, dups_per_val desc;

begin;
declare nextprior_cursor_desc cursor for
select * from nextprior_duplicate_test where dups_per_val in (450, 899)
order by dup desc, dups_per_val desc;

fetch forward 2 from nextprior_cursor_desc;
fetch forward 2 from nextprior_cursor_desc;
fetch forward 2 from nextprior_cursor_desc;

-- First wrong answer was seen here, just like original "order by dup asc,
-- dups_per_val asc" version:
fetch backward 1 from nextprior_cursor_desc;
-- Naturally, these were also wrong, since the state of the array keys is now
-- corrupt:
fetch backward 1 from nextprior_cursor_desc;
fetch forward 1 from nextprior_cursor_desc;
fetch forward 1 from nextprior_cursor_desc;
fetch forward 1 from nextprior_cursor_desc;
fetch forward 1 from nextprior_cursor_desc;

/* nextprior_cursor_desc  */ commit;

-- (November 23) Add test coverage for "mixed cardinality" cases where
-- leftmost pages have lots of distinct values in leading skippy column, but
-- then we encounter a few very large groupings across the index key space
--
-- Original intent of this was to get test coverage for experimental
-- optimization:

/*
commit b4a50b6441440b2a56d84c2c357b31befa945cc3
Refs: [skip-scan-2024-v17.1]
Author:     Peter Geoghegan <pg@bowt.ie>
AuthorDate: 2024-11-23 13:09:59 -0500
Commit:     Peter Geoghegan <pg@bowt.ie>
CommitDate: 2024-11-23 14:20:45 -0500

    Experiment, which doesn't increase any buffers accessed by more than one or two, suggesting a gap in test coverage
---
 src/backend/access/nbtree/nbtsearch.c | 12 ++++++------
 1 file changed, 6 insertions(+), 6 deletions(-)

diff --git a/src/backend/access/nbtree/nbtsearch.c b/src/backend/access/nbtree/nbtsearch.c
index 34deb970e..1cb8ffc93 100644
--- a/src/backend/access/nbtree/nbtsearch.c
+++ b/src/backend/access/nbtree/nbtsearch.c
@@ -1841,16 +1841,16 @@ _bt_readpage(IndexScanDesc scan, ScanDirection dir, OffsetNumber offnum,
            IndexTuple  itup = (IndexTuple) PageGetItem(page, iid);
            int         truncatt;

-           truncatt = BTreeTupleGetNAtts(itup, rel);
-           pstate.prechecked = false;  / precheck didn't cover HIKEY /
            if (pstate.skipskip)
            {
                Assert(itup == pstate.finaltup);
-
-               _bt_start_array_keys(scan, dir);
-               pstate.skipskip = false;    / reset for finaltup /
            }
-           _bt_checkkeys(scan, &pstate, arrayKeys, itup, truncatt);
+           else
+           {
+               truncatt = BTreeTupleGetNAtts(itup, rel);
+               pstate.prechecked = false;  / precheck didn't cover HIKEY /
+               _bt_checkkeys(scan, &pstate, arrayKeys, itup, truncatt);
+           }
        }

        if (!pstate.continuescan)
*/

set client_min_messages=error;
drop table if exists high_low_high_card;
reset client_min_messages;
create unlogged table high_low_high_card(
  skippy int4,
  key int4,
  type text
);
create index on high_low_high_card(skippy, key);

insert into high_low_high_card
select
  -- "+ 250" here to make sure that there are leftmost pages full of tuples with
  -- distinct "skippy" vals:
  (abs(hashint4(i % 5)) + 250) % (10000 + 250),
  abs(hashint4(i + 42)) % 100_000,
  'fat'
from
  generate_series(1, 100_000) i;

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

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

--------------------
-- Forwards scans --
--------------------
select * from high_low_high_card where key = 40 order by skippy, key;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select * from high_low_high_card where key = 40 order by skippy, key;

select * from high_low_high_card where key = 37 order by skippy, key;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select * from high_low_high_card where key = 37 order by skippy, key;

select * from high_low_high_card where key = 38 order by skippy, key;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select * from high_low_high_card where key = 38 order by skippy, key;

select * from high_low_high_card where key = 13 order by skippy, key;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select * from high_low_high_card where key = 13 order by skippy, key;

select * from high_low_high_card where key = 15 order by skippy, key;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select * from high_low_high_card where key = 15 order by skippy, key;

---------------------
-- Backwards scans --
---------------------
select * from high_low_high_card where key = 40 order by skippy desc, key desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select * from high_low_high_card where key = 40 order by skippy desc, key desc;

select * from high_low_high_card where key = 37 order by skippy desc, key desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select * from high_low_high_card where key = 37 order by skippy desc, key desc;

select * from high_low_high_card where key = 38 order by skippy desc, key desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select * from high_low_high_card where key = 38 order by skippy desc, key desc;

select * from high_low_high_card where key = 13 order by skippy desc, key desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select * from high_low_high_card where key = 13 order by skippy desc, key desc;

select * from high_low_high_card where key = 15 order by skippy desc, key desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF, SUMMARY OFF)
select * from high_low_high_card where key = 15 order by skippy desc, key desc;

-----------------------------------------------
-- RowCompare skipskip test (January 1 2025) --
-----------------------------------------------

set client_min_messages=error;
drop table if exists rowcompare_skipskip_test;
reset client_min_messages;
create unlogged table rowcompare_skipskip_test(
  a int4,
  b int4,
  c int4
);
create index on rowcompare_skipskip_test(a, b, c);

-- Create table with 3 columns, all of which are high cardinality -- making
-- this a poor target for skip scan in general:
insert into rowcompare_skipskip_test
select
  i, i, i
from generate_series(1, 1000) i;

-- Lower-order RowCompare query, which makes use of skipskip optimization to
-- avoid perf regressions relative to master branch:
--
-- Original assertion failure caused by this query:
--
-- TRAP: failed Assert("!pstate->skipskip"), File:
-- "../source/src/backend/access/nbtree/nbtutils.c", Line: 4995, PID: 3455365
-- [0x5624bbbb24aa] _bt_checkkeys: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtutils.c:4995
-- [0x5624bbbab19c] _bt_readpage: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtsearch.c:1790
-- [0x5624bbbaa10d] _bt_readnextpage: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtsearch.c:2405
-- [0x5624bbbaa7bd] _bt_next: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtsearch.c:0
select * from rowcompare_skipskip_test where (b,c) < (4, 2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from rowcompare_skipskip_test where (b,c) < (4, 2);

-----------------------------------------------------------------------------------------------------
-- (Feb 11 2025) test for logic in _bt_forcenonrequired with mixed range and non-range skip arrays --
-----------------------------------------------------------------------------------------------------
set client_min_messages=error;
drop table if exists forcenonrequired_test;
reset client_min_messages;
create unlogged table forcenonrequired_test(a int, b int, c int);
create index forcenonrequired_test_idx on forcenonrequired_test (a, b, c);

-- Setup
-- set skipscan_prefix_cols = 0;
select setseed(0.5);
insert into forcenonrequired_test
select
  (random() * 100)::int4 as a,
  (random() * 10)::int4 as b,
  (random() * 10)::int4 as c from generate_series(1, 20_000) i;

-- This was wrong because I somehow got the idea that a non-range skip array
-- doesn't represent the lack of an inequality constraint (regardless of
-- whether or not we proved that its column value never changed on the page):
select count(*) from forcenonrequired_test where b between 6 and 6 and c = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from forcenonrequired_test where b between 6 and 6 and c = 1;

-- This variant that gives the same answer was okay at the time, though:
select count(*) from forcenonrequired_test where a between 0 and 100 and b between 6 and 6 and c = 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from forcenonrequired_test where a between 0 and 100 and b between 6 and 6 and c = 1;

-- (Feb 11 2025) SAOP array test case had wrong answers because we didn't
-- actually check if "a" scan key is satisfied by any "a" value on the page,
-- spuriously deciding that "a" scan key must be satisfied just because "a"
-- only has one distinct value.  This causes wrong answers on the page that
-- exclusively contains "a = 93" values, which shouldn't be returning any
-- tuples here.
select * from forcenonrequired_test where a in (91,92,94) and c = 1 order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from forcenonrequired_test where a in (91,92,94) and c = 1 order by a desc, b desc;
