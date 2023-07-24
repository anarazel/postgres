set work_mem='100MB';
set effective_cache_size='24GB';
set random_page_cost=2.0;
set track_io_timing to off;
set enable_seqscan to off;
set client_min_messages=error;
-- set skipscan_skipsupport_enabled=false;
set vacuum_freeze_min_age = 0;
set cursor_tuple_fraction=1.000;
create extension if not exists pageinspect; -- just to have it
create extension if not exists pg_buffercache; -- to evict data when needed
-- set statement_timeout='4s';
reset client_min_messages;

-- Set aio stuff explicitly:
set io_combine_limit = 16;
set effective_io_concurrency = 100;

-- Set log_btree_verbosity to 1 without depending on having that patch
-- applied (HACK, just sets commit_siblings instead when we don't have that
-- patch available):

-- Establish if this server is master or the patch -- want to skip stress
-- tests if it's the latter
--
-- Reminder: Don't vary the database state between master and patch (just the
-- tests run, which must be read-only)
select (setting = '5432') as testing_patch from pg_settings where name = 'port'
       \gset

-------------------------------
-- Basic single column tests --
-------------------------------
set client_min_messages=error;
drop table if exists skippy_tbl;
reset client_min_messages;

create unlogged table skippy_tbl(
  bar int4
);

create index skippy_idx on skippy_tbl(bar);

insert into skippy_tbl
select
  i
from
  generate_series(1, 500) i;
-- prewarm
select count(*) from skippy_tbl;
vacuum analyze skippy_tbl;
-------------------------------

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;
-- Simple example:
select ctid, bar from skippy_tbl where bar in (2,3,4);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (2,3,4);
-- Simple example of a backwards scan:
select ctid, bar from skippy_tbl where bar in (2,3,4) order by bar desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (2,3,4) order by bar desc;
-- continuescan-on-highkey case should work:
select ctid, bar from skippy_tbl where bar in (365,366);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (365,366);
-- pivotsearch (first item on leftmost leaf page's right sibling page) case
-- should also work:
select ctid, bar from skippy_tbl where bar in (367,368);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (367,368);
-- Gap of one shouldn't confuse us:
select ctid, bar from skippy_tbl where bar in (2,4);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (2,4);
-- Backwards scan gap of one shouldn't confuse us:
select ctid, bar from skippy_tbl where bar in (2,4) order by bar desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (2,4) order by bar desc;
-- Gap of two shouldn't confuse us:
select ctid, bar from skippy_tbl where bar in (2,5);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (2,5);
-- Backwards scan gap of two shouldn't confuse us:
select ctid, bar from skippy_tbl where bar in (2,5) order by bar desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (2,5) order by bar desc;

-- Adjoining non-pivot tuples split only by leaf page high key should require
-- only one descent of btree, so second page is read by read next page path:
select ctid, bar from skippy_tbl where bar in (366,367);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (366,367);
-- Equivalent backwards scan:
select ctid, bar from skippy_tbl where bar in (366,367) order by bar desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (366,367) order by bar desc;

-- Index-only scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to on;
set enable_indexscan to off;
-- This is the one that sometimes uses a sequential scan (when run as part of
-- the whole pg_regress suite):
select bar from skippy_tbl where bar in (2,3,4);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select bar from skippy_tbl where bar in (2,3,4);

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;
select ctid, bar from skippy_tbl where bar in (2,3,4);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (2,3,4);
-- Same as "simple example", but with duplicates:
insert into skippy_tbl(bar) values (22), (23), (23), (24), (24), (24);
vacuum analyze skippy_tbl;
select ctid, bar from skippy_tbl where bar in (22,23,24) order by bar;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (22,23,24) order by bar;

-- 3 non-pivot tuple matches:
select * from skippy_tbl where bar in (362,365,366);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from skippy_tbl where bar in (362,365,366);

-- Test non-SAOP case in passing, to avoid regressions in how we handle
-- more standard "boundary cases":
select * from skippy_tbl where bar = 366;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from skippy_tbl where bar = 366;

select * from skippy_tbl where bar = 367;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from skippy_tbl where bar = 367;

---------------------------------------------------
-- Large group of duplicates spanning many pages --
---------------------------------------------------
insert into skippy_tbl
select
  555
from
  generate_series(1, 3000) i;

vacuum analyze skippy_tbl;

-- Looks like this now:
--
-- ┌───┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬──────────────────────────────────┐
-- │ i │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │             highkey              │
-- ├───┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼──────────────────────────────────┤
-- │ 1 │     1 │     1 │    372 │      3 │          0 │   373 │     0 │                 0 │      16 │       688 │ (bar)=(367)                      │
-- │ 2 │     2 │     1 │    134 │      2 │          0 │   135 │     0 │                 0 │      16 │     5,448 │ (bar)=(555)                      │
-- │ 3 │     4 │     1 │  1,278 │      6 │          0 │     7 │     0 │                 0 │   1,115 │       312 │ (bar)=(555), (htid)=('(7,202)')  │
-- │ 4 │     5 │     1 │  1,278 │      7 │          0 │     7 │     0 │                 0 │   1,115 │       312 │ (bar)=(555), (htid)=('(13,124)') │
-- │ 5 │     6 │     1 │    444 │      3 │          0 │    39 │     0 │                 0 │      78 │     4,920 │ ∅                                │
-- └───┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴──────────────────────────────────┘
--
---------------------------------------------------

-- Scan blknos 2,4,5,6
--
-- This avoids continuescan termination on block 2, which used to happen due
-- to using the wrong scan key (the first, from 500 constant).
-- It's fixed, so now we switch to next SAOP element rather than
-- terminate _bt_first-wise/_bt_search-wise scan at that point
--
-- This does one less buffer access than master (only 5, not 6).  Master has
-- an extra root page access, which we can avoid.  It's only one less because
-- master does at least avoid visiting the same leaf page a second time in its
-- second _bt_first-wise scan of the index.
select count(*) from skippy_tbl where bar in (500,555);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from skippy_tbl where bar in (500,555);

-- Same again, almost -- just don't scan blkno 2 this time
--
-- This results in one useful _bt_search call.  We can avoid another useless
-- one by realizing that we already ran out of tuples to output at the end of
-- the fist _bt_search (which doesn't return any 556 rows either, since there
-- is nothing to return).
--
-- This variant of the query requires only 4 buffer accesses. As against 6
-- buffer accesses total in index for master branch.  Here we win by more
-- compared to last time (by 2 buffer accesses) because the master branch
-- wasn't so lucky about not having to visit the same leaf page a second time.
select count(*) from skippy_tbl where bar in (555,556);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from skippy_tbl where bar in (555,556);
-- The previous test case exercises how _bt_readpage deals with non-matches
-- covering key space > the highest non-pivot tuple in the index and < +inf.
-- Make sure that it continues to do that by checking the maximum value in the
-- index:
select max(bar) from skippy_tbl having max(bar) = 555; -- avoids regressing test coverage (i.e. tests the tests)

-- We do want to go through the root (3) to descend to the leftmost page (1) and then step to its right
-- sibling page (2):
-- XXX right now we don't do that -- what we actually do is redescend from the
-- root anew instead, just like the master branch -- so it's 4 buffer accesses
-- on the index instead of 3 accesses (we fall short of the obtainable
-- ideal, for now, since we're not yet able to be clever about using info from
-- internal pages -- nor are we willing to gamble even more aggressively).
select ctid, bar from skippy_tbl where bar in (1, 500);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, bar from skippy_tbl where bar in (1, 500);
-- Now show a similar case where even now we manage to get the obtainable
-- ideal (same path through the index is actually attained this time around).
-- This is possible here, independent of any speculative behavior and/or
-- cleverness when we descend the tree -- since the high key is 367, which
-- matches qual exactly. (We get only one descent and 3 buffer accesses,
-- versus master's 2 descents and 4 buffer accesses.  We manage to do better
-- than master, despite the fact that even master doesn't revisit the same
-- leaf page twice here -- master's only failing is that it touches the root
-- page a second time.)
select ctid, * from skippy_tbl where bar = any ('{365,367}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, * from skippy_tbl where bar = any ('{365,367}');

-- However, the patch isn't entirely free of such speculative behavior.
-- Here is a more complicated case that manages to be optimal -- though
-- barely.  Here we take a small gamble, and win.
--
-- This time around we don't have an exact leftmost page high key (367) match.
-- But we still win, since we do have 366 in both qual and in index (must be
-- both):
--
-- XXX UPDATE (December 3): Not anymore.  No longer speculatively visit next
-- page without an exact match for non-truncated columns
select * from skippy_tbl where bar = any ('{365,366,368}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from skippy_tbl where bar = any ('{365,366,368}');
-- We were "between the values 366 and 368" at the point that we reached the
-- high key, whose value is 367 -- which is also "between" the same two
-- values.  On that basis alone we decided to move right.  We gambled and won.
-- This was a limited form of gamble that was only chosen because the only
-- value we were missing from page was the high key, 367.  The high key is a
-- little special here.
--
-- Now lets try almost the same case, just with 366 missing.  That has a
-- surprisingly big impact: now we won't gamble at all.  This time when we
-- compare our search-type scan key to the non-pivot 366, we didn't get a
-- match, AND we terminated the scan locally (we accepted continuescan=false).
select * from skippy_tbl where bar = any ('{365,368}');   -- omit non-pivot value '366' this time
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from skippy_tbl where bar = any ('{365,368}');   -- 4 buffer accesses again

-- Now we'll show a gamble that doesn't pay off -- same rationale as earlier
-- gamble case, but this time we're not so lucky.  As a result, this query
-- needs an extra buffer access compared to master/no optimization case:
--
-- (This time we lose because 2147483647 isn't on the next page, despite it
-- seeming like it might.  XXX For now we'll accept this as a bad speculation;
-- a cost of doing business.  Might want to rereview that decision later on.)
--
-- Here we get 5 index buffer hits (one extra):
--
-- XXX UPDATE (December 3): not anymore.  As already noted in last UPDATE from
-- today, we don't move to next page when high key isn't an exact match in
-- respect of non-truncated attributes.  So no extra buffer hit (4 hits only).
select * from skippy_tbl where bar = any ('{366,2147483647}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from skippy_tbl where bar = any ('{366,2147483647}');

-- (August 21) Infinite loop binary-search-array-keys bug test case:
insert into skippy_tbl select 2^31-1;

with a as (
  select
    i
  from
    generate_series(1, 150000) i
)
select count(*) from skippy_tbl
where bar = any(array[(select array_agg(i) from a)]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
with a as (
  select
    i
  from
    generate_series(1, 150000) i
)
select count(*) from skippy_tbl
where bar = any(array[(select array_agg(i) from a)]);

-- Backwards scan (more or less equivalent)

set enable_sort = off;
with a as (
  select
    i
  from
    generate_series(1, 150000) i
)
select * from skippy_tbl
where bar = any(array[(select array_agg(i) from a)]) order by bar desc limit 50 offset 3000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
with a as (
  select
    i
  from
    generate_series(1, 150000) i
)
select * from skippy_tbl
where bar = any(array[(select array_agg(i) from a)]) order by bar desc limit 50 offset 3000;


-----------------------------------------------------------------------
-- "More than one so->numArrayKeys" test case (uses 2 SAOPs/columns) --
-----------------------------------------------------------------------
set client_min_messages=error;
drop table if exists multi_test;
reset client_min_messages;

create unlogged table multi_test(
  a int,
  b int
);

create index multi_test_idx on multi_test(a, b);

insert into multi_test
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

vacuum analyze multi_test;

-- Looks like this now:
--
-- ┌───┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬──────────────┐
-- │ i │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │   highkey    │
-- ├───┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼──────────────┤
-- │ 1 │     1 │     1 │    854 │      4 │          0 │   123 │     0 │                 0 │      55 │       808 │ (a, b)=(62)  │
-- │ 2 │     2 │     1 │    854 │      5 │          0 │   123 │     0 │                 0 │      55 │       808 │ (a, b)=(123) │
-- │ 3 │     4 │     1 │    854 │      5 │          0 │   123 │     0 │                 0 │      55 │       808 │ (a, b)=(184) │
-- │ 4 │     5 │     1 │    854 │      5 │          0 │   123 │     0 │                 0 │      55 │       808 │ (a, b)=(245) │
-- │ 5 │     6 │     1 │    854 │      4 │          0 │   123 │     0 │                 0 │      55 │       808 │ (a, b)=(306) │
-- │ 6 │     7 │     1 │    854 │      5 │          0 │   123 │     0 │                 0 │      55 │       808 │ (a, b)=(367) │
-- │ 7 │     8 │     1 │    476 │      3 │          0 │    80 │     0 │                 0 │      49 │     3,908 │ ∅            │
-- └───┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴──────────────┘
--
-----------------------------------------------------------------------

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Simpler case
-- Should only need to scan root page (3) plus a single leaf page (4)
--
-- This means that _bt_checkkeys() continuescan handling mustn't get confused
-- about boundary conditions  in the presence of relatively complicated cases,
-- which this is -- multiple so->numArrayKeys is fairly rare.
select * from multi_test where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);

-- Harder case
-- Should only need to scan root page (3) plus a single leaf page (4).  This
-- is a bit trickier for _bt_checkkeys()-adjacent logic.
select * from multi_test where a in (123, 182, 183) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (123, 182, 183) and b in (1,2);

-- Hard case
-- Also needs to scan root page (3) plus leaf page 4 (like "Simpler case").
-- But this time we can't avoid going to a second leaf page -- leaf page 5.
-- That's where matches exceeding (184, -inf) are located.
select * from multi_test where a in (182, 183, 184) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (182, 183, 184) and b in (1,2);

-- Hard luck case
-- Here we gamble and lose.  Almost like earlier skippy_tbl test case, but with
-- multiple SAOP columns for additional test coverage.  And, we only get a
-- single _bt_search because we were "almost correct".
--
-- That is, we descend from the root (3) to leaf page 4, which has matches.
-- Then we gamble by moving right on the leaf level, moving to sibling page 5,
-- which has no matches.  However, page 5 _does_ have a high key that makes us
-- want to move right again, to page 6 -- which is where our final match is
-- found!
select * from multi_test where a in (182, 183, 245) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (182, 183, 245) and b in (1,2); -- 4 buffer hits

-- Harder luck case
-- Two _bt_search descents this time (we _bt_first once we
-- reach page 5 because its high key indicates that it's time to quit gambling)
--
-- XXX UPDATE (December 3) Not anymore, no more moving to right page when our
-- high key lacks an exact match for non-truncated columns.
select * from multi_test where a in (182, 183, 306) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (182, 183, 306) and b in (1,2); -- 4 buffer hits

-- Not-SK_BT_REQFWD-but-still-insertion-scankey case
--
-- This is an example of how insertion scankey can have an attribute/value for
-- "b", even though "b" entry in search-type scankey doesn't end up SK_BT_REQFWD:
-- (_bt_array_continuescan actually encounters this directly, too)
select * from multi_test where a in (3,4,5) and b > 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b > 0;
-- Variant (for good luck)
select * from multi_test where a in (3,4,5) and b >= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b >= 0;
-- This time we make "a" touch a boundary, in the style of "harder case":
select * from multi_test where a in (123, 182, 183) and b > 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (123, 182, 183) and b > 0; -- 2 buffer hits
-- This time we make "a" touch a boundary "inside the high key":
-- XXX UPDATE (February 28): This is 4 index buffer hits due to the
-- restriction on inequalities required in the opposite-to-scan direction
-- only.
--
-- XXX UPDATE (October 16 2024) Behavioral update to
-- required-in-opposite-direction-only scan keys has made this case get only 3
-- buffer hits (this will get 4 buffer hits on Postgres 17, though).
select * from multi_test where a in (123, 182, 183, 184) and b > 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (123, 182, 183, 184) and b > 0; -- 3 buffer hits

-- This time we make "b" search-type scankey required:
--
-- This is an example of the opposite: where an insertion scan key lacks an
-- entry corresponding to a search-type scankey's SK_BT_REQFWD entry.
-- (_bt_array_continuescan actually encounters this directly, too)
select * from multi_test where a in (3,4,5) and b < 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b < 0;
-- Variant (for good luck)
select * from multi_test where a in (3,4,5) and b < 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b < 1;
-- This time we make "a" touch a boundary, in the style of "harder case":
select * from multi_test where a in (123, 182, 183) and b < 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (123, 182, 183) and b < 3; -- 2 buffer hits
-- This time we make "a" touch a boundary "inside the high key":
-- XXX UPDATE (February 28): This is only 3 index buffer hits due to not
-- running afoul of the restriction on inequalities required in the
-- opposite-to-scan direction only (this inequality is required in the scan
-- direction, so we're good) .
select * from multi_test where a in (123, 182, 183, 184) and b < 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (123, 182, 183, 184) and b < 3; -- 3 buffer hits

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Simpler case
-- As above.
select * from multi_test where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);

-- Now as a backwards scan
select * from multi_test where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12)
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12)
order by a desc, b desc;

-- Hard case
-- As above.
select * from multi_test where a in (182, 183, 184) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (182, 183, 184) and b in (1,2);
-- Hard case backwards scan variant (just for coverage):
set enable_sort=off;
select * from multi_test where a in (182, 183, 184) and b in (1,2) order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (182, 183, 184) and b in (1,2) order by a desc, b desc;
set enable_sort=on;

-- Hard luck case
-- As above.
select * from multi_test where a in (182, 183, 245) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (182, 183, 245) and b in (1,2);

-- Harder luck case
-- As above.
select * from multi_test where a in (182, 183, 306) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (182, 183, 306) and b in (1,2);

-- Not-SK_BT_REQFWD-but-still-insertion-scankey case
-- As above.
select * from multi_test where a in (3,4,5) and b > 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b > 0;
-- As a backwards scan:
select * from multi_test where a in (3,4,5) and b > 0
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b > 0
order by a desc, b desc;

-- Variant (for good luck)
select * from multi_test where a in (3,4,5) and b >= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b >= 0;
-- As a backwards scan:
select * from multi_test where a in (3,4,5) and b >= 0
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b >= 0
order by a desc, b desc;

-- This time we make "b" search-type scankey required:
select * from multi_test where a in (3,4,5) and b < 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b < 0;
-- With <= instead of <:
select * from multi_test where a in (3,4,5) and b <= -1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b <= -1;
-- As a backwards scan:
select * from multi_test where a in (3,4,5) and b < 0
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b < 0
order by a desc, b desc;
-- As a backwards scan with <= instead of <:
select * from multi_test where a in (3,4,5) and b <= -1
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b <= -1
order by a desc, b desc;

-- Variant (for good luck)
select * from multi_test where a in (3,4,5) and b < 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b < 1;
-- Variant (for good luck) with <= instead of <
select * from multi_test where a in (3,4,5) and b <= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b <= 0;
-- As a backwards scan:
select * from multi_test where a in (3,4,5) and b < 1
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b < 1
order by a desc, b desc;
-- As a backwards scan with <= instead of <:
select * from multi_test where a in (3,4,5) and b <= 0
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b <= 0
order by a desc, b desc;

-- Index-only scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to on;
set enable_indexscan to off;

-- Simpler case
-- As above.
select * from multi_test where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);

-- Hard case
-- As above.
select * from multi_test where a in (182, 183, 184) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (182, 183, 184) and b in (1,2);

-- Hard luck case
-- As above.
select * from multi_test where a in (182, 183, 245) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (182, 183, 245) and b in (1,2);

-- Harder luck case
-- As above.
select * from multi_test where a in (182, 183, 306) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (182, 183, 306) and b in (1,2);

-- Not-SK_BT_REQFWD-but-still-insertion-scankey case
-- As above.
select * from multi_test where a in (3,4,5) and b > 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b > 0;
-- Variant (for good luck)
select * from multi_test where a in (3,4,5) and b >= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b >= 0;
-- This time we make "b" search-type scankey required:
select * from multi_test where a in (3,4,5) and b < 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b < 0;
-- Variant (for good luck)
select * from multi_test where a in (3,4,5) and b < 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (3,4,5) and b < 1;

-- (November 5) when _bt_preprocess_array_keys has two arrays against the same
-- column that have no intersecting elements, preprocessing will leave the
-- arrays empty.
--
-- This test makes sure that both scan keys are eliminated (not just the
-- second).
select * from multi_test where a in (180,345) and a in (230, 300);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a in (180,345) and a in (230, 300);

-- (February 2) Same again, but this time use bigint arrays -- still should be
-- detected as contradictory within _bt_preprocess_array_keys (not later
-- _bt_preprocess_keys start-of-primscan code paths).
--
-- This doesn't run afoul of any of the implementation restrictions inside
-- _bt_preprocess_array_keys because the array elements themselves are of the
-- same type -- that's all that matters there (the mere presence of cross-type
-- operators does _not_ matter, that's orthogonal).
select * from multi_test where a = any('{180,345}'::bigint[]) and a = any('{230,300}'::bigint[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a = any('{180,345}'::bigint[]) and a = any('{230,300}'::bigint[]);

-- (February 2) Note, however, that this variant cannot use that same
-- _bt_preprocess_array_keys merging/contradictory keys detection.
-- It must use the slightly less efficient handling in _bt_preprocess_keys
-- (works at the level of individual primitive index scans).
--
-- (March 10) UPDATE: Actually, it can, since we now have _bt_preprocess_keys
-- "operate on whole arrays".  This includes having _bt_preprocess_array_keys
-- merge together arrays of different types when necessary.
select * from multi_test where a = any('{180,345}'::bigint[]) and a = any('{230,300}'::integer[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test where a = any('{180,345}'::bigint[]) and a = any('{230,300}'::integer[]);

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- (February 27) This is why the "optimistically go to next leaf page given
-- uncertainty with truncated required scan key attributes" might be worth the
-- complexity:
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test
where
  a = any (array[( select array_agg(i) from a)])
  and b in (0, 1, 2, 3);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 8 buffer hits, not 14
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test
where
  a = any (array[( select array_agg(i) from a)])
  and b in (0, 1, 2, 3);

-- (June 8 2024) Skip scan equivalent of last test case:
select count(*)
from
  multi_test
where
  b in (0, 1, 2, 3);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 8 buffer hits, not 14
select count(*)
from
  multi_test
where
  b in (0, 1, 2, 3);

-- (February 28) This similar case also gets to jump from leaf page to leaf
-- page (a little less compelling than the last example, but it still works).
-- While it involves an inequality, it's required in the same direction as the
-- scan so we're good:
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test
where
  a = any (array[( select array_agg(i) from a)])
  and b < 4;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 8 buffer hits, not 14
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test
where
  a = any (array[( select array_agg(i) from a)])
  and b < 4;

-- (June 8 2024) Skip scan equivalent of last test case:
select count(*)
from
  multi_test
where
  b < 4;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*)
from
  multi_test
where
  b < 4;

-- (February 28) This similar case doesn't get to jump from leaf page to leaf
-- page, even though it's almost the same query as the last one (but I can
-- live with this one).  It's unsupported because it involves an inequality
-- marked required in the opposite direction only (how is _bt_check_compare
-- supposed to even notice this when we arrive on the next page having
-- speculated?):
--
-- XXX UPDATE (October 16 2024) Behavioral update to
-- required-in-opposite-direction-only scan keys has made this case behave
-- similarly to the last one -- now both get 8 buffer hits (this will get 14
-- buffer hits on Postgres 17, though).
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test
where
  a = any (array[( select array_agg(i) from a)])
  and b >= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 8 buffer hits, not 14
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test
where
  a = any (array[( select array_agg(i) from a)])
  and b >= 0;

-- (June 8 2024) Skip scan equivalent of last test case:
select count(*)
from
  multi_test
where
  b >= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*)
from
  multi_test
where
  b >= 0;

-- (March 1) This test determines if we're capable of proving that IS NULL
-- combined with IN() makes for a contradictory qual, which I assumed worked,
-- until realizing today that it never actually did.
--
-- In my defense, this is rather difficult to actually prove.  The planner
-- "helpfully" determines that the qual is contradictory ahead of time,
-- denying nbtree the opportunity to figure it out on its own, and make the
-- entire index scan a no-op.  Here I've had to trick the planner into
-- thinking that that reduction is invalid (it's probably actually still valid
-- with this formulation as written, but it's still good enough to fool the
-- planner and get a usable test case so whatever).
prepare is_null_confusion as
select a, b
from
  multi_test
where
  a = any($1) and a is null;

execute is_null_confusion ('{1,2,3}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
execute is_null_confusion ('{1,2,3}');
deallocate is_null_confusion;

-- Same again, but with IS NOT NULL + NULL array
prepare is_not_null_confusion as
select a, b
from
  multi_test
where
  a = any($1) and a is not null;

execute is_not_null_confusion (NULL);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
execute is_not_null_confusion (NULL);
deallocate is_not_null_confusion;

-- Same again, but with a NULL array alone
prepare null_array_confusion as
select a, b
from
  multi_test
where
  a = any($1);

execute null_array_confusion (NULL);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
execute null_array_confusion (NULL);
deallocate null_array_confusion;

prepare null_array_elements_coverage as
select a, b
from
  multi_test
where
  a = any($1);

-- _bt_preprocess_array_keys deals eliminates NULL array elements up front:
execute null_array_elements_coverage ('{1, NULL, 2}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 2 buffer hits
execute null_array_elements_coverage ('{1, NULL, 2}');

execute null_array_elements_coverage ('{NULL,NULL,NULL, NULL, NULL}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
execute null_array_elements_coverage ('{NULL,NULL,NULL, NULL, NULL}');

deallocate null_array_elements_coverage;

-- (March 9)
--
-- Lots of redundant and contradictory quals involving arrays mixed with
-- simple equality strategy scan keys

-- Simple contradictory
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a = 181;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a = 181;

-- Simple contradictory, but flip order
select *
from multi_test
where
  a = 181
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test
where
  a = 181
  and a in (1, 99, 182, 183, 184);

-- Simple redundant
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a = 182;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '182' (redundant qual)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a = 182;

-- Simple redundant, but flip order
select *
from multi_test
where
  a = 182
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '182' (redundant qual)
select *
from multi_test
where
  a = 182
  and a in (1, 99, 182, 183, 184);

---------------------------------
-- '>' operator/strategy tests --
---------------------------------

-- Simple > contradictory
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a > 184;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a > 184;

-- Simple > contradictory, but flip order
select *
from multi_test
where
  a > 184
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test
where
  a > 184
  and a in (1, 99, 182, 183, 184);

-- Simple > redundant
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a > 183;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '184' (redundant qual)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a > 183;

-- Simple > redundant, but flip order
select *
from multi_test
where
  a > 183
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '184' (redundant qual)
select *
from multi_test
where
  a > 183
  and a in (1, 99, 182, 183, 184);

----------------------------------
-- '>=' operator/strategy tests --
----------------------------------

-- Simple >= contradictory
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a >= 185;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a >= 185;

-- Simple >= contradictory, but flip order
select *
from multi_test
where
  a >= 185
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test
where
  a >= 185
  and a in (1, 99, 182, 183, 184);

-- Simple >= redundant
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a >= 184;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '184' (redundant qual)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a >= 184;

-- Simple >= redundant, but flip order
select *
from multi_test
where
  a >= 184
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '184' (redundant qual)
select *
from multi_test
where
  a >= 184
  and a in (1, 99, 182, 183, 184);

---------------------------------
-- '<' operator/strategy tests --
---------------------------------

-- Simple < contradictory
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a < 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a < 1;

-- Simple < contradictory, but flip order
select *
from multi_test
where
  a < 1
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test
where
  a < 1
  and a in (1, 99, 182, 183, 184);

-- Simple < redundant
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a < 2;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '1' (redundant qual)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a < 2;

-- Simple < redundant, but flip order
select *
from multi_test
where
  a < 2
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '1' (redundant qual)
select *
from multi_test
where
  a < 2
  and a in (1, 99, 182, 183, 184);

----------------------------------
-- '<=' operator/strategy tests --
----------------------------------

-- Simple <= contradictory
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a <= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a <= 0;

-- Simple <= contradictory, but flip order
select *
from multi_test
where
  a <= 0
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test
where
  a <= 0
  and a in (1, 99, 182, 183, 184);

-- Simple <= redundant
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a <= 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '1' (redundant qual)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a <= 1;

-- Simple <= redundant, but flip order
select *
from multi_test
where
  a <= 1
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '1' (redundant qual)
select *
from multi_test
where
  a <= 1
  and a in (1, 99, 182, 183, 184);

-- (March 11) This was a bug that you didn't catch right away when you added
-- code to exclude array entries using another scankey on same att with >
-- strategy (basically an off-by-one thing)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a > 1000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test
where
  a in (1, 99, 182, 183, 184)
  and a > 1000;

-- (March 11) This was another bug that you didn't catch right away,
-- discovered shortly after the one exercised by today's previous test case
select *
from multi_test
where
  a in (1, 99, 182, 184)
  and a < 188;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from multi_test
where
  a in (1, 99, 182, 184)
  and a < 188;

-----------------------------------------------
-- Skip scan parity for backwards scan tests --
-----------------------------------------------
--
-- (Jan 29 2025) Make sure that backwards scan and forward scan full index
-- scans have reasonably (if not exactly) comparable performance
-- characteristics in cases where skip scan is applied but cannot ever really
-- help
--

-- Index-only scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to on;
set enable_indexscan to off;

-- Forward scan:
select *
from multi_test
where b = 1
order by a, b;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from multi_test
where b = 1
order by a, b;

select *
from multi_test
where b < 0
order by a, b;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from multi_test
where b < 0
order by a, b;

-- Backward scan, should match forward scan "buffers" (more or less):
select *
from multi_test
where b = 1
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from multi_test
where b = 1
order by a desc, b desc;

select *
from multi_test
where b < 0
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from multi_test
where b < 0
order by a desc, b desc;

-------------------------------------------------------------------------------
-- tenk1 test cases involving queries where the optimization is inapplicable --
-------------------------------------------------------------------------------
set client_min_messages=error;
drop table if exists tenk1_dyn_saop;
reset client_min_messages;
\getenv abs_srcdir PG_ABS_SRCDIR
CREATE UNLOGGED TABLE tenk1_dyn_saop (
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
ALTER TABLE tenk1_dyn_saop SET (autovacuum_enabled=off);

\set filename :abs_srcdir '/data/tenk.data'
COPY tenk1_dyn_saop FROM :'filename';
CREATE INDEX tenk1_dyn_saop_thous_tenthous ON tenk1_dyn_saop (thousand, tenthous);
VACUUM ANALYZE tenk1_dyn_saop;
-------------------------------------------------------------------------------

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;
prepare regress_tenk1_inequality as
SELECT thousand, tenthous FROM tenk1_dyn_saop
 WHERE thousand < 2 AND tenthous IN (1001,3000)
 ORDER BY thousand;

execute regress_tenk1_inequality;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute regress_tenk1_inequality;
deallocate regress_tenk1_inequality;

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;
prepare regress_tenk1_inequality as
SELECT thousand, tenthous FROM tenk1_dyn_saop
 WHERE thousand < 2 AND tenthous IN (1001,3000)
 ORDER BY thousand;

execute regress_tenk1_inequality;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute regress_tenk1_inequality;
deallocate regress_tenk1_inequality;

-- Index-only scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to on;
set enable_indexscan to off;
prepare regress_tenk1_inequality as
SELECT thousand, tenthous FROM tenk1_dyn_saop
 WHERE thousand < 2 AND tenthous IN (1001,3000)
 ORDER BY thousand;

execute regress_tenk1_inequality;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute regress_tenk1_inequality;
deallocate regress_tenk1_inequality;

-- Now my own backwards scan variant, index-only scan:
prepare regress_tenk1_inequality_backwards as
SELECT thousand, tenthous FROM tenk1_dyn_saop
 WHERE thousand < 2 AND tenthous IN (1001,3000)
 ORDER BY thousand desc, tenthous desc;
execute regress_tenk1_inequality_backwards;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute regress_tenk1_inequality_backwards;
deallocate regress_tenk1_inequality_backwards;

------------------------------------------------------
-- Confusion about wraparound for high order column --
------------------------------------------------------

-- (September 13) This test case demonstrates the need to wraparound the
-- most significant column (which is "thousand" here) so that the scan will
-- terminate on the leftmost leaf page, without needlessly accessing further
-- leaf pages to the right.
--
-- This is surprisingly subtle, and seems like it's the only test case that'll
-- catch this.  Note that what I describe is independent of the issue covered
-- by the next test case (nonarray_equality_strategy_orderproc_required_both_stages)
-- which was all about not doing the required comparisons in both functions.
-- This is about __not__ resetting cur_elem to zero for the "thousand" array once
-- the scan gets past the last "thousand = 1" tuple.
--
-- (October 28) We want to not only get the expected number of buffer hits; we
-- also want to terminate the scan within even incrementally advancing the
-- array keys.  More concretely, it should look like this (and does, at the
-- time of writing):
--
-- (November 10): See also, must_wraparound_high_order_column_equality_nomatch
--
-- _bt_advance_array_keys, tuple: (thousand, tenthous)=(2, 2), 0x7ff88b087ea0   <-- first (2, *) tuple
--   numberOfKeys: 2
--  - sk_attno: 1, cur_elem 1/1, val: 1 [NULLS LAST, ASC]
--  - sk_attno: 2, cur_elem 9001/20500, val: 9001 [NULLS LAST, ASC]
--  + sk_attno: 1, cur_elem 1/1, val: 1 [NULLS LAST, ASC]              <--- No changes here
--  + sk_attno: 2, cur_elem 9001/20500, val: 9001 [NULLS LAST, ASC]    <--- Nor here
--  _bt_advance_array_keys: returns false
-- _bt_readpage final: (thousand, tenthous)=(2, 2), 0x7ff88b087ea0, from non-pivot offnum 22 TID (93,20) ended page and scan
-- _bt_readpage stats: currPos.firstItem: 0, currPos.lastItem: 19, nmatching: 20 ✅
-- _bt_first: returning offnum 2 TID (344,23)
-- _bt_readnextpage: ScanDirectionIsForward() case ran out of pages to the right
-- _bt_readnextpage: BTScanPosInvalidate() called for currPos
-- _bt_steppage: _bt_readnextpage() returns false so we do too
-- btendscan

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;
prepare must_wraparound_high_order_column as
with a as (
  select i from generate_series(0, 10500) i
)
select thousand, tenthous
from
  tenk1_dyn_saop
where thousand in (0, 1) and
tenthous = any (array[(select array_agg(i) from a)]);

execute must_wraparound_high_order_column;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute must_wraparound_high_order_column;
deallocate must_wraparound_high_order_column;

-- Same again, but backwards scan for good luck
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

prepare must_wraparound_high_order_column_desc as
with a as (
  select i from generate_series(0, 10500) i
)
select thousand, tenthous
from
  tenk1_dyn_saop
where thousand in (0, 1) and
tenthous = any (array[(select array_agg(i) from a)])
order by thousand desc, tenthous desc;

execute must_wraparound_high_order_column_desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute must_wraparound_high_order_column_desc;
deallocate must_wraparound_high_order_column_desc;

-- (September 12) This test case decisively proves that we need to use
-- non-array required BTEqualStrategyNumber scan keys, both in the
-- precheck-current-keys function, and the function that actually advances the
-- array keys using tuple values.
--
-- For a while the test would fail (we'd do useless extra leaf page visits)
-- because I lacked the required infrastructure in at least one of these two
-- functions.  This had surprisingly little (no?) coverage before then.  This
-- test case makes it really obvious.
-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;
prepare nonarray_equality_strategy_orderproc_required_both_stages as
with a as (
  select i from generate_series(-1, 10000) i
)
select
  thousand,
  tenthous
from
  tenk1_dyn_saop
where thousand = 1 and tenthous = any (array[(select array_agg(i) from a)]);

execute nonarray_equality_strategy_orderproc_required_both_stages;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute nonarray_equality_strategy_orderproc_required_both_stages;
deallocate nonarray_equality_strategy_orderproc_required_both_stages;

-- (June 8 2024) Skip scan equivalent of last test case:
prepare skip_nonarray_equality_strategy as
with a as (
  select i from generate_series(-1, 10000) i
)
select
  thousand,
  tenthous
from
  tenk1_dyn_saop
where thousand <= 1 and tenthous = any (array[(select array_agg(i) from a)]);

execute skip_nonarray_equality_strategy;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute skip_nonarray_equality_strategy;
deallocate skip_nonarray_equality_strategy;

-- Same again, but this time use a plain index scan for good luck:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;
prepare nonarray_equality_strategy_orderproc_required_both_stages as
with a as (
  select i from generate_series(-1, 10000) i
)
select
  thousand,
  tenthous
from
  tenk1_dyn_saop
where thousand = 1 and tenthous = any (array[(select array_agg(i) from a)]);

execute nonarray_equality_strategy_orderproc_required_both_stages;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute nonarray_equality_strategy_orderproc_required_both_stages;
deallocate nonarray_equality_strategy_orderproc_required_both_stages;

-- Same again, but this time use a backwards scan for good luck:
prepare nonarray_equality_strategy_orderproc_required_both_stages_desc as
with a as (
  select i from generate_series(-1, 10000) i
)
select
  thousand,
  tenthous
from
  tenk1_dyn_saop
where thousand = 1 and tenthous = any (array[(select array_agg(i) from a)])
order by thousand desc, tenthous desc;
execute nonarray_equality_strategy_orderproc_required_both_stages_desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute nonarray_equality_strategy_orderproc_required_both_stages_desc;

-- (June 8 2024) Skip scan equivalent of last test case:
prepare skip_nonarray_equality_strategy_orderproc_desc as
with a as (
  select i from generate_series(-1, 10000) i
)
select
  thousand,
  tenthous
from
  tenk1_dyn_saop
where thousand <= 1 and tenthous = any (array[(select array_agg(i) from a)])
order by thousand desc, tenthous desc;
execute skip_nonarray_equality_strategy_orderproc_desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute skip_nonarray_equality_strategy_orderproc_desc;

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- (September 19) But what about inequalities?
-- This is almost the same query as the last one, except we use < as a
-- replacement for =.  We should get the same number of buffer hits.
prepare nonarray_inequality as
with a as (
  select i from generate_series(-1, 10000) i
)
select
  thousand,
  tenthous
from
  tenk1_dyn_saop
where thousand < 2 and tenthous = any (array[(select array_agg(i) from a)]);

execute nonarray_inequality;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 2 buffer hits, just like original = query
execute nonarray_inequality;
deallocate nonarray_inequality;

-- Same again, but this time a backwards scan for good luck:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;
prepare nonarray_inequality_desc as
with a as (
  select i from generate_series(-1, 10000) i
)
select
  thousand,
  tenthous
from
  tenk1_dyn_saop
where thousand < 2 and tenthous = any (array[(select array_agg(i) from a)])
order by thousand desc, tenthous desc;

execute nonarray_inequality_desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 2 buffer hits, just like original = query
execute nonarray_inequality_desc;
deallocate nonarray_inequality_desc;

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- (November 10) This is like must_wraparound_high_order_column, except that
-- it has a new stressor for the implementation: the tenthous values the we'll
-- attempt to match are non-matches -- they're all too high to get any
-- matches.
prepare must_wraparound_high_order_column_equality_nomatch as
with a as (
  select i from generate_series(10000, 10500) i
)
select
  thousand,
  tenthous
from tenk1_dyn_saop
where thousand in (0, 1) and tenthous = any (array[(select array_agg(i) from a)]);

execute must_wraparound_high_order_column_equality_nomatch;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute must_wraparound_high_order_column_equality_nomatch;
deallocate must_wraparound_high_order_column_equality_nomatch;

-- (November 10) This is like
-- must_wraparound_high_order_column_equality_nomatch, except that it uses an
-- inequality that's "equivalent" to its first array -- which renders the
-- second array on tenthous non-required.  We expect identical access patterns
-- for both test cases (at least at the level of whole pages scanned).
--
-- This caused the postcondition assertion within _bt_advance_array_keys to
-- fail thusly:
--
-- TRAP: failed Assert("_bt_tuple_before_array_skeys(scan, pstate, tuple) == (!all_required_eqtype_sk_equal && !arrays_exhausted)")
--
-- At the time of writing, the "fallback on incremental advancement" path has
-- a design that doesn't properly account for non-required arrays.
prepare must_wraparound_high_order_column_inequality_nomatch as
with a as (
  select i from generate_series(10000, 10500) i
)
select
  thousand,
  tenthous
from tenk1_dyn_saop
where thousand < 2 and tenthous = any (array[( select array_agg(i) from a)]);

execute must_wraparound_high_order_column_inequality_nomatch;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute must_wraparound_high_order_column_inequality_nomatch;
deallocate must_wraparound_high_order_column_inequality_nomatch;

-- Microbenchmarks

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Low cardinality
CREATE INDEX tenk1_dyn_saop_idx_lowcard ON tenk1_dyn_saop (two, four, twenty, hundred);

-- Limit 10:
select ctid, * from tenk1_dyn_saop
where
  two in (0, 1)
  and four in (0, 1, 2)
  and twenty in (0, 1, 3)
  and hundred in (0, 1, 5)
order by
  two,
  four,
  twenty,
  hundred
limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, * from tenk1_dyn_saop
where
  two in (0, 1)
  and four in (0, 1, 2)
  and twenty in (0, 1, 3)
  and hundred in (0, 1, 5)
order by
  two,
  four,
  twenty,
  hundred
limit 10;

---------------------------------
-- non-SK_BT_REQFWD test cases --
---------------------------------

-- Index-only scan:
VACUUM (freeze,analyze) tenk1_dyn_saop;
set enable_indexonlyscan to on;

-- Four is omitted here:
prepare four_omitted as
select
  count(*),
  two,
  twenty
from
  tenk1_dyn_saop
where
  two = 0
  and twenty in (9, 10)
group by
  two,
  twenty;

execute four_omitted;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute four_omitted;
deallocate four_omitted;

-- tenk1_idx_extra_column_in_middle puzzle #1
--
-- Tests non-SK_BT_REQFWD array scan keys.  There is a "gap" in the columns
-- represented here.

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

create index tenk1_idx_extra_column_in_middle on tenk1_dyn_saop(two,four,twenty);
select
  count(*)
from
  tenk1_dyn_saop
where
  two in (0, 1) -- i.e., every possible "two" value
  and twenty in (0, 1);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  count(*)
from
  tenk1_dyn_saop
where
  two in (0, 1) -- i.e., every possible "two" value
  and twenty in (0, 1);

-- tenk1_idx_extra_column_in_middle puzzle #1.1
--
-- "four is not null" isn't like "four is null" in that it renders lower order
-- columns non-SK_BT_REQFWD.
select
  count(*)
from
  tenk1_dyn_saop
where
  two in (0, 1) -- i.e., every possible "two" value
  and four is not null -- no value in "four" is ever a NULL
  and twenty in (0, 1);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  count(*)
from
  tenk1_dyn_saop
where
  two in (0, 1) -- i.e., every possible "two" value
  and four is not null -- no value in "four" is ever a NULL
  and twenty in (0, 1);

-- tenk1_idx_extra_column_in_middle puzzle #1.2
--
-- What's at issue here is whether or not ">= any (array[1, 2])" should count as
-- an equality constraint
--
-- ">= any (array[1, 2])" is an SAOP that it executed by getting an extreme
-- element once, during preprocessing.  This mustn't be confused for the SAOPs
-- we care about.  It also renders lower order columns non-SK_BT_REQFWD, which
-- we must look out for for the usual reasons.
select
  count(*)
from
  tenk1_dyn_saop
where
  two in (0, 1) -- i.e., every possible "two" value
  and four >= any (array[1, 2]) -- ScalarArrayOpExr inequality
  and twenty in (0, 1);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  count(*)
from
  tenk1_dyn_saop
where
  two in (0, 1) -- i.e., every possible "two" value
  and four >= any (array[1, 2]) -- ScalarArrayOpExr inequality
  and twenty in (0, 1);

-- tenk1_idx_extra_column_in_middle puzzle #2
--
-- (August 29) This variant of puzzle #1 was interesting back in July.  I'm
-- keeping it now out of paranoia.
select
  count(*)
from
  tenk1_dyn_saop
where
  two in (0, 1) -- i.e., every possible "two" value
  and twenty in (0, 1, 2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select
  count(*)
from
  tenk1_dyn_saop
where
  two in (0, 1) -- i.e., every possible "two" value
  and twenty in (0, 1, 2);

-- tenk1_idx_extra_column_in_middle puzzle #3
--
-- (August 29) This variant of puzzle #1 was interesting back in July.  I'm
-- keeping it now out of paranoia.
create index tenk1_dyn_saop_idx_many_columns on tenk1_dyn_saop (two,four,twenty,unique1,hundred);
select ctid, *
from
  tenk1_dyn_saop
where
  two in (3, 5)
  and twenty in (0, 1, 3)
  and hundred in (0, 1, 5)
order by
  two,
  four,
  twenty
limit 15;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 2 buffer accesses (4 on master)
select ctid, *
from
  tenk1_dyn_saop
where
  two in (3, 5)
  and twenty in (0, 1, 3)
  and hundred in (0, 1, 5)
order by
  two,
  four,
  twenty
limit 15;

-- RhodiumToad test query from https://www.postgresql.org/message-id/flat/87egxzbn01.fsf%40news-spur.riddles.org.uk
--
-- RhodiumToad test #1
--
-- First let's see how it does with the existing tenk1_dyn_saop_thous_tenthous
-- index.
--
-- Patch doesn't do all that much better than master here (126 buffer hits vs
-- 144), and yet if you give the master branch a choice between
-- tenk1_dyn_saop_thous_tenthous and the rhodium_toad index, it'll prefer to
-- use the latter one -- which actually works out to be about 3x more
-- expensive, buffer-hits-wise.
select * from tenk1_dyn_saop
where
  thousand in (19, 29, 39, 49, 57, 66, 77, 8, 90, 12, 22, 32)
  and (ten >= 5) and (ten > 5 or unique1 > 5000)
order by ten, unique1 limit 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from tenk1_dyn_saop
where
  thousand in (19, 29, 39, 49, 57, 66, 77, 8, 90, 12, 22, 32)
  and (ten >= 5) and (ten > 5 or unique1 > 5000)
order by ten, unique1 limit 1;

-- RhodiumToad test #2
--
-- Same query, but now we have the rhodium_toad index available, which makes
-- things significantly less efficient for master (1009 buffers hit), and
-- significantly more efficient for patch (7 buffers hit):
drop index tenk1_dyn_saop_thous_tenthous; -- have to force patch here
create index rhodium_toad on tenk1_dyn_saop(ten, unique1, thousand);
select * from tenk1_dyn_saop
where
  thousand in (19, 29, 39, 49, 57, 66, 77, 8, 90, 12, 22, 32)
  and (ten >= 5) and (ten > 5 or unique1 > 5000)
order by ten, unique1 limit 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from tenk1_dyn_saop
where
  thousand in (19, 29, 39, 49, 57, 66, 77, 8, 90, 12, 22, 32)
  and (ten >= 5) and (ten > 5 or unique1 > 5000)
order by ten, unique1 limit 1;

-- Nice demo of importance of work in context of ORDER BY ... LIMIT
-- Only 13 buffer hits on patch...but 1337 buffer hits on master!
select ctid, two, four, twenty from tenk1_dyn_saop
where
  two in (0, 1) and four in (1, 2) and twenty in (1, 2)
order by two, four, twenty limit 20;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, two, four, twenty from tenk1_dyn_saop
where
  two in (0, 1) and four in (1, 2) and twenty in (1, 2)
order by two, four, twenty limit 20;

-- (March 12)
--
-- This case looks like it has lots of arrays (one on each of the four index
-- columns), but preprocessing manages to completely avoid ever needing a call
-- to _bt_advance_array_keys at runtime -- there are no array scan keys left
-- to advance once preprocessing completes.
--
-- So looks like this:
/*
so->numArrayKeys is 4 after initial array preprocessing
_bt_preprocess_keys:  inkeys[0]: [ strategy: = , flags: [SK_SEARCHARRAY], attno: 1 func OID: 65 ]
_bt_preprocess_keys:  inkeys[1]: [ strategy: InvalidStrategy, flags: [SK_SEARCHARRAY], attno: 1 func OID: 65 ]
_bt_preprocess_keys:  inkeys[2]: [ strategy: = , flags: [SK_SEARCHARRAY], attno: 2 func OID: 65 ]
_bt_preprocess_keys:  inkeys[3]: [ strategy: InvalidStrategy, flags: [SK_SEARCHARRAY], attno: 2 func OID: 65 ]
_bt_preprocess_keys:  inkeys[4]: [ strategy: = , flags: [SK_SEARCHARRAY], attno: 3 func OID: 65 ]
_bt_preprocess_keys:  inkeys[5]: [ strategy: > , flags: [], attno: 3 func OID: 147 ]
_bt_preprocess_keys:  inkeys[6]: [ strategy: = , flags: [SK_SEARCHARRAY], attno: 4 func OID: 65 ]
_bt_preprocess_keys:  inkeys[7]: [ strategy: < , flags: [], attno: 4 func OID: 66 ]
   _bt_binsrch_array_skey: searching for item 12, low_elem 0, high_elem 12, num_elems: 13
                           found item 12 at elem offset 11
   _bt_binsrch_array_skey: searching for item 94, low_elem 0, high_elem 3, num_elems: 4
                           found item 94 at elem offset 1
_bt_preprocess_keys: outkeys[0]: [ strategy: = , flags: [SK_BT_REQFWD, SK_BT_REQBKWD], attno: 1 func OID: 65 ]  <-- no trace of the SK_SEARCHARRAY markings here
_bt_preprocess_keys: outkeys[1]: [ strategy: = , flags: [SK_BT_REQFWD, SK_BT_REQBKWD], attno: 2 func OID: 65 ]  <-////
_bt_preprocess_keys: outkeys[2]: [ strategy: = , flags: [SK_BT_REQFWD, SK_BT_REQBKWD], attno: 3 func OID: 65 ]  <-///
_bt_preprocess_keys: outkeys[3]: [ strategy: = , flags: [SK_BT_REQFWD, SK_BT_REQBKWD], attno: 4 func OID: 65 ]  <-//
_bt_preprocess_keys: scan->numberOfKeys is 8, so->numberOfKeys on output is 4, so->numArrayKeys on output is 0  <-/
*/
select ctid, two, four, twenty, hundred
from tenk1_dyn_saop
where
  two in (0, 1) and two in (1, 3)
  and
  four in (0, 1) and four in (1,2)
  and
  twenty in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10,11,12,13) and twenty > 12
  and
  hundred in (93, 94, 95, 96) and hundred < 94
order by two, four, twenty;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, two, four, twenty, hundred
from tenk1_dyn_saop
where
  two in (0, 1) and two in (1, 3)
  and
  four in (0, 1) and four in (1,2)
  and
  twenty in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10,11,12,13) and twenty > 12
  and
  hundred in (93, 94, 95, 96) and hundred < 94
order by two, four, twenty;

VACUUM ANALYZE tenk1_dyn_saop;
set enable_indexonlyscan to on;
set enable_sort to off;

-------------------------------------------------------------
-- Pathological case with a huge number of array constants --
-------------------------------------------------------------

-- This stresses "binary search for array keys" logic, verifying that we never
-- do very much work under a buffer lock.  Needed in mid to late August.

prepare binsearch_stress_forward as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
group by
  two, four, twenty
order by
  two, four, twenty;

prepare binsearch_stress_backwards as
with a as (
  select i from generate_series(-1, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
group by
  two, four, twenty
order by
  two desc, four desc, twenty desc;

-- Forward and backwards variants both tested:
execute binsearch_stress_forward;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Sept 28 2024, disable for test stability
-- execute binsearch_stress_forward;
execute binsearch_stress_backwards;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Ditto
-- execute binsearch_stress_backwards;

-- (February 20) Stress test with large arrays and contradictory quals

-- This doesn't make the query have only one call to _bt_preprocess_keys, but
-- it does reduce the number to 5000 (since that's the number of distinct
-- "two" values, and "two" is the most significant column here):
prepare binsearch_stress_contradictory_two_equal as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
  from
  tenk1_dyn_saop
  where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)]) and
-- Avoid redundancy detection in planner and in _bt_preprocess_array_keys by using
-- cross-type contradictory qual:
  two = 500000::int8 and two = 600000::int4
group by
  two, four, twenty
order by
  two, four, twenty;

prepare binsearch_stress_contradictory_two_inequal_first as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
  and two >= 5 and two = 4
group by
  two, four, twenty
order by
  two, four, twenty;

prepare binsearch_stress_contradictory_two_inequal_second as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two >= 5 and two = 4 and
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
group by
  two, four, twenty
order by
  two, four, twenty;

prepare binsearch_stress_contradictory_two_inequal_third as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two >= 5 and
  two = any (array[(select array_agg(i) from a)]) and
  two = 4 and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
group by
  two, four, twenty
order by
  two, four, twenty;

prepare binsearch_stress_contradictory_two_inequal_fourth as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = 4 and
  two = any (array[(select array_agg(i) from a)]) and
  two >= 5 and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
group by
  two, four, twenty
order by
  two, four, twenty;

-- This doesn't make the query have only one call to _bt_preprocess_keys, and
-- since "twenty" is the lowest order column it isn't very effective at all:
prepare binsearch_stress_contradictory_twenty_equal as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)]) and
-- Avoid redundancy detection in planner and in _bt_preprocess_array_keys by using
-- cross-type contradictory qual:
  twenty = 500000::int8 and twenty = 600000::int4
group by
  two, four, twenty
order by
  two, four, twenty;

-- This is essentially the same issue, except that it works perfectly (only
-- one call to _bt_preprocess_keys for whole top-level query) due to the fact
-- that the contradictory qual doesn't involve an array at all:
prepare binsearch_stress_contradictory_unique1_equal as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)]) and
-- Avoid redundancy detection in planner and in _bt_preprocess_array_keys by using
-- cross-type contradictory qual:
  unique1 = 500000::int8 and unique1 = 600000::int4
group by
  two, four, twenty
order by
  two, four, twenty;

prepare binsearch_stress_contradictory_twenty_inequal_first as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
  and twenty >= 5 and twenty = 4
group by
  two, four, twenty
order by
  two, four, twenty;

prepare binsearch_stress_contradictory_twenty_inequal_second as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  twenty >= 5 and twenty = 4 and
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
group by
  two, four, twenty
order by
  two, four, twenty;

prepare binsearch_stress_contradictory_twenty_inequal_third as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty >= 5 and
  twenty = any (array[(select array_agg(i) from a)]) and
  twenty = 4
group by
  two, four, twenty
order by
  two, four, twenty;

prepare binsearch_stress_contradictory_twenty_inequal_fourth as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = 4 and
  twenty = any (array[(select array_agg(i) from a)]) and
  twenty >= 5
group by
  two, four, twenty
order by
  two, four, twenty;

prepare binsearch_stress_contradictory_twenty_inequal_fifth as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)]) and
  twenty > 5000
group by
  two, four, twenty
order by
  two, four, twenty;

-- (XXX UPDATE June 18 2024) Disable these flappy tests, which haven't caught
-- a bug during skip scan project
--
-- execute binsearch_stress_contradictory_two_equal;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
-- execute binsearch_stress_contradictory_two_equal;

execute binsearch_stress_contradictory_two_inequal_first;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Sept 28 2024, disable for test stability
-- execute binsearch_stress_contradictory_two_inequal_first;
execute binsearch_stress_contradictory_two_inequal_second;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Sept 28 2024, disable for test stability
-- execute binsearch_stress_contradictory_two_inequal_second;
execute binsearch_stress_contradictory_two_inequal_third;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)    <-- Ditto
-- execute binsearch_stress_contradictory_two_inequal_third;
execute binsearch_stress_contradictory_two_inequal_fourth;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)    <-- Ditto
-- execute binsearch_stress_contradictory_two_inequal_fourth;

-- Don't execute problematic tests on lower-order "twenty" column, at least
-- for now (this takes far too long, times out):
--
-- (February 29) UPDATE:  Now we do these tests, too
--
-- (XXX UPDATE June 18 2024) Disable these flappy tests, which haven't caught
-- a bug during skip scan project
--
-- execute binsearch_stress_contradictory_twenty_equal;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
-- execute binsearch_stress_contradictory_twenty_equal;

-- Note, however, that this very similar variant of the problematic query
-- works just fine, all because the contradictory qual thing doesn't happen
-- to involve an array this time (though it does involve a very low-order
-- column, this time unique1 instead of "twenty"):
--
-- (XXX UPDATE June 18 2024) Disable these flappy tests, which haven't caught
-- a bug during skip scan project
-- execute binsearch_stress_contradictory_unique1_equal;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
-- execute binsearch_stress_contradictory_unique1_equal;

-- (February 29) UPDATE:  Now we start running these tests, too
execute binsearch_stress_contradictory_twenty_inequal_first;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Sept 28 2024, disable for test stability
-- execute binsearch_stress_contradictory_twenty_inequal_first;
execute binsearch_stress_contradictory_twenty_inequal_second;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Ditto
-- execute binsearch_stress_contradictory_twenty_inequal_second;
execute binsearch_stress_contradictory_twenty_inequal_third;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Ditto
-- execute binsearch_stress_contradictory_twenty_inequal_third;
execute binsearch_stress_contradictory_twenty_inequal_fourth;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Ditto
-- execute binsearch_stress_contradictory_twenty_inequal_fourth;
execute binsearch_stress_contradictory_twenty_inequal_fifth;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Ditto
-- execute binsearch_stress_contradictory_twenty_inequal_fifth;

deallocate binsearch_stress_contradictory_two_equal;

deallocate binsearch_stress_contradictory_two_inequal_first;
deallocate binsearch_stress_contradictory_two_inequal_second;
deallocate binsearch_stress_contradictory_two_inequal_third;
deallocate binsearch_stress_contradictory_two_inequal_fourth;

deallocate binsearch_stress_contradictory_twenty_equal;

deallocate binsearch_stress_contradictory_twenty_inequal_first;
deallocate binsearch_stress_contradictory_twenty_inequal_second;
deallocate binsearch_stress_contradictory_twenty_inequal_third;
deallocate binsearch_stress_contradictory_twenty_inequal_fourth;

-- Same again, but with extra values at the end of the key space to stress the
-- implementation:
insert into tenk1_dyn_saop select unique1, unique2, 4999, four, ten, twenty, hundred, thousand, twothousand, fivethous, tenthous, odd, even, stringu1, stringu2, string4 from tenk1_dyn_saop limit 1 offset 0;
insert into tenk1_dyn_saop select unique1, unique2, 5000, four, ten, twenty, hundred, thousand, twothousand, fivethous, tenthous, odd, even, stringu1, stringu2, string4 from tenk1_dyn_saop limit 1 offset 1;
insert into tenk1_dyn_saop select unique1, unique2, 5001, four, ten, twenty, hundred, thousand, twothousand, fivethous, tenthous, odd, even, stringu1, stringu2, string4 from tenk1_dyn_saop limit 1 offset 2;
insert into tenk1_dyn_saop select unique1, unique2,   -5, four, ten, twenty, hundred, thousand, twothousand, fivethous, tenthous, odd, even, stringu1, stringu2, string4 from tenk1_dyn_saop limit 1 offset 3;

-- Forward and backwards variants both tested:
execute binsearch_stress_forward;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Sept 28 2024, disable for test stability
-- execute binsearch_stress_forward;
execute binsearch_stress_backwards;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Ditto
-- execute binsearch_stress_backwards;

deallocate binsearch_stress_forward;
deallocate binsearch_stress_backwards;

-- Minor variant (500 rather than 5000) can independently break:
prepare binsearch_stress_variant as
with a as (
  select i from generate_series(0, 500) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
group by
  two, four, twenty
order by
  two, four, twenty;
execute binsearch_stress_variant;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Sept 28 2024, disable for test stability
-- execute binsearch_stress_variant;

deallocate binsearch_stress_variant;

-- Minor variant (500 rather than 5000) with a backwards scan, just for good luck:
prepare binsearch_stress_variant_backwards as
with a as (
  select i from generate_series(0, 500) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
group by
  two, four, twenty
order by
  two desc, four desc, twenty desc;
execute binsearch_stress_variant_backwards;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Sept 28 2024, disable for test stability
-- execute binsearch_stress_variant_backwards;

deallocate binsearch_stress_variant_backwards;

-- Another variant (4999 rather than 5000) can independently break:
prepare binsearch_stress_other_variant as
with a as (
  select i from generate_series(0, 4999) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
group by
  two, four, twenty
order by
  two, four, twenty;
execute binsearch_stress_other_variant;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Sept 28 2024, disable for test stability
-- execute binsearch_stress_other_variant;

deallocate binsearch_stress_other_variant;

-- Yet another variant (4666 rather than 5000 -- no high matches returned) might be able to independently
-- break, so be careful and include coverage for that case too:
prepare binsearch_stress_yav as
with a as (
  select i from generate_series(0, 4666) i
)
select
  count(*), two, four, twenty
from
  tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
group by
  two, four, twenty
order by
  two, four, twenty;
execute binsearch_stress_yav;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- <-- Sept 28 2024, disable for test stability
-- execute binsearch_stress_yav;

deallocate binsearch_stress_yav;


-----------------------------
-- Empty index stress test --
-----------------------------

-- (September 12) This test shows the importance of avoiding becoming confused
-- when we fail to reach _bt_readpage due to not having any pages in the index
-- (but having some array keys)
set client_min_messages=error;
drop table if exists empty_tenk1_dyn_saop;
reset client_min_messages;
create unlogged table empty_tenk1_dyn_saop
(
  like tenk1_dyn_saop including indexes
);

prepare empty_table_stress_test as
with a as (
  select i from generate_series(0, 5000) i
)
select
  count(*), two, four, twenty
from
  empty_tenk1_dyn_saop
where
  two = any (array[(select array_agg(i) from a)]) and
  four = any (array[(select array_agg(i) from a)]) and
  twenty = any (array[(select array_agg(i) from a)])
group by
  two, four, twenty
order by
  two, four, twenty;
execute empty_table_stress_test;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
-- execute empty_table_stress_test;

-- Here it's important not to charge too much for a ludicrously high number of
-- descents of the index that exceeds what is possible with the patch:
--
-- July 21: Same example as the one shown to Tomas on-list today.
--
-- Index-only scan to make this realistic/compelling:
VACUUM (freeze,analyze) tenk1_dyn_saop;
set enable_indexonlyscan to on;

prepare tomas_demo as
select count(*), two, four, twenty from tenk1_dyn_saop
where
  two in (0, 1)
  and four in (1, 2, 3, 4)
  and twenty in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)
group by
  two,
  four,
  twenty
order by
  two,
  four,
  twenty;

execute tomas_demo;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 10 buffer hits
execute tomas_demo;

-- (August 26)
-- Same query, but we must force the use of tenk1_dyn_saop_idx_lowcard, which
-- failed in an independently interesting way alongside the prior query during
-- work on binary search for next key:
drop index tenk1_dyn_saop_idx_many_columns;
drop index tenk1_idx_extra_column_in_middle;

-- Same query with other index:
execute tomas_demo;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 9 buffer hits
execute tomas_demo;
deallocate tomas_demo;

-- (August 26)
-- Recreate temporarily dropped tenk1_idx_extra_column_in_middle index from
-- before:
create index tenk1_idx_extra_column_in_middle on tenk1_dyn_saop(two,four,twenty);

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Same trick should work with leading attribute a non-SAOP:
select ctid, thousand from tenk1_dyn_saop
where
  two = 0 and four in (1, 2) and twenty in (1, 2)
order by two, four, twenty limit 20;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, thousand from tenk1_dyn_saop
where
  two = 0 and four in (1, 2) and twenty in (1, 2)
order by two, four, twenty limit 20;

-- Same trick should work with middle attribute a non-SAOP:
select ctid, thousand from tenk1_dyn_saop
where
  two in (0, 1) and four = 1 and twenty in (1, 2)
order by two, four, twenty limit 20;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, thousand from tenk1_dyn_saop
where
  two in (0, 1) and four = 1 and twenty in (1, 2)
order by two, four, twenty limit 20;

-- Even an inequality on two should work, since we can get through non-matches
-- from the index quickly.
--
-- Even with a leading inequality ("two !=0"), the patch does almost 10x fewer
-- buffer accesses than the master branch will.
select ctid, thousand from tenk1_dyn_saop
where
  two != 0 and four in (1, 2) and twenty in (1, 2)
order by two, four, twenty limit 20;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, thousand from tenk1_dyn_saop
where
  two != 0 and four in (1, 2) and twenty in (1, 2)
order by two, four, twenty limit 20;

-- Same trick should work with interlaced SOAPs and non-SAOPS:
drop index tenk1_idx_extra_column_in_middle;
-- First variant:
select ctid, two, four, twenty, hundred
  from tenk1_dyn_saop
where
  two in (0, 1) and four = 1 and twenty in (0, 1) and hundred = 1
order by two, four, twenty, hundred limit 20;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, two, four, twenty, hundred
  from tenk1_dyn_saop
where
  two in (0, 1) and four = 1 and twenty in (0, 1) and hundred = 1
order by two, four, twenty, hundred limit 20;
-- Second variant:
select ctid, two, four, twenty, hundred
  from tenk1_dyn_saop
where
  two = 1 and four in (1, 2) and twenty = 1 and hundred in (0, 1)
order by two, four, twenty, hundred limit 20;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, two, four, twenty, hundred
  from tenk1_dyn_saop
where
  two = 1 and four in (1, 2) and twenty = 1 and hundred in (0, 1)
order by two, four, twenty, hundred limit 20;

-- (November 8)
--
-- Provides backwards scan "We're still setting the keys to values >= the
-- tuple here -- it just needs to work for the tuple as a whole." case
-- coverage.
--
-- This hits "hundred" column with tuple "(two, four, twenty, hundred)=(0, 0, 16, 96)".
-- This tuple is the first tuple seen by the first _bt_readpage call (its
-- offnum actually came from _bt_first's _bt_binsrch call).
--
-- Seems like this needs to make sure that we reset the cur_elem for "hundred"
-- to the first element for the current scan direction.  So this transition is
-- important:
--
-- - sk_attno: 1, cur_elem 0/0, val: 0 [NULLS LAST, ASC]
-- - sk_attno: 2, cur_elem 1/1, val: 1 [NULLS LAST, ASC]
-- - sk_attno: 3, cur_elem 0/0, val: 15 [NULLS LAST, ASC]
-- - sk_attno: 4, cur_elem 1/1, val: 1000 [NULLS LAST, ASC]
--
-- + sk_attno: 1, cur_elem 0/0, val: 0 [NULLS LAST, ASC]
-- + sk_attno: 2, cur_elem 0/1, val: 0 [NULLS LAST, ASC]    <-- advances (from 1 to 0), so we're "matching the tuple" now
-- + sk_attno: 3, cur_elem 0/0, val: 15 [NULLS LAST, ASC]   <-- now we're "ahead of the tuple", since its value for this is 16
--                                                              (though cannot advance array because it has only one element)
-- + sk_attno: 4, cur_elem 1/1, val: 1000 [NULLS LAST, ASC] <-- this one had better remain at the first element for scan direction
--                                                              (and not spuriously "advance" to 96 value from tuple/array, for example)
--
-- Not completely clear whether or not the handling for this case is strictly
-- necessary.  After all, sk_attno 4/"hundred" doesn't actually need to
-- change at all here.  Still seems practically mandatory to have this
-- handling, since it is likely almost impossible to prove this either way.
-- (Besides, we definitely need similar logic for forwards scans already, and having symmetry is no bad thing).
--
-- (February 4) UPDATE: Actually, next test case (written today) decisively
-- proves that proper symmetric handling of this case during backward scans is
-- strictly necessary (as suspected all along).
select *
from tenk1_dyn_saop
where
  two = any ('{0}') and four = any ('{0, 1}') and twenty = any ('{15}') and hundred = any ('{96,1000}')
order by
  two desc, four desc, twenty desc, hundred desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from tenk1_dyn_saop
where
  two = any ('{0}') and four = any ('{0, 1}') and twenty = any ('{15}') and hundred = any ('{96,1000}')
order by
  two desc, four desc, twenty desc, hundred desc;

-- (March 2) Probably useless variant of the last test case of
-- February 4:
select *
from tenk1_dyn_saop
where
  two = any ('{0}') and four = any ('{0, 1}') and twenty = any ('{15}') and hundred = any ('{-1, 95}')
order by
  two desc, four desc, twenty desc, hundred desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from tenk1_dyn_saop
where
  two = any ('{0}') and four = any ('{0, 1}') and twenty = any ('{15}') and hundred = any ('{-1, 95}')
order by
  two desc, four desc, twenty desc, hundred desc;

-- (February 4) Proper symmetric handling of "array advancement must not get
-- far ahead" in the backward scans case is strictly necessary, as evidenced
-- by this test case:
insert into tenk1_dyn_saop (two, four, twenty, hundred) select 0, 0, 7, 97;
select *
from tenk1_dyn_saop
where
  two = any ('{0}') and four = any ('{0, 1}') and twenty = any ('{7,6,1}') and hundred = any ('{76,96,85,97,78}')
order by
  two desc, four desc, twenty desc, hundred desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from tenk1_dyn_saop
where
  two = any ('{0}') and four = any ('{0, 1}') and twenty = any ('{7,6,1}') and hundred = any ('{76,96,85,97,78}')
order by
  two desc, four desc, twenty desc, hundred desc;

-- (September 14)
--
-- Here we see contradictory scan keys on the column "two" during the start of
-- the first would-be primitive index scan where two = 1, but not any earlier
-- primitive scans.
prepare qual_on_two_not_okay_later_on as
select count(*), two, four, twenty, hundred
from
  tenk1_dyn_saop
where
  two in (0, 1) and four in (1, 2, 3)
  and two < 1
  and twenty in (1, 2, 5, 7, 8, 11, 12, 13, 14, 17)
  and hundred in (1, 3, 4, 9, 14, 51, 90, 88, 41, 39, 22)
group by
  two, four, twenty, hundred
order by
  two, four, twenty, hundred;

execute qual_on_two_not_okay_later_on;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute qual_on_two_not_okay_later_on;
deallocate qual_on_two_not_okay_later_on;

-- (October 30)
-- Proof that we need to be careful around truncated high key
-- attributes.  They must have their cur_elem set to 0 directly.
--
-- Here the truncated high key "(two, four, twenty, hundred)=(1, 1, 5)" is at
-- issue.  We must not advance the "hundred" scan key's cur_elem past 45
-- prematurely, because we'll miss matches on:
-- "(two, four, twenty, hundred)=(1, 1, 5, 45)".
--
-- Here is what the buggy version looked like (note that this involves two
-- attributes needing to advance at the same time, on "twenty" and "hundred"):
--
-- _bt_advance_array_keys, pivot tuple: (two, four, twenty, hundred)=(1, 1, 5), 0x7f268631e508
--  numberOfKeys: 4
--  - sk_attno: 1, cur_elem 1/1, val: 1 [NULLS LAST, ASC]
--  - sk_attno: 3, cur_elem 0/2, val: 1 [NULLS LAST, ASC]
--  - sk_attno: 4, cur_elem 2/2, val: 88 [NULLS LAST, ASC]
--  + sk_attno: 1, cur_elem 1/1, val: 1 [NULLS LAST, ASC]
--  + sk_attno: 3, cur_elem 2/2, val: 5 [NULLS LAST, ASC]      <--- correct
--  + sk_attno: 4, cur_elem 2/2, val: 88 [NULLS LAST, ASC]     <--- buggy
-- _bt_advance_array_keys: returns true
--
-- And here is the correct version:
--
-- _bt_advance_array_keys, pivot tuple: (two, four, twenty, hundred)=(1, 1, 5), 0x7fa6612ca508
--  numberOfKeys: 4
--  - sk_attno: 1, cur_elem 1/1, val: 1 [NULLS LAST, ASC]
--  - sk_attno: 3, cur_elem 0/2, val: 1 [NULLS LAST, ASC]
--  - sk_attno: 4, cur_elem 2/2, val: 88 [NULLS LAST, ASC]
--  + sk_attno: 1, cur_elem 1/1, val: 1 [NULLS LAST, ASC]
--  + sk_attno: 3, cur_elem 2/2, val: 5 [NULLS LAST, ASC]      <--- correct, as before
--  + sk_attno: 4, cur_elem 0/2, val: 1 [NULLS LAST, ASC]      <--- correct this time around
-- _bt_advance_array_keys: returns true
--
prepare high_key_first_elem_confusion as
select count(*), two, four, twenty, hundred
from tenk1_dyn_saop
where two in (0, 1) and four = 1 and twenty in (1, 2, 5) and hundred in (1, 45, 88)
group by two, four, twenty, hundred
order by two, four, twenty, hundred;

execute high_key_first_elem_confusion;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute high_key_first_elem_confusion;
deallocate high_key_first_elem_confusion;

-- (September 14) Similar to above, but it's earlier value of two, not later
-- ones
prepare qual_on_two_not_okay_earlier_on as
select count(*), two, four, twenty, hundred
from
  tenk1_dyn_saop
where
  two in (0, 1) and four in (1, 2, 3)
  and two > 0
  and twenty in (1, 2, 5, 7, 8, 11, 12, 13, 14, 17)
  and hundred in (1, 3, 4, 9, 14, 51, 90, 88, 41, 39, 22)
group by
  two, four, twenty, hundred
order by
  two, four, twenty, hundred;

execute qual_on_two_not_okay_earlier_on;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute qual_on_two_not_okay_earlier_on;
deallocate qual_on_two_not_okay_earlier_on;

-- (January 27) Similar to above, but switch the order of the "two" quals for
-- extra test coverage in _bt_preprocess_keys:
prepare qual_on_two_not_okay_earlier_on_switchqual as
select count(*), two, four, twenty, hundred
from
  tenk1_dyn_saop
where
  two > 0 and
  two in (0, 1) and four in (1, 2, 3)
  and twenty in (1, 2, 5, 7, 8, 11, 12, 13, 14, 17)
  and hundred in (1, 3, 4, 9, 14, 51, 90, 88, 41, 39, 22)
group by
  two, four, twenty, hundred
order by
  two, four, twenty, hundred;

execute qual_on_two_not_okay_earlier_on_switchqual;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute qual_on_two_not_okay_earlier_on_switchqual;
deallocate qual_on_two_not_okay_earlier_on_switchqual;

-- (January 27) Similar to above, but switch the order of the "two" quals for
-- extra test coverage in _bt_preprocess_keys, with different details:
prepare qual_on_two_not_okay_earlier_on_switchqual_again as
select count(*), two, four, twenty, hundred
from
  tenk1_dyn_saop
where
  two in (0, 1) and four in (1, 2, 3)
  and twenty in (1, 2, 5, 7, 8, 11, 12, 13, 14, 17)
  and hundred in (1, 3, 4, 9, 14, 51, 90, 88, 41, 39, 22)
  and two > 0
group by
  two, four, twenty, hundred
order by
  two, four, twenty, hundred;

execute qual_on_two_not_okay_earlier_on_switchqual_again;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute qual_on_two_not_okay_earlier_on_switchqual_again;
deallocate qual_on_two_not_okay_earlier_on_switchqual_again;

-- (January 27) Similar to above, but with = instead of > for "two" qual:
prepare qual_on_two_not_okay_earlier_on_switchqual_equal as
select count(*), two, four, twenty, hundred
from
  tenk1_dyn_saop
where
  two in (0, 1) and four in (1, 2, 3)
  and twenty in (1, 2, 5, 7, 8, 11, 12, 13, 14, 17)
  and hundred in (1, 3, 4, 9, 14, 51, 90, 88, 41, 39, 22)
  and two = 1
group by
  two, four, twenty, hundred
order by
  two, four, twenty, hundred;

execute qual_on_two_not_okay_earlier_on_switchqual_equal;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute qual_on_two_not_okay_earlier_on_switchqual_equal;
deallocate qual_on_two_not_okay_earlier_on_switchqual_equal;

-- (September 14) Similar to above, but it's an equality this time
prepare qual_on_two_not_okay_earlier_on_equality as
select count(*), two, four, twenty, hundred
from
  tenk1_dyn_saop
where
  two in (0, 1) and four in (1, 2, 3)
  and two = 1
  and twenty in (1, 2, 5, 7, 8, 11, 12, 13, 14, 17)
  and hundred in (1, 3, 4, 9, 14, 51, 90, 88, 41, 39, 22)
group by
  two, four, twenty, hundred
order by
  two, four, twenty, hundred;

execute qual_on_two_not_okay_earlier_on_equality;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute qual_on_two_not_okay_earlier_on_equality;
deallocate qual_on_two_not_okay_earlier_on_equality;

-- (September 14) Similar to above, but it's multiple SAOP equalities this time
prepare qual_on_two_multiple_saops as
select count(*), two, four, twenty, hundred
from
  tenk1_dyn_saop
where
  two in (0, 1) and two in (1, 2)
  and four in (1, 2, 3)
group by two, four, twenty, hundred
order by two, four, twenty, hundred;

execute qual_on_two_multiple_saops;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute qual_on_two_multiple_saops;
deallocate qual_on_two_multiple_saops;

-- (October 18) Similar to above, but tries to break lack of support for the
-- full set of _bt_preprocess_keys() push-ups in stripped down version of
-- function:
prepare qual_on_two_skew as
select count(*), two, four, twenty, hundred
from
  tenk1_dyn_saop
where
  two in (0, 1) and four in (1, 2, 3)
  and two in(-1,0)
  and twenty in (1, 2, 5, 7, 8, 11, 12, 13, 14, 17)
  and hundred in (1, 3, 4, 9, 14, 51, 90, 88, 41, 39, 22)
group by
  two, four, twenty, hundred
order by
  two, four, twenty, hundred;

-- (October 27) UPDATE: we are supposed to get (and do get) 175 buffer hits for this -- not 178
-- buffer hits.
--
-- At one point we got the following superfluous 4 page/buffer accesses at the
-- end of the scan, due to not recognizing that contradictory quals make "two = 1":
--
-- _bt_readpage: 🍀  7 with 12 offsets/tuples (leftsib 6, rightsib 8) ➡️
--  _bt_readpage first: (two, four, twenty, hundred)=(1, 1, 5, 5), 0x7faf243e1d80, from non-pivot offnum 2 TID (0,28) started page
--  _bt_readpage final: (two, four, twenty, hundred)=(1, 1, 13, 33), 0x7faf243e0508, continuescan high key check did not end scan so must continue to right sibling in next _bt_readpage call, if any
--  _bt_readpage stats: currPos.firstItem: 0, currPos.lastItem: -1, nmatching: 0 ❌
-- _bt_readnextpage: ScanDirectionIsForward() case reads right sibling blk 7
-- _bt_readpage: 🍀  8 with 12 offsets/tuples (leftsib 7, rightsib 9) ➡️
--  _bt_readpage first: (two, four, twenty, hundred)=(1, 1, 13, 33), 0x7faf243dfd80, from non-pivot offnum 2 TID (2,5) started page
--  _bt_readpage final: (two, four, twenty, hundred)=(1, 3, 3, 43), 0x7faf243de508, continuescan high key check did not end scan so must continue to right sibling in next _bt_readpage call, if any
--  _bt_readpage stats: currPos.firstItem: 0, currPos.lastItem: -1, nmatching: 0 ❌
-- _bt_readnextpage: ScanDirectionIsForward() case reads right sibling blk 8
-- _bt_readpage: 🍀  9 with 12 offsets/tuples (leftsib 8, rightsib 10) ➡️
--  _bt_readpage first: (two, four, twenty, hundred)=(1, 3, 3, 43), 0x7faf243ddd80, from non-pivot offnum 2 TID (0,10) started page
--  _bt_readpage final: (two, four, twenty, hundred)=(1, 3, 11, 71), 0x7faf243dc508, continuescan high key check did not end scan so must continue to right sibling in next _bt_readpage call, if any
--  _bt_readpage stats: currPos.firstItem: 0, currPos.lastItem: -1, nmatching: 0 ❌
-- _bt_readnextpage: ScanDirectionIsForward() case reads right sibling blk 9
-- _bt_readpage: 🍀  10 with 15 offsets/tuples (leftsib 9, rightsib 0) ➡️
--  _bt_readpage first: (two, four, twenty, hundred)=(1, 3, 11, 71), 0x7faf243b9d80, from non-pivot offnum 1 TID (0,15) started page
--  _bt_readpage final: (two, four, twenty, hundred)=(4999, 0, 0, 0), 0x7faf243b8298, from non-pivot offnum 13 TID (344,25) ended page and scan
--  _bt_readpage stats: currPos.firstItem: 0, currPos.lastItem: -1, nmatching: 0 ❌
-- _bt_readnextpage: ScanDirectionIsForward() case reads right sibling blk 10
--
-- Ultimate fix for this (which made us not do this unnecessary page visits) was to
-- teach _bt_preprocess_array_keys() to merge together arrays related to the
-- same attribute (in this example it's 2 arrays on the attribute named "two"),
-- plus some tweaks to the main _bt_advance_array_keys logic.
execute qual_on_two_skew;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 175 buffer hits (not 178)
execute qual_on_two_skew;
deallocate qual_on_two_skew;

-- (October 26) Similar to above, but lower order (though still SK_BT_REQFWD) scankey "four" this
-- time around:
prepare qual_on_four_skew as
select count(*), two, four, twenty, hundred
from
  tenk1_dyn_saop
where
  two = 0 and
  -- This is effectively "four in (1,2)" once _bt_preprocess_keys is applied
  -- correctly:
  four in (-1, 0, 1, 2) and
  four in (1, 2, 3, 4, 5)
group by
  two, four, twenty, hundred
order by
  two, four, twenty, hundred;

execute qual_on_four_skew;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute qual_on_four_skew;
deallocate qual_on_four_skew;

-- (October 19) Similar to above, but involves non-required scankeys only.
-- This test led to an assertion failure in new _bt_advance_array_keys
-- function.
prepare qual_on_four_nonrequired_skew as
select count(*), two, four, twenty, hundred
from
  tenk1_dyn_saop
where
  four in (1, 2, 3)
  and four in (-1, 1, 2, 4)
  and twenty in (1, 2, 5, 7, 8, 11, 12, 13, 14, 17)
  and hundred in (1, 3, 4, 9, 14, 51, 90, 88, 41, 39, 22)
group by
  two, four, twenty, hundred
order by
  two, four, twenty, hundred;

execute qual_on_four_nonrequired_skew;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute qual_on_four_nonrequired_skew;
deallocate qual_on_four_nonrequired_skew;

-- (November 29 2024) Query that uniquely detected skip scan "skipskip"
-- optimization's behavioral change (actually, this is just a refined version
-- of the previous qual_on_four_nonrequired_skew test case, which randomly
-- detected the issue in an overly-coarse manner)
--
-- UPDATE: ultimate conclusion about the issue raised by this test case was
-- that I should make _bt_readpage call _bt_start_array_keys before its
-- finaltup call to _bt_checkkeys.  The underlying issue was confusion
-- stemming from treating the SAOP arrays as non-required in the skipskip
-- context, even though they were generally considered required (and were
-- marked required).
prepare skipskip_skipping_differs as
select two, four, twenty, hundred
from tenk1_dyn_saop
where four = 1 and twenty in (-100000, 5, 13) and hundred in (25, 10000000);

execute skipskip_skipping_differs;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute skipskip_skipping_differs;
deallocate skipskip_skipping_differs;

-- (November 29 2024) Query that is capable of detecting issues similar to
-- skipskip_skipping_differs, that it won't necessarily detect itself
prepare skipskip_skipping_also_differs as
select two, four, twenty, hundred
from tenk1_dyn_saop
where four = 1 and twenty in (1, 5, 13) and hundred in (1, 14, 34);

execute skipskip_skipping_also_differs;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute skipskip_skipping_also_differs;
deallocate skipskip_skipping_also_differs;

-- (February 1) This made a recently written assertion fail when the
-- _bt_check_compare retry for a call to _bt_advance_array_keys that was
-- !sktrigrequired returned false:
--
-- TRAP: failed Assert("sktrigrequired"), File: "../source/src/backend/access/nbtree/nbtutils.c", Line: 1973, PID: 6849
-- 0   postgres                            0x0000000104c7bf9c ExceptionalCondition + 236
-- 1   postgres                            0x000000010452bb60 _bt_advance_array_keys + 4432
-- 2   postgres                            0x0000000104529cec _bt_checkkeys + 724
-- 3   postgres                            0x000000010451cb4c _bt_readpage + 1616
--
-- (Fix was to move this assertion down slightly, into the
-- "if (!pstate->continuescan)" block)
prepare nonrequired_array_nonrequired_inequality_confusion as
select count(*), two, four, twenty, hundred
from
  tenk1_dyn_saop
where
  four in (1, 2, 3)
  and four in (-1, 1, 2, 4)
  and twenty in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17)
  and hundred < 50
group by
  two, four, twenty, hundred
order by
  two, four, twenty, hundred;

execute nonrequired_array_nonrequired_inequality_confusion;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute nonrequired_array_nonrequired_inequality_confusion;
deallocate nonrequired_array_nonrequired_inequality_confusion;

-----------------------------------------
-- functional_dependencies test cases  --
-----------------------------------------
set client_min_messages=error;
drop table if exists functional_dependencies;
reset client_min_messages;

CREATE UNLOGGED TABLE functional_dependencies (
    filler1 TEXT,
    filler2 NUMERIC,
    a INT,
    b TEXT,
    filler3 DATE,
    c INT,
    d TEXT
);
CREATE INDEX fdeps_abc_idx ON functional_dependencies (a, b, c);
-- prewarm
select count(*) from functional_dependencies;
vacuum analyze functional_dependencies;
INSERT INTO functional_dependencies (a, b, c, filler1)
     SELECT mod(i,100), mod(i,50), mod(i,25), i FROM generate_series(1,5000) s(i);
-----------------------------------------

-- Test case from regression tests that failed with binary-search-saop-array
-- work, even when all other tests in this file passed:
SELECT count(*) FROM functional_dependencies WHERE a IN (1, 51) AND b IN ('1', '2');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
SELECT count(*) FROM functional_dependencies WHERE a IN (1, 51) AND b IN ('1', '2');


select count(*) from functional_dependencies
where
  a in (1, 2, 26, 27, 51, 52, 76, 77)
  and b in ('1', '2', '26', '27')
  and c in (1, 2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from functional_dependencies
where
  a in (1, 2, 26, 27, 51, 52, 76, 77)
  and b in ('1', '2', '26', '27')
  and c in (1, 2);

-- Test case for missing key column in predicate -- simple
prepare functional_dependencies_norequired_one as
select count(*), a, b, c
from
  functional_dependencies
where a = 25 and c in (0, 1, 2, 3)
group by a, b, c;

execute functional_dependencies_norequired_one;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_one;
deallocate functional_dependencies_norequired_one;

-- Test case for missing key column in predicate -- must skip 14 and match on
-- 15 here, slightly trickier
prepare functional_dependencies_norequired_two as
select count(*), a, b, c
from
  functional_dependencies
where a = 65 and c in (14, 15)
group by a, b, c;

execute functional_dependencies_norequired_two;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_two;
deallocate functional_dependencies_norequired_two;

-- Skip to different parts of index for each of 44, 94:
prepare functional_dependencies_norequired_three as
select count(*), a, b, c
from
  functional_dependencies
  where a in (44,94) and c in (18,19,20)
  group by a, b, c;

execute functional_dependencies_norequired_three;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_three;
deallocate functional_dependencies_norequired_three;

prepare functional_dependencies_norequired_four as
select count(*), a, b, c
from
  functional_dependencies
where
  a = any (array[1, 51])
  and b = '1'
group by a, b, c;

execute functional_dependencies_norequired_four;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_four;
deallocate functional_dependencies_norequired_four;

prepare functional_dependencies_norequired_five as
select count(*), a, b, c
from
  functional_dependencies
where
  a = any (array[1, 26, 51, 76])
  and b = any (array['1', '26'])
  and c = 1
group by a, b, c;

execute functional_dependencies_norequired_five;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_five;
deallocate functional_dependencies_norequired_five;

prepare functional_dependencies_norequired_six as
select count(*), a, b, c
from
  functional_dependencies
where
  a in (1, 2, 51, 52) and b = '1'
group by a, b, c;

execute functional_dependencies_norequired_six;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_six;
deallocate functional_dependencies_norequired_six;

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Now do backwards scan equivalents of all functional_dependencies tests:
select a, b, c from functional_dependencies
where
  a in (1, 2, 26, 27, 51, 52, 76, 77)
  and b in ('1', '2', '26', '27')
  and c in (1, 2)
order by a desc, b desc, c desc limit 52;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b, c from functional_dependencies
where
  a in (1, 2, 26, 27, 51, 52, 76, 77)
  and b in ('1', '2', '26', '27')
  and c in (1, 2)
order by a desc, b desc, c desc limit 52;

-- Test case for missing key column in predicate -- simple
prepare functional_dependencies_norequired_one_desc as
select count(*), a, b, c
from
  functional_dependencies
where a = 25 and c in (0, 1, 2, 3)
group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_one_desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_one_desc;
deallocate functional_dependencies_norequired_one_desc;

-- Test case for missing key column in predicate -- must skip 14 and match on
-- 15 here, slightly trickier
prepare functional_dependencies_norequired_two_desc as
select count(*), a, b, c
from
  functional_dependencies
where a = 65 and c in (14, 15)
group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_two_desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_two_desc;
deallocate functional_dependencies_norequired_two_desc;

-- Skip to different parts of index for each of 44, 94:
prepare functional_dependencies_norequired_three_desc as
select count(*), a, b, c
from
  functional_dependencies
  where a in (44,94) and c in (18,19,20)
  group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_three_desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_three_desc;
deallocate functional_dependencies_norequired_three_desc;

prepare functional_dependencies_norequired_four_desc as
select count(*), a, b, c
from
  functional_dependencies
where
  a = any (array[1, 51])
  and b = '1'
group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_four_desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_four_desc;
deallocate functional_dependencies_norequired_four_desc;

prepare functional_dependencies_norequired_five_desc as
select count(*), a, b, c
from
  functional_dependencies
where
  a = any (array[1, 26, 51, 76])
  and b = any (array['1', '26'])
  and c = 1
group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_five_desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_five_desc;
deallocate functional_dependencies_norequired_five_desc;

prepare functional_dependencies_norequired_six_desc as
select count(*), a, b, c
from
  functional_dependencies
where
  a in (1, 2, 51, 52) and b = '1'
group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_six_desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_six_desc;
deallocate functional_dependencies_norequired_six_desc;

-- (February 22)
--
-- Assertion failure due to non-required scan key not starting from the first
-- array element (for current scan direction) after _bt_preprocess_keys was
-- done:
prepare functional_dependencies_norequired_no_first_elem as
select count(*), a, b, c
from
  functional_dependencies
where
  a in (1,2) and c in (1,2,3) and c = 2
group by a, b, c;

-- Original assertion failure:
--
-- TRAP: failed Assert("_bt_verify_arrays_bt_first(scan, dir)"), File: "../source/src/backend/access/nbtree/nbtutils.c", Line: 2776, PID: 513412
-- [0x556451e17798] _bt_preprocess_keys: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtutils.c:2776
-- [0x556451e0e646] _bt_first: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtsearch.c:1181
-- [0x556451e0b0da] btgettuple: /mnt/nvme/postgresql/patch/build_meson_dc/../source/src/backend/access/nbtree/nbtree.c:290
--
-- Fix for this was to move the assertion into _bt_array_keys_remain, where
-- _bt_preprocess_keys incremental advancement cannot break things.
execute functional_dependencies_norequired_no_first_elem;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_no_first_elem;
deallocate functional_dependencies_norequired_no_first_elem;

------------------------------------------------
-- functional_dependencies random test cases  --
------------------------------------------------

set client_min_messages=error;
drop table if exists functional_dependencies_random;
reset client_min_messages;

CREATE UNLOGGED TABLE functional_dependencies_random (
    filler1 TEXT,
    filler2 NUMERIC,
    a INT,
    b TEXT,
    filler3 DATE,
    c INT,
    d TEXT
);
CREATE INDEX fdeps_abc_random_idx ON functional_dependencies_random (a, b, c);
INSERT INTO functional_dependencies_random (a, b, c, filler1)
     SELECT mod(i, 5), mod(i, 7), mod(i, 11), i FROM generate_series(1,1000) s(i);
-- prewarm
select count(*) from functional_dependencies_random;
vacuum analyze functional_dependencies_random;

SELECT count(*) FROM functional_dependencies_random WHERE a IN (1, 51) AND b IN ('1', '2');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
SELECT count(*) FROM functional_dependencies_random  WHERE a IN (1, 51) AND b IN ('1', '2');

select count(*) from functional_dependencies_random
where
  a in (1, 2, 26, 27, 51, 52, 76, 77)
  and b in ('1', '2', '26', '27')
  and c in (1, 2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from functional_dependencies_random
where
  a in (1, 2, 26, 27, 51, 52, 76, 77)
  and b in ('1', '2', '26', '27')
  and c in (1, 2);

-- Test case for missing key column in predicate -- simple
prepare functional_dependencies_norequired_one_random as
select count(*), a, b, c
from
  functional_dependencies_random
where a = 25 and c in (0, 1, 2, 3)
group by a, b, c;

execute functional_dependencies_norequired_one_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_one_random;
deallocate functional_dependencies_norequired_one_random;

-- Test case for missing key column in predicate -- must skip 14 and match on
-- 15 here, slightly trickier
prepare functional_dependencies_norequired_two_random as
select count(*), a, b, c
from
  functional_dependencies_random
where a = 65 and c in (14, 15)
group by a, b, c;

execute functional_dependencies_norequired_two_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_two_random;
deallocate functional_dependencies_norequired_two_random;

-- Skip to different parts of index for each of 44, 94:
prepare functional_dependencies_norequired_three_random as
select count(*), a, b, c
from
  functional_dependencies_random
  where a in (44,94) and c in (18,19,20)
  group by a, b, c;

execute functional_dependencies_norequired_three_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_three_random;
deallocate functional_dependencies_norequired_three_random;

prepare functional_dependencies_norequired_four_random as
select count(*), a, b, c
from
  functional_dependencies_random
where
  a = any (array[1, 51])
  and b = '1'
group by a, b, c;

execute functional_dependencies_norequired_four_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_four_random;
deallocate functional_dependencies_norequired_four_random;

prepare functional_dependencies_norequired_five_random as
select count(*), a, b, c
from
  functional_dependencies_random
where
  a = any (array[1, 26, 51, 76])
  and b = any (array['1', '26'])
  and c = 1
group by a, b, c;

execute functional_dependencies_norequired_five_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_five_random;
deallocate functional_dependencies_norequired_five_random;

prepare functional_dependencies_norequired_six_random as
select count(*), a, b, c
from
  functional_dependencies_random
where
  a in (1, 2, 51, 52) and b = '1'
group by a, b, c;

execute functional_dependencies_norequired_six_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_six_random;
deallocate functional_dependencies_norequired_six_random;

select a, b, c from functional_dependencies_random
where
  a in (1, 2, 26, 27, 51, 52, 76, 77)
  and b in ('1', '2', '26', '27')
  and c in (1, 2)
order by a desc, b desc, c desc limit 52;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select a, b, c from functional_dependencies_random
where
  a in (1, 2, 26, 27, 51, 52, 76, 77)
  and b in ('1', '2', '26', '27')
  and c in (1, 2)
order by a desc, b desc, c desc limit 52;

-- Test case for missing key column in predicate -- simple
prepare functional_dependencies_norequired_one_desc_random as
select count(*), a, b, c
from
  functional_dependencies_random
where a = 25 and c in (0, 1, 2, 3)
group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_one_desc_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_one_desc_random;
deallocate functional_dependencies_norequired_one_desc_random;

prepare functional_dependencies_norequired_two_desc_random as
select count(*), a, b, c
from
  functional_dependencies_random
where a = 65 and c in (14, 15)
group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_two_desc_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_two_desc_random;
deallocate functional_dependencies_norequired_two_desc_random;

-- Skip to different parts of index for each of 44, 94:
prepare functional_dependencies_norequired_three_desc_random as
select count(*), a, b, c
from
  functional_dependencies_random
  where a in (44,94) and c in (18,19,20)
  group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_three_desc_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_three_desc_random;
deallocate functional_dependencies_norequired_three_desc_random;

prepare functional_dependencies_norequired_four_desc_random as
select count(*), a, b, c
from
  functional_dependencies_random
where
  a = any (array[1, 51])
  and b = '1'
group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_four_desc_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_four_desc_random;
deallocate functional_dependencies_norequired_four_desc_random;

prepare functional_dependencies_norequired_five_desc_random as
select count(*), a, b, c
from
  functional_dependencies_random
where
  a = any (array[1, 26, 51, 76])
  and b = any (array['1', '26'])
  and c = 1
group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_five_desc_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_five_desc_random;
deallocate functional_dependencies_norequired_five_desc_random;

prepare functional_dependencies_norequired_six_desc_random as
select count(*), a, b, c
from
  functional_dependencies_random
where
  a in (1, 2, 51, 52) and b = '1'
group by a, b, c
order by a desc, b desc, c desc;

execute functional_dependencies_norequired_six_desc_random;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute functional_dependencies_norequired_six_desc_random;
deallocate functional_dependencies_norequired_six_desc_random;

---------------------------------------------------------------------------------
-- Don't accidentally scan way too many leaf pages rather than re-descend tree --
---------------------------------------------------------------------------------
--
-- UPDATE: (October 21) This variant of redescend_test was discovered by random chance
-- when you stressed the implementation by varying BTREE_DEFAULT_FILLFACTOR.
--
-- Apparently wraparound during backwards scans results in buggy
-- behavior/confusion when the scan direction changes.
--
-- UPDATE: (October 28) This bug was related to wraparound from incremental
-- advancement of array keys.  Recall that every form of what you could call array
-- wraparound (including during incremental advancement of array keys) has
-- since been removed.
--
-- UPDATE: (November 5) Recall that you originally had a separate test case
-- that minimally reproduced the wraparound bug by duplicating the start of
-- this test, but with fillfactor=30.  Today you decided to remove the smaller
-- test, and just use fillfactor=30 here, for the full test suite.
--
-- This decision was based on the fact that one of the later fetches from
-- cursor would fail a newly added assertion verifying agreement between input and
-- output scan keys, which wouldn't have been caught had I not randomly
-- decided to recheck what happened when I once again lowered fillfactor for
-- this test.  (The other factor that led to changing the tests like this was
-- the total lack of evidence that having independent coverage for default
-- fillfactor=90 had any independent benefit.  Plus it saved test cycles.)
set client_min_messages=error;
drop table if exists redescend_test;
reset client_min_messages;
create unlogged table redescend_test (district int4, warehouse int4, orderid int4, orderline int4);
create index must_not_full_scan on redescend_test (district, warehouse, orderid, orderline) with (fillfactor=30);
insert into redescend_test
select district, warehouse, orderid, orderline
from
  generate_series(1, 3) district,
  generate_series(1, 5) warehouse,
  generate_series(1, 150) orderid,
  generate_series(1, 10) orderline
order by
district, warehouse, orderid, orderline;
-- prewarm
select count(*) from redescend_test;
vacuum analyze redescend_test;
---------------------------------------------------------------------------------

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;
-- Our goal here (and the only reasonable approach that's possible given all
-- the specifics) is to be at parity with the master branch, index-buffer-hit-wise.
select ctid, * from redescend_test where district in (1,2,3) and warehouse = 5 and orderid = 22;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, * from redescend_test where district in (1,2,3) and warehouse = 5 and orderid = 22;

--
-- Page 75 (from index 'must_not_full_scan') high key looks like this:
-- (district, warehouse, orderid, orderline)=(3, 3, 125)
--
-- This provides us with "NULL > NOT_NULL" coverage, which led to assertion
-- failure because this is a forward scan, contrary to my expectations:
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,124,125) and orderline is null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,124,125) and orderline is null; -- 2 buffer hits
-- This should only need 2 buffer hits:
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,124) and orderline > 1000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,124) and orderline > 1000; -- 2 buffer hits
-- This should also only need 2 buffer hits:
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,124) and orderline < 1000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,124) and orderline < 1000; -- 2 buffer hits
-- This will also need 2:
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,125) and orderline > 1000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,125) and orderline > 1000; -- 2 buffer hits
-- This will also need 2:
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,125) and orderline < 1000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,125) and orderline < 1000; -- 2 buffer hits

-- Try it the other way -- 'NOT NULL' this time around:
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,124,125) and orderline is not null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test where district = 3 and warehouse = 3 and orderid in (121,124,125) and orderline is not null;

-- Can support ScalarArrayOpExr-as-index-quals alongside RowCompareExpr:
select count(*) from redescend_test where district = 3 and warehouse in (4,5) and (orderid,orderline) >= (3,3);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from redescend_test where district = 3 and warehouse in (4,5) and (orderid,orderline) >= (3,3);
-- Ditto:
select count(*) from redescend_test where district = 3 and warehouse in (4,5) and (orderid,orderline) < (1,3);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from redescend_test where district = 3 and warehouse in (4,5) and (orderid,orderline) < (1,3);

-- (November 17 2023)
--
-- Test handling of scans where it's important to account for how a scan key
-- that's only required in the direction opposite the current scan direction
-- (i.e. one that can affect _bt_first's initial position but won't terminate scan)
--
-- Here the trick is to not fail to terminate a primitive index scan that can
-- advance the arrays to the next set of equality constraint values, that
-- fails to notice that we're still way before where we'd end up if _bt_first
-- was used instead.
--
-- We expect parity with master branch for this (much like any other simple
-- low cardinality array column case).
--
-- (November 22) UPDATE: Make life harder by adding a handful of NULL rows
-- around the boundaries of each district.  This caused some assertion
-- failures in your original fixes for these issues.
--
-- Recall how _bt_checkkeys/_bt_check_compare has weird rules around
-- required-ness and NULLs, that also affected Korotkov's required scan keys
-- optimization.  (See commit 882368e8, "Fix btree stop-at-nulls logic properly"
-- for more context on this behavior).
insert into redescend_test
select district, NULL, NULL, NULL
from
  generate_series(1, 3) district,
  generate_series(1, 5) want_five_nulls_per_district;

select * from redescend_test where district in (1,2,3) and warehouse > 4 and orderid > 149;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test where district in (1,2,3) and warehouse > 4 and orderid > 149; -- 6 buffer hits with skip scan

-- (November 1 2024)
-- Same again, but use cross-type operators for skip arrays, expecting same
-- number of buffer hits:
--
-- UPDATE XXX (November 15 2024): I decided that cross-type support from this
-- transformation wasn't going to happen after all (see timestamptz tests in
-- skip_scan suit), so we're back to doing it the inefficient way now.
select * from redescend_test where district in (1,2,3) and warehouse > 4::int8 and orderid > 149::int8;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test where district in (1,2,3) and warehouse > 4::int8 and orderid > 149::int8;

-- This variant returns same rows as previous test case, but has equality
-- constraints for all columns barring the last, making it visit far fewer
-- leaf pages:
--
-- Note: this variant *doesn't* exercise bug like the original did at first,
-- because we've long been able to recognize that we're passed the end of
-- "warehouse = 5" equality constraint grouping.
--
-- (November 22) UPDATE:
--
-- Note: this is one of the only test cases that demonstrates that non-array
-- equality scan keys cannot exclusively rely on _bt_check_compare/the =
-- operator, which isn't too obvious (even less so now that we have
-- sktrig-based beyond_end_advance advancement).
--
-- IOW this shows that in general non-array equality scan keys need to be able
-- to trigger both beyond_end_advance and !all_eqtype_sk_equal/"before-start"
-- advancement.  IOW it shows that we can't get away with only having ORDER
-- procs for arrays themselves, which has been something that you've been
-- confused by at various points.
select * from redescend_test where district in (1,2,3) and warehouse = 5 and orderid > 149;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test where district in (1,2,3) and warehouse = 5 and orderid > 149; -- 6 buffer hits (patch + master)

-- This variant found a bug in my bugfix for the original (though only an hour
-- or so after), when I realized that I'd been sloppy about
-- required-vs-nonrequired arrays in my original fix.
--
-- This is exactly the same in terms of buffers accessed -- but the addition
-- of the non-required "orderline in (6,7,8)" array makes it slightly harder.
select * from redescend_test
where district in (1, 2, 3) and warehouse > 4 and orderid > 149 and orderline in (6, 7, 8);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse > 4 and orderid > 149 and orderline in (6, 7, 8);
-- (November 19)
--
-- >= instead of > variant, independently found a bug:
--
-- Update (February 18): What's really notable about this test case is that it
-- makes "orderline" non-required, but also sees _bt_first use all 4 scan keys (including
-- "orderline") in its initial descent insertion scan key.
--
-- As such, it is a good demonstration of possible dangers around being too clever about giving up
-- earlier for an unsatisfiable non-required array -- it might be that
-- _bt_first expects the non-required array keys to be advanced.
--
-- UPDATE XXX (February 19): see also, later similar redescend_test that goes
-- further than this one.
select * from redescend_test
where district in (1, 2, 3) and warehouse >= 5 and orderid >= 150 and orderline in (6, 7, 8);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse >= 5 and orderid >= 150 and orderline in (6, 7, 8);

-- Gap this time (just for good luck):
select * from redescend_test
where district in (1, 2, 3) and warehouse >= 5 and orderid >= 150 and orderline in (-1, 6, 8, 1000);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse >= 5 and orderid >= 150 and orderline in (-1, 6, 8, 1000);

-- Mix >= and >:
select * from redescend_test
where district in (1, 2, 3) and warehouse >= 5 and orderid > 149 and orderline in (6, 7, 8);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse >= 5 and orderid > 149 and orderline in (6, 7, 8);
-- Mix >= and > again:
select * from redescend_test
where district in (1, 2, 3) and warehouse > 4 and orderid >= 150 and orderline in (6, 7, 8);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse > 4 and orderid >= 150 and orderline in (6, 7, 8);

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Never had this same problem with same query once it uses a backwards scan:
select * from redescend_test
where district in (1, 2, 3) and warehouse > 4 and orderid > 149 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse > 4 and orderid > 149 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

-- (November 19 2024) Never had this same problem with same query once it uses a backwards scan:
-- (Repeat without skip support, caught a bug in avoid-coleman-testcase-regressions patch)
set skipscan_skipsupport_enabled=false;
select * from redescend_test
where district in (1, 2, 3) and warehouse > 4 and orderid > 149 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse > 4 and orderid > 149 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
reset skipscan_skipsupport_enabled;

-- UPDATE (March 2): "never had this problem" must have been due to how
-- _bt_first can "cons up" a SK_SEARCHNOTNULL scan key in its insertion scan
-- key, even though that doesn't appear in search type scan keys.
--
-- See:

/*
👾  btbeginscan to begin scan of index "must_not_full_scan"
♻️  btrescan
btrescan: BTScanPosInvalidate() called for markPos
_bt_preprocess_array_keys: SK_SEARCHARRAY input 0: [ flags: [SK_SEARCHARRAY], attno: 1 func OID: 65 ]
_bt_preprocess_array_keys: SK_SEARCHARRAY input 3: [ flags: [SK_SEARCHARRAY], attno: 4 func OID: 65 ]
_bt_preprocess_array_keys: scan_key: 0, num_elems: 3
_bt_preprocess_array_keys: scan_key: 3, num_elems: 3
 so->numArrayKeys is 2
_bt_start_array_keys: cur_elem 2, sk_attno: 1, val: 3 [NULLS LAST, ASC]
_bt_start_array_keys: cur_elem 2, sk_attno: 4, val: 8 [NULLS LAST, ASC]
_bt_preprocess_keys:  inkeys[0]: [ flags: [SK_SEARCHARRAY], attno: 1 func OID: 65 ]
_bt_preprocess_keys:  inkeys[1]: [ flags: [], attno: 2 func OID: 147 ]
_bt_preprocess_keys:  inkeys[2]: [ flags: [], attno: 3 func OID: 147 ]
_bt_preprocess_keys:  inkeys[3]: [ flags: [SK_SEARCHARRAY], attno: 4 func OID: 65 ]
_bt_preprocess_keys: outkeys[0]: [ flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD], attno: 1 func OID: 65 ]
_bt_preprocess_keys: outkeys[1]: [ flags: [SK_BT_REQBKWD], attno: 2 func OID: 147 ]
_bt_preprocess_keys: outkeys[2]: [ flags: [], attno: 3 func OID: 147 ]
_bt_preprocess_keys: outkeys[3]: [ flags: [SK_SEARCHARRAY], attno: 4 func OID: 65 ]
_bt_preprocess_keys: scan->numberOfKeys is 4, so->numberOfKeys on output is 4

➕     ➕     ➕
_bt_first: generating insertion scankey for initial positioning purposes using search/operator type scan keys
_bt_first: input startKeys[0]: [ flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD], attno: 1 func OID: 65 ]
_bt_first: sk_attno 1. val: 3, flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD]
_bt_first: input startKeys[1]: [ flags: [SK_ISNULL, SK_SEARCHNOTNULL], attno: 2 func OID: 0 ]
           sk_attno 2. val: NULL, flags: [SK_ISNULL, SK_SEARCHNOTNULL]
          with strat_total=1, inskey.keys=2, inskey.nextkey=0, inskey.backward=1
*/

-- This is the one case where a scan key required in the same direction as the
-- scan actually influences initial positioning strategy (usually it's only
-- equality strategy scan keys and required-in-opposite-direction-only
-- inequalities that can do that).  This is related to how _bt_check_compares
-- has that one case with NULLs that sees required-in-opposite-direction-only
-- cases passed as the sktrig to _bt_advance_array_keys.
--
-- The central feature of both of these optimizations is that the requiredness
-- of an inequality doesn't affect our continuescan-setting behavior with NULLs.
-- All that matters is the current scan direction, and the relationship
-- between that direction and whether NULLs are stored first or last relative
-- to non-NULLs


-- Same isn't true with this variant of the original query, that's also a
-- backwards scan, but has the operators flipped -- now the query has the same
-- adversarial character as the original (relative to the scan direction)
select * from redescend_test
where district in (1, 2, 3) and warehouse < 2 and orderid < 2 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse < 2 and orderid < 2 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

-- (November 1 2024)
--
-- Same again, but this time make life hard for skip scan preprocessing
-- optimization by using cross-type operators for those attributes that'll get
-- skip arrays:
--
-- UPDATE XXX (November 15 2024): I decided that cross-type support from this
-- transformation wasn't going to happen after all (see timestamptz tests in
-- skip_scan suit), so we're back to doing it the inefficient way now.
select * from redescend_test
where district in (1, 2, 3) and warehouse < 2::int8 and orderid < 2::int8 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- Should have same number of hits as before
select * from redescend_test
where district in (1, 2, 3) and warehouse < 2::int8 and orderid < 2::int8 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

-- (November 19)
--
-- <= instead of < variant, independently found a bug:
--
-- Update (February 18): What's really notable about this test case is that it
-- makes "orderline" non-required, but also sees _bt_first use all 4 scan keys (including
-- "orderline") in its initial descent insertion scan key.
--
-- As such, it is a good demonstration of possible dangers around being too clever about giving up
-- earlier for an unsatisfiable non-required array -- it might be that
-- _bt_first expects the non-required array keys to be advanced.
--
-- UPDATE XXX (February 19): see also, later similar redescend_test that goes
-- further than this one.
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

-- (March 8)
-- Derp, simple error in _bt_rewind_nonrequired_arrays (used && when I should
-- have used || instead)
--
-- TRAP: failed Assert("array->scan_key == ikey"), File: "../source/src/backend/access/nbtree/nbtutils.c", Line: 1037, PID: 154561
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid in (0, 1) and orderline >= any ('{6,7,8}')
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid in (0, 1) and orderline >= any ('{6,7,8}')
order by district desc, warehouse desc, orderid desc, orderline desc;

-- Gap this time (just for good luck):
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (-1, 6, 8, 1000)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (-1, 6, 8, 1000)
order by district desc, warehouse desc, orderid desc, orderline desc;

-- Mix <= and <:
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid < 2 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid < 2 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
-- Mix <= and < again:
select * from redescend_test
where district in (1, 2, 3) and warehouse < 2 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse < 2 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

-- (November 21)
--
-- RowCompare inequality variants

-- Test > RowCompare
prepare test_rowcompare as
select count(*), district, warehouse, orderid
from redescend_test
where district in (1, 2, 3) and (warehouse, orderid) > (4, 149)
group by district, warehouse, orderid
order by district, warehouse, orderid;
execute test_rowcompare;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute test_rowcompare;
deallocate test_rowcompare;

-- Test > RowCompare + non-required SAOPs
prepare test_rowcompare as
select count(*), district, warehouse, orderid
from redescend_test
where district in (1, 2, 3) and (warehouse, orderid) > (4, 149) and orderline in (-5,1,7,9,10,11)
group by district, warehouse, orderid
order by district, warehouse, orderid;
execute test_rowcompare;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute test_rowcompare;
deallocate test_rowcompare;

-- Test > RowCompare + non-required SAOPs, but with NULL RowCompare
prepare test_rowcompare_null as
select count(*), district, warehouse, orderid
from redescend_test
where district in (1, 2, 3) and (warehouse, orderid) > (4, NULL) and orderline in (-5,1,7,9,10,11)
group by district, warehouse, orderid
order by district, warehouse, orderid;
execute test_rowcompare_null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute test_rowcompare_null;
deallocate test_rowcompare_null;

-- Test >= RowCompare
prepare test_rowcompare as
select count(*), district, warehouse, orderid
from redescend_test
where district in (1, 2, 3) and (warehouse, orderid) >= (5, 149)
group by district, warehouse, orderid
order by district, warehouse, orderid;
execute test_rowcompare;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute test_rowcompare;
deallocate test_rowcompare;

-- Test >= RowCompare + non-required SAOPs
prepare test_rowcompare as
select count(*), district, warehouse, orderid
from redescend_test
where district in (1, 2, 3) and (warehouse, orderid) >= (5, 149) and orderline in (-5,1,7,9,10,11)
group by district, warehouse, orderid
order by district, warehouse, orderid;
execute test_rowcompare;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute test_rowcompare;
deallocate test_rowcompare;

-- Test < RowCompare (backwards scan)
prepare test_rowcompare as
select count(*), district, warehouse, orderid
from redescend_test
where district in (1, 2, 3) and (warehouse, orderid) < (2, 2)
group by district, warehouse, orderid
order by district desc, warehouse desc, orderid desc;
execute test_rowcompare;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute test_rowcompare;
deallocate test_rowcompare;

-- Test < RowCompare (backwards scan) + non-required SAOPs
prepare test_rowcompare as
select count(*), district, warehouse, orderid
from redescend_test
where district in (1, 2, 3) and (warehouse, orderid) < (2, 2) and orderline in (-5,1,7,9,10,11)
group by district, warehouse, orderid
order by district desc, warehouse desc, orderid desc;
execute test_rowcompare;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute test_rowcompare;
deallocate test_rowcompare;

-- Test <= RowCompare (backwards scan)
prepare test_rowcompare as
select count(*), district, warehouse, orderid
from redescend_test
where district in (1, 2, 3) and (warehouse, orderid) <= (1, 2)
group by district, warehouse, orderid
order by district desc, warehouse desc, orderid desc;
execute test_rowcompare;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute test_rowcompare;
deallocate test_rowcompare;

-- Test <= RowCompare (backwards scan), but with NULL RowCompare
prepare test_rowcompare_null as
select count(*), district, warehouse, orderid
from redescend_test
where district in (1, 2, 3) and (warehouse, orderid) <= (1, NULL)
group by district, warehouse, orderid
order by district desc, warehouse desc, orderid desc;
execute test_rowcompare_null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute test_rowcompare_null;
deallocate test_rowcompare_null;

-- Test <= RowCompare (backwards scan) + non-required SAOPs
prepare test_rowcompare as
select count(*), district, warehouse, orderid
from redescend_test
where district in (1, 2, 3) and (warehouse, orderid) <= (1, 2) and orderline in (-5,1,7,9,10,11)
group by district, warehouse, orderid
order by district desc, warehouse desc, orderid desc;
execute test_rowcompare;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute test_rowcompare;
deallocate test_rowcompare;

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Repeat last "backwards scan" test, but now use a forward/bitmap scan, to
-- exercise same code paths in the never-buggy scan direction (forwards):
select * from redescend_test
where district in (1, 2, 3) and warehouse < 2 and orderid < 2 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse < 2 and orderid < 2 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

-- Now test cursors that change the direction of the scan repeatedly, with
-- default scroll behavior:
set work_mem = 64;
set enable_sort = off;
-- forces index scan to get cursor to truly change directions in nbtree code
begin;
declare default_scroll_cursor cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
order by district, warehouse, orderid, orderline;
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset

fetch forward 100 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 50 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 25 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- This fetch proves that we need to not allow incremental advancement of
-- array keys to wrap/roll over, since if you remove that hardening you'll see
-- Assert(_bt_verify_array_scankeys(scan)) fail:
fetch backward 15 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 5 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Move to the end of the key space:
move forward 10000 in default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- See the last few rows there:
fetch backward 15 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Move back to the start of the key space:
move backward 10000 in default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- See the first few rows there:
fetch forward 15 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

/* default_scroll_cursor */ commit;

-- Show EXPLAIN ANALYZE for cursor from transaction block (doing better than
-- this seems nontrivial):
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
declare default_scroll_cursor cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
order by district, warehouse, orderid, orderline;

-- Same thing again, but with full scroll behavior this time around:
begin;
declare full_scroll_cursor scroll cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
order by district, warehouse, orderid, orderline;
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset

fetch forward 100 from full_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 50 from full_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 25 from full_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 15 from full_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 5 from full_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Move to the end of the key space:
move forward 10000 in full_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- See the last few rows there:
fetch backward 15 from full_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Move back to the start of the key space:
move backward 10000 in full_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- See the first few rows there:
fetch forward 15 from full_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

/* full_scroll_cursor */ commit;

-- Show EXPLAIN ANALYZE for cursor from transaction block:
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
declare full_scroll_cursor scroll cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
order by district, warehouse, orderid, orderline;

begin;
declare noscroll_cursor no scroll cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
order by district, warehouse, orderid, orderline;
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset

fetch forward 100 from noscroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 25 from noscroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 5 from noscroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Move to the end of the key space:
move forward 10000 in noscroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- See the last few rows there:
fetch backward 15 from noscroll_cursor; -- fails this time around

/* noscroll_cursor */ abort;

-- Show EXPLAIN ANALYZE for cursor from transaction block:
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
declare noscroll_cursor no scroll cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
order by district, warehouse, orderid, orderline;

-- Index scan
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- (November 23) Variant of "forces index scan to get cursor to truly change
-- directions in nbtree code", but with inequalities
--
-- The intention here is to add some inequalities that don't actually change
-- the answers we get compared to the original, and stress the implementation
-- by tickling the "required in opposite direction only" code path within
-- _bt_advance_array_keys.
--
-- This was the first test case added on thanksgiving -- this one was
-- exploratory, motivated by recent issues with inequalities.
begin;
declare default_scroll_cursor cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
and orderid > 0 and orderid <= 150
order by district, warehouse, orderid, orderline;
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset

fetch forward 100 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 50 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 25 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 15 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 5 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Move to the end of the key space:
move forward 10000 in default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- See the last few rows there:
fetch backward 15 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Move back to the start of the key space:
move backward 10000 in default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- See the first few rows there:
fetch forward 15 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

/* default_scroll_cursor */ commit;

-- (November 23) Show EXPLAIN ANALYZE for cursor from transaction block (doing
-- better than this seems nontrivial):
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 151 buffer hits (matches original)
declare default_scroll_cursor cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
and orderid > 0 and orderid <= 150
order by district, warehouse, orderid, orderline;

-- (November 23) Variant of "forces index scan to get cursor to truly change
-- directions in nbtree code", but with inequalities -- this time they have
-- much tighter bounds than prior variant.
--
-- The intention here is to NOT match the original, but rather to do far
-- fewer leaf page accesses because we can do several selective primitive
-- scans with these tighter inequality bounds.
--
-- This was the second test case added on thanksgiving -- this one had an
-- assertion failure related bug (confusion over start/end of primitive scan),
-- shown below next to the relevant statement.
--
-- Problem was that we didn't account for scan direction when scheduling a
-- primitive index scan.  So now _bt_steppage knows to forget about a schedule
-- primitive scan in the opposite/wrong direction, fixing the bug.
begin;
declare default_scroll_cursor cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
and orderid >= 42 and orderid <= 47
and orderline in (-500,5,7,8,9,500)
order by district, warehouse, orderid, orderline;
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset

-- Constants here differ to earlier on:
fetch forward 10 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- This caused an assertion failure at one point, right at the top of _bt_checkkeys:
-- TRAP: failed Assert("!so->needPrimScan"), File: "../source/src/backend/access/nbtree/nbtutils.c", Line: 2964, PID: 663861
fetch backward 20 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 5 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 5 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 15 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Move to the end of the key space:
move forward 10000 in default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- See the last few rows there:
fetch backward 15 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Move back to the start of the key space:
move backward 10000 in default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- See the first few rows there:
fetch forward 15 from default_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

/* default_scroll_cursor */ commit;


EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
declare default_scroll_cursor cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
and orderid >= 42 and orderid <= 47
and orderline in (-500,5,7,8,9,500)
order by district, warehouse, orderid, orderline;

-- (November 24)
--
-- Day after thanksgiving and I notice that yesterday's test case has a lack
-- of test coverage for the case where we change from backwards to forwards
-- and cancel a pending backwards-directed primitive index scan (it only had
-- coverage for cancelling a forwards scan when we go backwards, which is
-- far easier hit, probably due to suffix truncation of
-- the high key).
--
-- Remedy the situation by adding coverage here.
begin;
declare cancel_backwards_prim_scan_changedir cursor for
select * from redescend_test
where district in (1, 3, 800) and warehouse in (1,2)
and orderid in (48, 50)
order by district, warehouse, orderid, orderline;
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset

fetch forward 60 from cancel_backwards_prim_scan_changedir;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- This sets up the backwards primitive scan that ends up getting cancelled
-- later:
fetch backward 29 from cancel_backwards_prim_scan_changedir;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- This is the statement that actually cancels a pending primitive index scan
-- directed backward, since it goes forward at exactly the right/wrong time.
fetch forward 30 from cancel_backwards_prim_scan_changedir;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- These are for good luck:
fetch backward 30 from cancel_backwards_prim_scan_changedir;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward  31 from cancel_backwards_prim_scan_changedir;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 32 from cancel_backwards_prim_scan_changedir;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward  33 from cancel_backwards_prim_scan_changedir;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 34 from cancel_backwards_prim_scan_changedir;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward  35 from cancel_backwards_prim_scan_changedir;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

/* cancel_backwards_prim_scan_changedir */ commit;


-- Show EXPLAIN ANALYZE for cursor from transaction block:
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
declare cancel_backwards_prim_scan_changedir cursor for
select * from redescend_test
where district in (1, 3, 800) and warehouse in (1,2)
and orderid in (48, 50)
order by district, warehouse, orderid, orderline;

-- (November 25)
--
-- Here we don't remember the scan's array keys before processing a page, only
-- after processing a page (which is implicit, it's just the scan's current
-- keys).  So when we move the scan backwards we think that the top-level scan
-- should terminate, when in reality it should jump backwards to the leaf page
-- that we last visited.
--
-- This is closely related to the November 24 test, but can fail independently
-- in some way that I don't have the time or patience to pin down right now.
set client_min_messages=error;
drop table if exists backup_wrong_tbl;
reset client_min_messages;

create unlogged table backup_wrong_tbl (district int4, warehouse int4, orderid int4, orderline int4);
create index backup_wrong_idx on backup_wrong_tbl (district, warehouse, orderid, orderline);
insert into backup_wrong_tbl
select district, warehouse, orderid, orderline
from
  generate_series(1, 3) district,
  generate_series(1, 2) warehouse,
  generate_series(1, 51) orderid,
  generate_series(1, 10) orderline;

-- prewarm
select count(*) from backup_wrong_tbl;
vacuum analyze backup_wrong_tbl;

begin;
declare back_up_terminate_toplevel_wrong cursor for
select * from backup_wrong_tbl
where district in (1, 3) and warehouse in (1,2)
and orderid in (48, 50)
order by district, warehouse, orderid, orderline;
select pg_stat_get_xact_blocks_hit('backup_wrong_idx'::regclass) as cur_blocks_hit \gset

fetch forward 60 from back_up_terminate_toplevel_wrong;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('backup_wrong_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 29 from back_up_terminate_toplevel_wrong;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('backup_wrong_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 12 from back_up_terminate_toplevel_wrong;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('backup_wrong_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 30 from back_up_terminate_toplevel_wrong;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('backup_wrong_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward  31 from back_up_terminate_toplevel_wrong;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('backup_wrong_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 32 from back_up_terminate_toplevel_wrong;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('backup_wrong_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward  33 from back_up_terminate_toplevel_wrong;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('backup_wrong_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 34 from back_up_terminate_toplevel_wrong;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('backup_wrong_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward  35 from back_up_terminate_toplevel_wrong;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('backup_wrong_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

/* back_up_terminate_toplevel_wrong */ commit;

-- Show EXPLAIN ANALYZE for cursor from transaction block:
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
declare back_up_terminate_toplevel_wrong cursor for
select * from backup_wrong_tbl
where district in (1, 3) and warehouse in (1,2)
and orderid in (48, 50)
order by district, warehouse, orderid, orderline;

-- (November 25)
--
-- Case that led to an assertion failure in _bt_advance_array_keys, at the
-- point that it deals with inequalities required in the opposite direction
-- only.
begin;
declare inequalities_orderid cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
and orderid >= 1 and orderid <= 1000
and orderline in (-500,5,7,8,9,500)
order by district, warehouse, orderid, orderline;
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset

fetch forward 10 from inequalities_orderid;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 20 from inequalities_orderid;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 5 from inequalities_orderid;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 5 from inequalities_orderid;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 15 from inequalities_orderid;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

move forward 10000 in inequalities_orderid;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- "Fetch backward 15 from inequalities_orderid" Led to this assertion failure:
-- TRAP: failed Assert("opsktrig < first_nonrequired_ikey || first_nonrequired_ikey == -1"), File: "../source/src/backend/access/nbtree/nbtutils.c", Line: 1993, PID: 900702
fetch backward 15 from inequalities_orderid;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

move backward 10000 in inequalities_orderid;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 15 from inequalities_orderid;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('must_not_full_scan'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

/* inequalities_orderid */ commit;

-- Show EXPLAIN ANALYZE for cursor from transaction block:
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
declare inequalities_orderid cursor for
select * from redescend_test
where district in (1, 3) and warehouse in (3, 4, 5, 6, 7)
and orderid >= 1 and orderid <= 1000
and orderline in (-500,5,7,8,9,500)
order by district, warehouse, orderid, orderline;

--
-- Proof that code path that starts a new primitive index scan when it detects
-- that we might be too far before the start of _bt_first-wise matches, by flippping
-- scan direction and recalling _bt_check_compare, was buggy.
--
-- (February 19)
--
-- Issue is that we need to reset lower-order non-required arrays after the
-- required unsatisfied inequality that's required in the opposite-to-scan
-- direction to the first element (for the current scan direction).

-- This is like an earlier November 19 "independently found a bug" test case,
-- except that it deletes stuff from index that masked the bug there:
delete from redescend_test
where district = 2
  and warehouse = 1
  and orderid = 1
  and orderline < 8;
vacuum (index_cleanup on) redescend_test;

-- Other than that it's the same as earlier test (same answer, even):
select *
from redescend_test
where
  district in (1, 2, 3)
  and warehouse >= 5
  and orderid >= 150
  and orderline in (6, 7, 8);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from redescend_test
where
  district in (1, 2, 3)
  and warehouse >= 5
  and orderid >= 150
  and orderline in (6, 7, 8);

-- Repeat, deleting all the orderlines matching (2,1,1, *):
delete from redescend_test
where district = 2
  and warehouse = 1
  and orderid = 1;
vacuum (index_cleanup on) redescend_test;

-- Other than that it's the same as earlier test (same answer, even):
select *
from redescend_test
where
  district in (1, 2, 3)
  and warehouse >= 5
  and orderid >= 150
  and orderline in (6, 7, 8);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from redescend_test
where
  district in (1, 2, 3)
  and warehouse >= 5
  and orderid >= 150
  and orderline in (6, 7, 8);

-- Backward scan variant (for good luck):
delete from redescend_test
where district = 2
  and warehouse = 5
  and orderid = 150
  and orderline > 6;
vacuum (index_cleanup on) redescend_test;

-- First backward scan
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

delete from redescend_test
where district = 2
  and warehouse = 5
  and orderid = 150;
vacuum (index_cleanup on) redescend_test;

-- Second backward scan
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from redescend_test
where district in (1, 2, 3) and warehouse <= 1 and orderid <= 1 and orderline in (6, 7, 8)
order by district desc, warehouse desc, orderid desc, orderline desc;

--
-- We must not get confused by NULLs, which can terminate the scan (by having
-- _bt_check_compare set continuesca=false) even when the inequality is
-- required in the opposite scan direction only.  (See commit 882368e8,
-- "Fix btree stop-at-nulls logic properly" for more context on this behavior).
--
-- (February 23)
--
-- In this test case we insert many irrelevant-to-scan NULLs, that fill pages
-- that our index scan had better not waste time on accessing:
insert into redescend_test
select district, NULL, NULL, NULL
from
  generate_series(1, 2) district,
  generate_series(1, 2000) want_2k_nulls_per_district;

--
-- (February 23)
--
-- Fix for this bug was to treat required-in-opposite-direction-only scan keys
-- that see _bt_check_compare trigger continuescan=false due to the NULL
-- special case get treated rather like any other case where _bt_check_compare
-- sets continuescan=false upon encountering a required scan key -- namely, we
-- now do "beyond_end_advance" advancement in the style of other non-array required
-- scan keys.
--
-- This created a new problem (regressing older tests), though: it made
-- earlier and longer established required-in-opposite-direction-only cases
-- start to scan way too many pages again.  So fix had to do a special
-- sktrig-opposite-dir test that considers starting a new primitive index scan
-- if finaltup is still < new array keys (according to _bt_tuple_before_array_skeys,
-- at least).
--
select *
from redescend_test
where
  district in (1, 2, 3)
  and warehouse >= 5
  and orderid >= 150;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from redescend_test
where
  district in (1, 2, 3)
  and warehouse >= 5
  and orderid >= 150;

--
-- (February 23)
--
-- Same again, but with RowCompare at the end (just for good luck)
--
select *
from redescend_test
where
  district in (1, 2, 3)
  and (warehouse,orderid) >= (5,150);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from redescend_test
where
  district in (1, 2, 3)
  and (warehouse,orderid) >= (5,150);

--
-- (February 23)
--
-- Same again, but with non-required array at the end (just for good luck)
--
select *
from redescend_test
where
  district in (1, 2, 3)
  and (warehouse,orderid) >= (5,150)
  and orderline in (3, 6, 9);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from redescend_test
where
  district in (1, 2, 3)
  and (warehouse,orderid) >= (5,150)
  and orderline in (3, 6, 9);

reset work_mem;
reset enable_sort;

------------------------
-- DESC columns tests --
------------------------
set client_min_messages=error;
drop table if exists skippy_tbl_desc;
reset client_min_messages;

create unlogged table skippy_tbl_desc(
  bar int4
);
create index skippy_idx_desc on skippy_tbl_desc(bar desc);

insert into skippy_tbl_desc
select
  i
from
  generate_series(1, 500) i;
insert into skippy_tbl_desc
select
  555
from
  generate_series(1, 3000) i;

-- prewarm
select count(*) from skippy_tbl_desc;
vacuum analyze skippy_tbl_desc;

-- Looks like this now:
--
-- ┌───┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬──────────────────────────────────┐
-- │ i │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │             highkey              │
-- ├───┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼──────────────────────────────────┤
-- │ 1 │     1 │     1 │  1,278 │      6 │          0 │     7 │     0 │                 0 │   1,115 │       312 │ (bar)=(555), (htid)=('(7,196)')  │
-- │ 2 │     7 │     1 │  1,278 │      7 │          0 │     7 │     0 │                 0 │   1,115 │       312 │ (bar)=(555), (htid)=('(13,118)') │
-- │ 3 │     8 │     1 │    444 │      3 │          0 │    41 │     0 │                 0 │      75 │     4,888 │ (bar)=(500)                      │
-- │ 4 │     6 │     1 │     50 │      2 │          0 │    51 │     0 │                 0 │      16 │     7,128 │ (bar)=(450)                      │
-- │ 5 │     5 │     1 │    204 │      1 │          0 │   205 │     0 │                 0 │      16 │     4,048 │ (bar)=(246)                      │
-- │ 6 │     4 │     1 │    204 │      2 │          0 │   205 │     0 │                 0 │      16 │     4,048 │ (bar)=(42)                       │
-- │ 7 │     2 │     1 │     42 │      1 │          0 │    42 │     0 │                 0 │      16 │     7,308 │ ∅                                │
-- └───┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴──────────────────────────────────┘
--
-----------------------------------------------------------------------

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;
-- Don't get confused by DESC condition with pivot tuple high key termination:
select count(*) from skippy_tbl_desc where bar in (500,555);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from skippy_tbl_desc where bar in (500,555);
-- Backwards scan variant (doesn't use high key at all):
-- This is a convenient point to verify that backwards scans terminate without
-- redescending when there are a bunch of non-existent low sorting values on
-- the leftmost page (a similar test for forward scans happens elsewhere).
set enable_sort=off;
select distinct bar from skippy_tbl_desc where bar in (500,555,556,557,558,559,600) order by bar asc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select distinct bar from skippy_tbl_desc where bar in (500,555,556,557,558,559,600) order by bar asc;
set enable_sort=on;

----------------
-- NULLs test --
----------------

set client_min_messages=error;
drop table if exists nulls_test;
reset client_min_messages;
create unlogged table nulls_test(
  a int,
  b int
);

create index nulls_test_idx on nulls_test(a nulls first, b);

insert into nulls_test
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
insert into nulls_test
select
  NULL,
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

vacuum analyze nulls_test;

-- Looks like this now:
--
-- ┌────┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬───────────────────────────────────────┐
-- │ i  │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │                highkey                │
-- ├────┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼───────────────────────────────────────┤
-- │  1 │     1 │     1 │  1,271 │      7 │          0 │     7 │     0 │                 0 │   1,116 │       304 │ (a, b)=(null, 0), (htid)=('(30,188)') │
-- │  2 │    11 │     1 │  1,271 │      7 │          0 │     7 │     0 │                 0 │   1,116 │       304 │ (a, b)=(null, 0), (htid)=('(36,201)') │
-- │  3 │    12 │     1 │  1,271 │      7 │          0 │     7 │     0 │                 0 │   1,116 │       304 │ (a, b)=(null, 0), (htid)=('(42,214)') │
-- │  4 │    13 │     1 │  1,271 │      8 │          0 │     7 │     0 │                 0 │   1,116 │       304 │ (a, b)=(null, 0), (htid)=('(49,1)')   │
-- │  5 │    14 │     1 │    116 │      1 │          0 │   117 │     0 │                 0 │      24 │     4,872 │ (a, b)=(null, 1)                      │
-- │  6 │    10 │     1 │    778 │     28 │          0 │   113 │     0 │                 0 │      57 │     1,192 │ (a, b)=(28)                           │
-- │  7 │     9 │     1 │    476 │      3 │          0 │    69 │     0 │                 0 │      55 │     4,048 │ (a, b)=(62)                           │
-- │  8 │     2 │     1 │    854 │      5 │          0 │   123 │     0 │                 0 │      55 │       808 │ (a, b)=(123)                          │
-- │  9 │     4 │     1 │    854 │      5 │          0 │   123 │     0 │                 0 │      55 │       808 │ (a, b)=(184)                          │
-- │ 10 │     5 │     1 │    854 │      5 │          0 │   123 │     0 │                 0 │      55 │       808 │ (a, b)=(245)                          │
-- │ 11 │     6 │     1 │    854 │      4 │          0 │   123 │     0 │                 0 │      55 │       808 │ (a, b)=(306)                          │
-- │ 12 │     7 │     1 │    854 │      5 │          0 │   123 │     0 │                 0 │      55 │       808 │ (a, b)=(367)                          │
-- │ 13 │     8 │     1 │    476 │      3 │          0 │    80 │     0 │                 0 │      49 │     3,908 │ ∅                                     │
-- └────┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴───────────────────────────────────────┘
--
-----------------------------------------------------------------------

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Just two buffer accesses here (one for root, the other for leaf page 14):
-- Note: provides coverage of "NULL < NOT_NULL" case
select count(*) from nulls_test where a is NULL and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from nulls_test where a is NULL and b in (1,2);

-- Need 7 buffer accesses here (one for root, another 6 for pages 1, 11, 12,
-- 13, 14, and 10):
-- Note: provides coverage of "NULL < NOT_NULL" case
select count(*) from nulls_test where a is NULL and b in (0,1);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from nulls_test where a is NULL and b in (0,1); -- shouldn't be visiting 9, though

-- On the other hand we'll only need 6 here (root, plus another 5 for pages 1,
-- 11, 13, and 14).
select count(*) from nulls_test where a is NULL and b in (-1, 0);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from nulls_test where a is NULL and b in (-1, 0); -- shouldn't be visiting 10, though

-- NULLS FIRST, but a backwards scan:
-- Note: provides coverage of "NOT_NULL > NULL" case
set enable_bitmapscan to off;
set enable_indexonlyscan to on;
set enable_indexscan to off;
select * from nulls_test where a in (1,2) and b in (-1,-2) order by a desc nulls last, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_test where a in (1,2) and b in (-1,-2) order by a desc nulls last, b desc;

-- More complicated variant:
--
-- XXX Incorrect commentary:
--
-- Note: (July 18) I initially overlooked a regression here, where backwards
-- scans would senselessly fail to skip several leaf pages (remember, it was
-- after I added emojis to debug log to make it easier to grasp the high level
-- structure with lots of output?).
--
-- I guess it's easier to overlook backwards scan issues, since you can't just
-- force a bitmap index scan to get index breakdown in EXPLAIN ANALYZE.
--
-- Note: (July 18) This is also an example of speculatively accessing the next
-- page during a backwards scan, while dealing with uncertainty about what's
-- on the next page.  Here we find some matches on the initial page we descend
-- onto, which encourages us to access its left sibling page -- which doesn't
-- work out.  If we didn't make this gamble then we'd only have 8 buffer
-- accesses instead of 9.  But it's an intelligent gamble that usually works
-- out (and works out in other test cases), so I say that this is worth it --
-- a bad speculation, but the cost of doing business.
--
-- XXX correction:
--
-- Update: (September 12) Well, that previous commentary was all wrong, and
-- likely written on autopilot just before leaving for NYC (the fact that this
-- was before I came up with the idea of using unlogged tables for better
-- insights into index page accesses in EXPLAIN ANALYZE output wouldn't have
-- helped, either).
--
-- In reality, this index should do 2 descents for two leaf pages, for a total
-- of 4 buffer accesses to the index itself, and 5 total (assuming one VM hit).
--
-- Update: (November 1) Recall that this test case went on to cause further
-- confusion in late October.  Today you figured out that this was really an
-- issue with _bt_binsrch_array_skey() not doing the right thing for backwards
-- scans, in that the progress of array key advancement wouldn't ratchet in
-- the scan direction (i.e. it was unlike forward scans once you actually
-- instrumented the binary searches themselves, even for the very simplest
-- cases with one page and two constants).
select * from nulls_test where a in (1,2,350,359,360) and b in (-1,-2,1) order by a desc nulls last, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_test where a in (1,2,350,359,360) and b in (-1,-2,1) order by a desc nulls last, b desc; -- 4 or 5 (depending on if you count the VM or not) buffer accesses

-- These don't hit "NOT_NULL > NULL" path, so they're just for good luck:
select * from nulls_test where a in (368,369) and b in (-1,-2,1) order by a desc nulls last, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_test where a in (368,369) and b in (-1,-2,1) order by a desc nulls last, b desc;

-- Note: (September 4) This was the test case that was broken for some time,
-- unbeknownst to you.  You only figured this out when work on binary search
-- in arrays (the difficult work of getting the code to stop rescanning the
-- same tuple multiple times) was well underway.  Slightly unpleasant to learn
-- that I'd missed this earlier.
--
-- Here we have to visit leaf pages 7 and 8 (plus the root) because (since
-- this is a backwards scan) there's no way we can be sure that page 7 doesn't
-- have a tuple "(367, *)".  Of course the high key of page 7 would let us
-- know that, but we're coming from the right sibling to the left, so that's
-- not available (we could also choose to remember info from internal pages,
-- but seems not to be worth it to me)
--
-- XXX UPDATE (December 3): This changed (two extra buffer hits) when you changed
-- finaltup rules to insist on exact match.
select * from nulls_test where a in (367,368) and b in (-1,-2,1) order by a desc nulls last, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_test where a in (367,368) and b in (-1,-2,1) order by a desc nulls last, b desc;

-- XXX UPDATE (December 3): This also changed (two extra buffer hit) when you changed
-- finaltup rules to insist on exact match.
select * from nulls_test where a in (366,367) and b in (-1,-2,1) order by a desc nulls last, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_test where a in (366,367) and b in (-1,-2,1) order by a desc nulls last, b desc;

-- XXX UPDATE (December 3): This one stayed the same, though.
select * from nulls_test where a in (365,366) and b in (-1,-2,1) order by a desc nulls last, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from nulls_test where a in (365,366) and b in (-1,-2,1) order by a desc nulls last, b desc;

----------------------------------------
-- Basic "NULL != NULL" code coverage --
----------------------------------------
set client_min_messages=error;
drop table if exists coverage_null_compare_nonequal;
reset client_min_messages;
create unlogged table coverage_null_compare_nonequal(a int4, b int4);
create index coverage_null_compare_nonequal_idx on coverage_null_compare_nonequal (a, b nulls first);
insert into coverage_null_compare_nonequal select i from generate_series(1,3) i;
insert into coverage_null_compare_nonequal select null from generate_series(1,2000);

-- Looks like this now:
--
-- ┌───┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬─────────────────────────────────────────┐
-- │ i │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │                 highkey                 │
-- ├───┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼─────────────────────────────────────────┤
-- │ 1 │     1 │     1 │      3 │      1 │          0 │     4 │     0 │                 0 │      22 │     8,044 │ (a, b)=(null)                           │
-- │ 2 │     2 │     1 │  1,319 │      5 │          0 │     7 │     0 │                 0 │   1,150 │        64 │ (a, b)=(null, null), (htid)=('(4,159)') │
-- │ 3 │     4 │     1 │    681 │      3 │          0 │   276 │     0 │                 0 │      24 │       180 │ ∅                                       │
-- └───┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴─────────────────────────────────────────┘
--
-----------------------------------------------------------------------

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Hits the path that needs at least some coverage:
-- Note: provides coverage of "NOT_NULL < NULL" case
select * from coverage_null_compare_nonequal where a in (1,2,3);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from coverage_null_compare_nonequal where a in (1,2,3);

---------------------------------------------------------
-- required-same-direction IS NOT NULL confusion tests --
---------------------------------------------------------

set client_min_messages=error;
drop table if exists scankey_confusion;
reset client_min_messages;
create unlogged table scankey_confusion(
  a int,
  b int,
  c int
);

create index scankey_confusion_idx on scankey_confusion(a, b, c);
insert into scankey_confusion select 1, NULL, 3 from generate_series(1,1500);
insert into scankey_confusion select 0, NULL, 0;

-- looks like this now:
--
-- ┌───┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬────────────────────────────────────────────┐
-- │ i │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │                  highkey                   │
-- ├───┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼────────────────────────────────────────────┤
-- │ 1 │     1 │     1 │  1,272 │      7 │          0 │     8 │     0 │                 0 │     980 │       276 │ (a, b, c)=(1, null, 3), (htid)=('(5,141)') │
-- │ 2 │     2 │     1 │    229 │      2 │          0 │   229 │     0 │                 0 │      24 │     1,736 │ ∅                                          │
-- └───┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴────────────────────────────────────────────┘
--
-----------------------------------------------------------------------

-- Make sure that (a, b)=(1, null), (htid)=('(5,141)') high key will be compared by
-- insertion scan key function if we have a regression and allow
-- SK_BT_REQFWD-less scankeys to be used again:
delete from scankey_confusion where a = 1 and b is null;
vacuum scankey_confusion;

-- Now that we've done that delete + VACUUM, index looks like this (XXX a little misleading):
--
-- ┌───┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬────────────────────────────────────────────┐
-- │ i │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │                  highkey                   │
-- ├───┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼────────────────────────────────────────────┤
-- │ 1 │     1 │     1 │      1 │      1 │          0 │     2 │     0 │                 0 │      28 │     8,084 │ (a, b, c)=(1, null, 3), (htid)=('(5,141)') │
-- └───┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴────────────────────────────────────────────┘
--
-- XXX: This is misleading.  Rightmost page not shown due to this case borking
-- :leafkeyspace query, but it is still there.  Just for context, here's
-- :rootitems at this same point in the test:
--
-- ┌────────────┬──────────┬─────────┬───────┬──────┬────────────────────────┬──────┬─────────┬──────┐
-- │ itemoffset │   ctid   │ itemlen │ nulls │ vars │          data          │ dead │  htid   │ tids │
-- ├────────────┼──────────┼─────────┼───────┼──────┼────────────────────────┼──────┼─────────┼──────┤
-- │          1 │ (1,0)    │       8 │ f     │ f    │ (a, b, c)=()           │ ∅    │ ∅       │ ∅    │
-- │          2 │ (2,4099) │      32 │ t     │ f    │ (a, b, c)=(1, null, 3) │ ∅    │ (5,141) │ ∅    │
-- └────────────┴──────────┴─────────┴───────┴──────┴────────────────────────┴──────┴─────────┴──────┘

-- Main, original "scankey_confusion" test:
--
-- Try to make the implementation confused about same-direction-required IS NOT NULL inequality
--
-- (September 7) XXX:  This gets 3 buffer hits, instead of 2, as before
-- binary search on tuple stuff was in.  I think that that might be due to the
-- fact that one of the search-type scan keys isn't required by the scan.
-- We can maybe do better here, but let's not worry about it just yet.
--
-- (September 11) XXX: Yeah, I think that that's all it is.  It's probably an
-- example of a more general problem, though.  I think that any combination of
-- a inequality strategy scan key that's required.  So anything that looks
-- roughly like this + a high key comparison could be a problem
-- (SK_SEARCHNOTNULL is not relevant, SK_BT_REQFWD-only + high key is relevant):
--
-- _bt_preprocess_keys: output inkeys[1]: [ flags: [SK_ISNULL, SK_SEARCHNOTNULL, SK_BT_REQFWD]
--
-- Basically this happens because the second call to _bt_check_compare() is
-- (somewhat suspiciously) not allowed to set continuescan=false, no matter
-- what, in the case where _bt_advance_array_keys_locally() said that the
-- tuple has keys that equal all of the corresponding current array elements.
--
-- What's so special about BTEqualStrategyNumber, anyway? The only special
-- thing about them is the need to suppress continuescan=false when we're
-- too early (per _bt_first and _bt_checkkeys/_bt_check_compare comments).
--
-- (September 14) UPDATE: Yeah, this is now fixed once again -- now we go back
-- to not being confused about inequalities.  Recall that this happened when
-- you figured out (or more like stumbled upon) a way to not have to veto what the
-- second call to _bt_check_compare says about "continuescan" -- the call at
-- the end of _bt_checkkeys (after _bt_checkkeys has found matching equality/array
-- keys, leaving only the possibility of continuescan=false being set due to
-- required inequality type keys).
--
-- XXX UPDATE: (November 11) Actually, this was wrong.  I think that I do need
-- to suppress continuescan=false set by the second call to _bt_check_compare
-- after all.  See "don't accept continuescan=false during second call to
-- _bt_check_compare" bug test case, below.
--
-- XXX UPDATE: (November 16) Actually, the original idea (that we only need
-- two buffer hits for this original query) was correct, but became conflated
-- with the aforementioned correctness issue of November 11.
--
-- Here's whats really going on here: we need to make _bt_advance_array_keys
-- call itself recursively to deal with a tuple that has two required scan
-- keys that become unsatisfied at the same time: the array on "a", and the
-- inequality on "b" (IS NOT NULL is required in only one direction, sort of
-- like 'WHERE b < NULL").  If we don't realize that it's both at the same
-- time during within the same tuple/call, we won't notice the second
-- inequality right away.
--
-- AFAICT allowing this really wouldn't be harmful, but it would mean that
-- this case would only terminate having read both of the leaf pages (leftmost
-- and rightmost leaf pages).  More importantly, not handling this precisely
-- would add an awkward caveat to the promise that _bt_advance_array_keys
-- makes about advancing the array keys to the maximum extent that's known to
-- be possible based on the scan's current tuple.  The recursive call thing is
-- kinda ugly, but right now I judge that it's less ugly than not having it.
--
-- Note: Here the recursive call to _bt_advance_array_keys happens on tuple:
-- (a, b)=(0, null)
--
-- XXX UPDATE (February 18): See also, related test case
-- equal_inequal_nonrequired, which is a more complicated scenario involving
-- the recusive call to _bt_advance_array_keys
select * from scankey_confusion where a in (-1,0,1) and b is not null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from scankey_confusion where a in (-1,0,1) and b is not null; -- Just 2 buffer hits (root + leftmost leaf)

-- This "is null" variant of the original provides a useful contrast, because
-- it really does need 3 buffer hits:
select * from scankey_confusion where a in (-1,0,1) and b is null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from scankey_confusion where a in (-1,0,1) and b is null; -- 3 buffer hits (root and both leaf pages)

-- This one should only need rightmost page (along with root):
select * from scankey_confusion where a in (2,3,4,5) and b is null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from scankey_confusion where a in (2,3,4,5) and b is null; -- Just 2 buffer hits (root + rightmost leaf)

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Original:
select * from scankey_confusion where a in (-1,0,1) and b is not null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from scankey_confusion where a in (-1,0,1) and b is not null; -- Just 2 buffer hits (root + leftmost leaf)

-- This "is null" variant of the original provides a useful contrast, because
-- it really does need 3 buffer hits:
select * from scankey_confusion where a in (-1,0,1) and b is null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from scankey_confusion where a in (-1,0,1) and b is null; -- 3 buffer hits (root and both leaf pages)

-- This one should only need rightmost page (along with root):
select * from scankey_confusion where a in (2,3,4,5) and b is null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from scankey_confusion where a in (2,3,4,5) and b is null; -- Just 2 buffer hits (root + rightmost leaf)

-- (November 11) "don't accept continuescan=false during second call to
-- _bt_check_compare" bug test case:
--
-- UPDATE (November 16) Note: Here the recursive call to
-- _bt_advance_array_keys happens on tuple:
--
-- (a, b)=(0, null)
--
-- IOW, it's the same tuple as the one we saw back with our "original" test
-- case, though this time the immediate outcome isn't terminating the
-- top-level scan -- making sure that the scan continues at that point instead
-- is what we set out to verify with this test case.
insert into scankey_confusion select 1, 1, 1; -- tuple goes on right edge of leftmost page, matches quals below

-- Quals match tuples that straddle leftmost and rightmost pages (all leaf
-- pages in index):
select * from scankey_confusion where a in (-1,0,1) and b is not null;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 2 index buffer hits + 1 heap buffer hit
select * from scankey_confusion where a in (-1,0,1) and b is not null;

-- Shouldn't matter if we have a satisfiable extra non-required array on c:
select * from scankey_confusion where a in (-1,0,1) and b is not null and c in (0,1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 2 index buffer hits + 1 heap buffer hit
select * from scankey_confusion where a in (-1,0,1) and b is not null and c in (0,1,2);

-- Still shouldn't matter if we have a satisfiable extra non-required array on c:
select * from scankey_confusion where a in (-1,0,1) and b is not null and c in (0,1,2,50);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 2 index buffer hits + 1 heap buffer hit
select * from scankey_confusion where a in (-1,0,1) and b is not null and c in (0,1,2,50);

-- Plus even a non-satisfiable c array scan key shouldn't change number of
-- blocks hit in index (still 2, though no heap hit this time):
select * from scankey_confusion where a in (-1,0,1) and b is not null and c in (-55,-54, 40);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 2 index buffer hits + 0 heap buffer hits
select * from scankey_confusion where a in (-1,0,1) and b is not null and c in (-55,-54, 40);

-----------------------------------------
-- equal_inequal_nonrequired test case --
-----------------------------------------

-- (February 18)
--
-- Don't get confused with a mix of equality, inequality, and non-required
-- array -- this is a fancy version of scankey_confusion inequality.
--
-- This is another more complicated scenario involving
-- the recusive call to _bt_advance_array_keys.  It tests one aspect of the
-- logic becoming confused in the presence of a mix of required equality, required
-- inequality, and non-required array equality -- specifically, when they all
-- happen to change within one single call to _bt_advance_array_keys, for one
-- tuple only.

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

set client_min_messages=error;
drop table if exists equal_inequal_nonrequired;
reset client_min_messages;

-- Index has to have very wide tuples, to make it easy to defeat suffix
-- truncation:
create unlogged table equal_inequal_nonrequired(equal int4, inequal int4, nonrequired int4, filler text);
alter table plain_equal_inequal_nonrequired set (autovacuum_enabled=off);
alter table equal_inequal_nonrequired alter column filler set storage plain;
create index plain_equal_inequal_nonrequired on equal_inequal_nonrequired(equal, inequal, nonrequired, filler);

-- Make sure storage of index is as expected (no TOAST compression):
select attrelid::regclass, attname, atttypid, attstorage
from pg_attribute
where attrelid::regclass::text = 'plain_equal_inequal_nonrequired';

-- Bulk insert:
insert into equal_inequal_nonrequired
select
  42,
  i % 5,
  i,
  repeat(chr(ascii('0')), 2500)
from
  generate_series(1, 20) i;

-- Now delete a much smaller tuple (no filler) that's going to be the one the
-- query returns from end of first page (page at blkno 1):
insert into equal_inequal_nonrequired
select
  41,
  -1,
  -1;
-- Delete other tuples in blkno 1:
delete from equal_inequal_nonrequired
where equal = 42
  and inequal = 0
  and nonrequired < 15;
-- Make sure the deleted tuples are gone from index:
vacuum equal_inequal_nonrequired;

-- Should see this now (:leafkeyspace not :rootitems shown in comments, just
-- for the extra detail):
--
-- pg@regression:5432 [131412]=# :leafkeyspace
--┌───┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬───────────────────────────────────────────────────┐
--│ i │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │                      highkey                      │
--├───┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼───────────────────────────────────────────────────┤
--│ 1 │     1 │     1 │      1 │      1 │          0 │     2 │     0 │                 0 │      28 │     8,084 │ (equal, inequal, nonrequired, filler)=(42, 0, 15) │
--│ 2 │     7 │     1 │      3 │      3 │          0 │     4 │     0 │                 0 │   1,902 │       524 │ (equal, inequal, nonrequired, filler)=(42, 1, 6)  │
--│ 3 │     4 │     1 │      3 │      3 │          0 │     4 │     0 │                 0 │   1,900 │       532 │ (equal, inequal, nonrequired, filler)=(42, 2)     │
--│ 4 │     6 │     1 │      2 │      2 │          0 │     3 │     0 │                 0 │   1,693 │     3,056 │ (equal, inequal, nonrequired, filler)=(42, 2, 12) │
--│ 5 │     8 │     1 │      2 │      2 │          0 │     3 │     0 │                 0 │   1,690 │     3,064 │ (equal, inequal, nonrequired, filler)=(42, 3)     │
--│ 6 │     2 │     1 │      2 │      2 │          0 │     3 │     0 │                 0 │   1,693 │     3,056 │ (equal, inequal, nonrequired, filler)=(42, 3, 13) │
--│ 7 │     9 │     1 │      2 │      2 │          0 │     3 │     0 │                 0 │   1,690 │     3,064 │ (equal, inequal, nonrequired, filler)=(42, 4)     │
--│ 8 │     5 │     1 │      2 │      2 │          0 │     3 │     0 │                 0 │   1,693 │     3,056 │ (equal, inequal, nonrequired, filler)=(42, 4, 14) │
--│ 9 │    10 │     1 │      2 │      2 │          0 │     2 │     0 │                 0 │   2,528 │     3,084 │ ∅                                                 │
--└───┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴───────────────────────────────────────────────────┘
--(9 rows)

-- This is :rootitems, just to avoid test regressions:
select itemoffset, ctid, itemlen, nulls from bt_page_items('plain_equal_inequal_nonrequired',
  (select fastroot::int4 from bt_metap('plain_equal_inequal_nonrequired')));

-- Returns (41 ,-1, -1) tuple that we just inserted on its own (right before the
-- DELETE + VACUUM):
select *
from
  equal_inequal_nonrequired
where
  equal in (41, 42)
  and inequal < 0
  and nonrequired in (-1, 55, 56);

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- Two index buffer accesses (for root and one leaf) expected
select *
from
  equal_inequal_nonrequired
where
  equal in (41, 42)
  and inequal < 0
  and nonrequired in (-1, 55, 56);

-- Buggy continuescan=true behavior version of the patch just before bug was
-- fixed:

/*
_bt_readpage: 🍀  1 with 2 offsets/tuples (leftsib 0, rightsib 7) ➡️
 _bt_readpage first: (equal, inequal, nonrequired, filler)=(41, -1, -1, null), 0x7fc801bdafb8, from non-pivot offnum 2 TID (6,3) started page
  _bt_checkkeys: comparing (equal, inequal, nonrequired, filler)=(41, -1, -1, null) with TID (6,3), 0x7fc801bdafb8
                 final result is true
  _bt_checkkeys: comparing (equal, inequal, nonrequired, filler)=(42, 0, 15) with TID -inf, 0x7fc801bdafd8
   ikey 0/sk_attno 1: (sk_argument 41) result: false, with continuescan=0
_bt_advance_array_keys, pivot tuple: (equal, inequal, nonrequired, filler)=(42, 0, 15), 0x7fc801bdafd8
  numberOfKeys: 3
  - sk_attno: 1, cur_elem 0/1, val: 41 [NULLS LAST, ASC]
  - sk_attno: 3, cur_elem 0/2, val: -1 [NULLS LAST, ASC]
   _bt_binsrch_array_skey: searching for item 42, cur_elem: 0 (val 41), low_elem 0, high_elem 1, num_elems: 2
                           found item 42 at elem offset 1
   _bt_binsrch_array_skey: searching for item 15, cur_elem: 0 (val 18446744073709551615), low_elem 0, high_elem 2, num_elems: 3
                           found item 55 at elem offset 1
  + sk_attno: 1, cur_elem 1/1, val: 42 [NULLS LAST, ASC]
  + sk_attno: 3, cur_elem 1/2, val: 55 [NULLS LAST, ASC]
  _bt_advance_array_keys: all_required_or_array_satisfied is 0
 _bt_readpage final: (equal, inequal, nonrequired, filler)=(42, 0, 15), 0x7fc801bdafd8, continuescan high key check did not set so->currPos.moreRight=false ➡️  🟢
 _bt_readpage stats: currPos.firstItem: 0, currPos.lastItem: 0, nmatching: 1 ✅
_bt_first: returning offnum 2 TID (6,3)
_bt_readpage: 🍀  7 with 4 offsets/tuples (leftsib 1, rightsib 4) ➡️   <--- Wonky, this leaf page access is superfluous

*** SNIP wasted leaf page access for next sibling page not shown ****
*/

-- The fix for this issue involved making sure that _bt_advance_array_keys
-- didn't skip its recheck of the tuple (meaning its call to _bt_check_compare)
-- due only to an unsatisfied non-required array key that it detected
-- (detected by not finding a matching array element for that non-required
-- scan key array).
--
-- The buggy case involved:
--
-- 1. Needing to call _bt_advance_array_keys to advance the required array on
--    "equal" att.  Finding a match (advancing the array) for "equal" happens
--    while finding an exact match.
-- 2. Skipping over the required inequality on "inequal" att.
-- 3. Needing to find a match for non-required array scan key for
--    "nonrequired" att.  Finding a match for "nonrequired" fails.
--
-- This sets us up for trouble, because buggy code thought that the recheck
-- call to _bt_check_compare wasn't necessary due to point 3 above.  That
-- meant that there was no possible way that we could detect point 2 above in
-- second pass over tuple (with recursive call to _bt_advance_array_keys).
--
-- How can there be a second pass over the tuple if there is no second call to
-- _bt_check_compare that allows us to notice the inequality missed in
-- _bt_checkkeys's original (pre-_bt_advance_array_keys) call to
-- _bt_check_compare?  Point 2 can only be detected by seeing the second call
-- to _bt_check_compare set continuescan=false, and working backwards from that.

--
-- (February 25)
--
-- This test case is concerned with not confusing Korotkov's continuescan
-- precheck mechanism when we gamble and assume that -inf truncated attributes suggest
-- matches on the very next page.  (See also: next test case, keys_ahead test.)
select equal, inequal, nonrequired
from equal_inequal_nonrequired
where
  equal = 42
  and inequal in (1, 2)
  and nonrequired = 17;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select equal, inequal, nonrequired
from equal_inequal_nonrequired
where
  equal = 42
  and inequal in (1, 2)
  and nonrequired = 17;

-- (March 15) This is like the original, but uses a cursor
--
-- Test case expanded upon to help with simplify handling of
-- scan-changes-direction changes within code such as _bt_steppage and
-- _bt_readnextpage
set work_mem = 64;
set enable_sort = off;

begin;
declare korotkov_scroll_cursor cursor for
select equal, inequal, nonrequired
from equal_inequal_nonrequired
where
  equal = 42
  and inequal in (1, 2)
  and nonrequired = 17
order by equal, inequal, nonrequired;
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
\set old_blocks_hit :cur_blocks_hit

fetch forward 1 from korotkov_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh; -- 5 index hits, matches original

fetch forward 1 from korotkov_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh; -- 0 index hits (past end of matches)

fetch backward 1 from korotkov_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh; -- 2 index hits

fetch backward 270 from korotkov_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh; -- 2 index hits

fetch forward 1 from korotkov_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh; -- 5 index hits (back to where we started)

/* korotkov_scroll_cursor */ commit;

reset work_mem;
reset enable_sort;

-- (March 15) Variant of February 25 test case, that somehow now fails on
-- current version of the patch
select equal, inequal, nonrequired
from equal_inequal_nonrequired
where
  equal = 42
  and inequal in (1, 2, 6, 7)
  and nonrequired in (7, 17);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select equal, inequal, nonrequired
from equal_inequal_nonrequired
where
  equal = 42
  and inequal in (1, 2, 6, 7)
  and nonrequired in (7, 17);

-- (March 15) This is like the original cursor, but uses the new query variant
set work_mem = 64;
set enable_sort = off;

begin;
declare korotkov_scroll_cursor cursor for
select equal, inequal, nonrequired
from equal_inequal_nonrequired
where
  equal = 42
  and inequal in (1, 2, 6, 7)
  and nonrequired in (7, 17)
order by equal, inequal, nonrequired;
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
\set old_blocks_hit :cur_blocks_hit

fetch forward 1 from korotkov_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 1 from korotkov_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 1 from korotkov_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 270 from korotkov_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch forward 1 from korotkov_scroll_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('plain_equal_inequal_nonrequired'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

/* korotkov_scroll_cursor */ commit;


reset work_mem;
reset enable_sort;

--------------------------
-- keys_ahead test case --
--------------------------

--
-- (February 25)
--
-- This test case is concerned with not failing to cut our losses on the very
-- next page when we gamble and assume that -inf truncated attributes suggest
-- matches on the very next page.
--
-- When we get to the very next page and see that we guessed wrong, we must
-- cut our losses at that point -- we mustn't vainly plow on through other
-- leaf pages that come after that one.

set client_min_messages=error;
drop table if exists keys_ahead;
reset client_min_messages;
create unlogged table keys_ahead(a int4, b int4);
-- "Fillfactor 50" to make sure we get a truncated high key "(a, b)=(2)"
create index keys_ahead_idx on keys_ahead (a, b) with (fillfactor=50);
insert into keys_ahead
select i, j
  from generate_series(1, 2) i, generate_series(1, 2000) j
-- "order by hashint4(j)" to make sure we get a truncated high key "(a, b)=(2)"
order by hashint4(j);
vacuum analyze keys_ahead;

-- Now index looks like this:

/*
pg@regression:5432 [665720]=# :leafkeyspace
┌────┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬──────────────────┐
│ i  │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │     highkey      │
├────┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼──────────────────┤
│  1 │     1 │     1 │    261 │     18 │          0 │   262 │     0 │                 0 │      16 │     2,908 │ (a, b)=(1, 262)  │
│  2 │    11 │     1 │    270 │     18 │          0 │   271 │     0 │                 0 │      16 │     2,728 │ (a, b)=(1, 532)  │
│  3 │     6 │     1 │    237 │     18 │          0 │   238 │     0 │                 0 │      16 │     3,388 │ (a, b)=(1, 769)  │
│  4 │    16 │     1 │    252 │     18 │          0 │   253 │     0 │                 0 │      16 │     3,088 │ (a, b)=(1, 1021) │
│  5 │     4 │     1 │    251 │     18 │          0 │   252 │     0 │                 0 │      16 │     3,108 │ (a, b)=(1, 1272) │
│  6 │    14 │     1 │    242 │     18 │          0 │   243 │     0 │                 0 │      16 │     3,288 │ (a, b)=(1, 1514) │
│  7 │     8 │     1 │    245 │     18 │          0 │   246 │     0 │                 0 │      16 │     3,228 │ (a, b)=(1, 1759) │
│  8 │    13 │     1 │    242 │     18 │          0 │   243 │     0 │                 0 │      16 │     3,288 │ (a, b)=(2)       │  <--- shouldn't skip here (gamble instead)
│  9 │     2 │     1 │    261 │     18 │          0 │   262 │     0 │                 0 │      16 │     2,908 │ (a, b)=(2, 262)  │  <--- But we should be cutting our losses here...
│ 10 │    10 │     1 │    270 │     18 │          0 │   271 │     0 │                 0 │      16 │     2,728 │ (a, b)=(2, 532)  │
│ 11 │     7 │     1 │    237 │     18 │          0 │   238 │     0 │                 0 │      16 │     3,388 │ (a, b)=(2, 769)  │
│ 12 │    17 │     1 │    251 │     18 │          0 │   252 │     0 │                 0 │      16 │     3,108 │ (a, b)=(2, 1020) │
│ 13 │     5 │     1 │    251 │     18 │          0 │   252 │     0 │                 0 │      16 │     3,108 │ (a, b)=(2, 1271) │
│ 14 │    15 │     1 │    238 │     18 │          0 │   239 │     0 │                 0 │      16 │     3,368 │ (a, b)=(2, 1509) │
│ 15 │     9 │     1 │    243 │     18 │          0 │   244 │     0 │                 0 │      16 │     3,268 │ (a, b)=(2, 1752) │
│ 16 │    12 │     1 │    249 │     18 │          0 │   249 │     0 │                 0 │      16 │     3,168 │ ∅                │  <--- ...and then skipping to here without scanning all those other pages
└────┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴──────────────────┘
(16 rows)
*/

-- This is :rootitems, just to avoid test regressions:
select itemoffset, ctid, itemlen, nulls from bt_page_items('keys_ahead_idx',
  (select fastroot::int4 from bt_metap('keys_ahead_idx')));

-- 5 index hits because we descend once (root), access leaf page (13), gamble and waste
-- an access (2) but notice and redescend (root again), then finally touch
-- rightmost leaf page (12)
select *
  from keys_ahead
where
  a in (1, 2)
  and b = 1999;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 5 index hits
select *
  from keys_ahead
where
  a in (1, 2)
  and b = 1999;

-- Same again, but now it's lower matching b value of 1990:
-- (Somewhat surprisingly, this variant independently remained broken after
-- the original was fixed)
select *
  from keys_ahead
where
  a in (1, 2)
  and b = 1990;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 5 index hits
select *
  from keys_ahead
where
  a in (1, 2)
  and b = 1990;

-- Same again, but now it's final matching b value of 2000:
select *
  from keys_ahead
where
  a in (1, 2)
  and b = 2000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 5 index hits
select *
  from keys_ahead
where
  a in (1, 2)
  and b = 2000;

-- Same again, but now it's non-matching b value of 2001:
select *
  from keys_ahead
where
  a in (1, 2)
  and b = 2001;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 5 index hits
select *
  from keys_ahead
where
  a in (1, 2)
  and b = 2001;

-- Same again, but > instead of = on truncated "b" attribute:
select *
  from keys_ahead
where
  a in (1, 2)
  and b > 1999;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
  from keys_ahead
where
  a in (1, 2)
  and b > 1999;

-- Same again, but >= instead of = on truncated "b" attribute:
select *
  from keys_ahead
where
  a in (1, 2)
  and b >= 1999;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
  from keys_ahead
where
  a in (1, 2)
  and b >= 1999;

-- Same again, but > instead of = on truncated "b" attribute (sort of):
select *
  from keys_ahead
where
  a in (1, 2)
  and b > 1990 and b < 1999;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
  from keys_ahead
where
  a in (1, 2)
  and b > 1990 and b < 1999;

-- Same again, but < uses value higher than any real value:
select *
  from keys_ahead
where
  a in (1, 2)
  and b > 1990 and b < 2005;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
  from keys_ahead
where
  a in (1, 2)
  and b > 1990 and b < 2005;

-- Same again, but >= instead of = on truncated "b" attribute (sort of):
select *
  from keys_ahead
where
  a in (1, 2)
  and b >= 1990 and b <= 1999;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
  from keys_ahead
where
  a in (1, 2)
  and b >= 1990 and b <= 1999;

-- Same again, but <= uses value higher than any real value:
select *
  from keys_ahead
where
  a in (1, 2)
  and b >= 1990 and b <= 2005;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
  from keys_ahead
where
  a in (1, 2)
  and b >= 1990 and b <= 2005;

-------------------------------------------------------------------------------------------------------
-- Don't be too conservative about disabling optimization with low order column lacking SK_BT_REQFWD --
-------------------------------------------------------------------------------------------------------

set client_min_messages=error;
drop table if exists dont_be_too_conservative;
reset client_min_messages;
create unlogged table dont_be_too_conservative(
  a int,
  b int,
  c int
);
create index dont_be_too_conservative_idx on dont_be_too_conservative(a, b, c);
insert into dont_be_too_conservative select i, i, i from generate_series(1,500) i;
vacuum analyze dont_be_too_conservative;

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Here the column c is in search type scan key, but isn't a SK_BT_REQFWD
-- column:
select * from dont_be_too_conservative where a in (2,3,4,5,6,7,8) and c = 7;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from dont_be_too_conservative where a in (2,3,4,5,6,7,8) and c = 7; -- just 2 buffer hits

----------------------------------------------
-- Don't get confused by NULLs FIRST column --
----------------------------------------------
set client_min_messages=error;
drop table if exists nulls_first;
reset client_min_messages;
create unlogged table nulls_first(
  district int4,
  warehouse int4,
  orderid int4,
  anotherorderid int4,
  orderline int4
);
create index nulls_first_idx on nulls_first(district, warehouse, orderid nulls first, anotherorderid, orderline);

insert into nulls_first
select district, warehouse, NULL, orderid, orderline
from
  generate_series(1, 3) district,
  generate_series(1, 5) warehouse,
  generate_series(1, 15) orderid,
  generate_series(1, 10) orderline
order by
district, warehouse, orderid, orderline;

-- prewarm
select count(*) from nulls_first;
vacuum analyze nulls_first;

-- looks like this now:
--
-- ┌────┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬────────────────────────────────────────────────────────────────────────────┐
-- │ i  │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │                                  highkey                                   │
-- ├────┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼────────────────────────────────────────────────────────────────────────────┤
-- │  1 │     1 │     1 │    200 │      2 │          0 │   201 │     0 │                 0 │      32 │       912 │ (district, warehouse, orderid, anotherorderid, orderline)=(1, 2, null, 6)  │
-- │  2 │     2 │     1 │    200 │      2 │          0 │   201 │     0 │                 0 │      32 │       912 │ (district, warehouse, orderid, anotherorderid, orderline)=(1, 3, null, 11) │
-- │  3 │     4 │     1 │    200 │      2 │          0 │   201 │     0 │                 0 │      31 │       928 │ (district, warehouse, orderid, anotherorderid, orderline)=(1, 5)           │
-- │  4 │     5 │     1 │    200 │      2 │          0 │   201 │     0 │                 0 │      32 │       912 │ (district, warehouse, orderid, anotherorderid, orderline)=(2, 1, null, 6)  │
-- │  5 │     6 │     1 │    200 │      2 │          0 │   201 │     0 │                 0 │      32 │       912 │ (district, warehouse, orderid, anotherorderid, orderline)=(2, 2, null, 11) │
-- │  6 │     7 │     1 │    200 │      2 │          0 │   201 │     0 │                 0 │      31 │       928 │ (district, warehouse, orderid, anotherorderid, orderline)=(2, 4)           │
-- │  7 │     8 │     1 │    200 │      2 │          0 │   201 │     0 │                 0 │      32 │       912 │ (district, warehouse, orderid, anotherorderid, orderline)=(2, 5, null, 6)  │
-- │  8 │     9 │     1 │    200 │      2 │          0 │   201 │     0 │                 0 │      32 │       912 │ (district, warehouse, orderid, anotherorderid, orderline)=(3, 1, null, 11) │
-- │  9 │    10 │     1 │    200 │      2 │          0 │   201 │     0 │                 0 │      31 │       928 │ (district, warehouse, orderid, anotherorderid, orderline)=(3, 3)           │
-- │ 10 │    11 │     1 │    200 │      2 │          0 │   201 │     0 │                 0 │      32 │       912 │ (district, warehouse, orderid, anotherorderid, orderline)=(3, 4, null, 6)  │
-- │ 11 │    12 │     1 │    200 │      2 │          0 │   201 │     0 │                 0 │      32 │       912 │ (district, warehouse, orderid, anotherorderid, orderline)=(3, 5, null, 11) │
-- │ 12 │    13 │     1 │     50 │      2 │          0 │    50 │     0 │                 0 │      32 │     6,348 │ ∅                                                                          │
-- └────┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴────────────────────────────────────────────────────────────────────────────┘
--
-----------------------------------------------------------------------

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

select ctid, * from nulls_first
where
  district = 1
  and warehouse = 3
  and orderid is null
  and anotherorderid in (9, 10)
  and orderline in (8, 9, 10, 11);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, * from nulls_first
where
  district = 1
  and warehouse = 3
  and orderid is null
  and anotherorderid in (9, 10)
  and orderline in (8, 9, 10, 11);

-- Now try IS NOT NULL variant:
select ctid, * from nulls_first
where
  district = 1
  and warehouse = 3
  and orderid is not null
  and anotherorderid in (9, 10)
  and orderline in (8, 9, 10, 11);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, * from nulls_first
where
  district = 1
  and warehouse = 3
  and orderid is not null
  and anotherorderid in (9, 10)
  and orderline in (8, 9, 10, 11);

select ctid, * from nulls_first where district = 1 and warehouse = 5 and orderid is null and anotherorderid in (11,12) and orderline in (8, 9, 10, 11);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, * from nulls_first where district = 1 and warehouse = 5 and orderid is null and anotherorderid in (11,12) and orderline in (8, 9, 10, 11);

select ctid, * from nulls_first where district = 1 and warehouse = 5 and orderid is not null and anotherorderid in (11,12) and orderline in (8, 9, 10, 11);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, * from nulls_first where district = 1 and warehouse = 5 and orderid is not null and anotherorderid in (11,12) and orderline in (8, 9, 10, 11);

-- RowCompare variant -- detects unsafe mixing of RowCompareExpr clauses with
-- ScalarArrayOpExr caluses
--
-- We want to exercise plans with a combination of RowCompareExpr and ScalarArrayOpExr.

select ctid, * from nulls_first where (district, warehouse) >= (3,3) and orderid is null and anotherorderid = any ('{1,2}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, * from nulls_first where (district, warehouse) >= (3,3) and orderid is null and anotherorderid = any ('{1,2}');

-- The same row constructor syntax works automatically (this doesn't even appear
-- as a RowCompare clause in the optimizer):
select ctid, * from nulls_first where (district, warehouse) = (3,3) and orderid is null and anotherorderid =  any ('{1,2}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, * from nulls_first where (district, warehouse) = (3,3) and orderid is null and anotherorderid =  any ('{1,2}');

-- (September 8) Here we show a distilled case that demonstrates the need for
-- an opclass support function 1 (ORDER function) for every column that's
-- BTEqualStrategyNumber -- not just those that are SK_SEARCHARRAY array keys.
-- Recall that this was surprisingly unlikely to break queries.
--
-- If you remove the required comparator and just skip over relevant scan
-- keys when checking if a tuple needs to advance the array keys, you'll find
-- that this query apparently works as expected:
--
-- (November 22) UPDATE:
-- XXX Not really true anymore, as of recent versions of the patch (those
-- after v7) which can get by using _bt_check_compare/the = operator most of
-- the time -- just not all the time.  (This is an indirect consequence of
-- having more worked out handling of required inequality scan keys.)
select ctid, *
from nulls_first
where district = 1
  and warehouse = 5
  and orderid is null
  and anotherorderid = any ('{6}')
  and orderline = any ('{-5,500}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, *
from nulls_first
where district = 1
  and warehouse = 5
  and orderid is null
  and anotherorderid = any ('{6}')
  and orderline = any ('{-5,500}');
-- OTOH this very similar query fails:
select ctid, *
from nulls_first
where district = 1
  and warehouse = 5
  and orderid is null
  and anotherorderid = any ('{7}')
  and orderline = any ('{-5,500}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, *
from nulls_first
where district = 1
  and warehouse = 5
  and orderid is null
  and anotherorderid = any ('{7}')
  and orderline = any ('{-5,500}');

-- The actual reason why the first query "succeeds" is that the page high key
-- for the relevant leaf page (block 5) looks like this:
--
-- (district, warehouse, orderid, anotherorderid, orderline)=(2, 1, null, 6)
--
-- So the only reason why the first variant would "succeed" was because
-- an "anotherorderid" of 6 made the high key seem to be within the bounds of
-- the array keys for the first query, but not the second query.  The second
-- query would repeat its access to page 5 because the state machine had the
-- wrong idea about our progress in the key space.
--
-- An additional complicating factor here is the interaction with suffix
-- truncation.  This variant of the failing query lacks "orderline = any
-- ('{-5,500}')", but is otherwise identical -- and so it always worked as
-- expected, even with the bug present:
select ctid, *
from nulls_first
where district = 1
  and warehouse = 5
  and orderid is null
  and anotherorderid = any ('{7}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select ctid, *
from nulls_first
where district = 1
  and warehouse = 5
  and orderid is null
  and anotherorderid = any ('{7}');

--------------------------------------------------
-- Mark/restore ScalarArrayOpExr coverage tests --
--------------------------------------------------

set client_min_messages=error;
drop table if exists mark_restore_join_table1;
drop table if exists mark_restore_join_table2;
reset client_min_messages;

set enable_nestloop to 0;
set enable_hashjoin to 0;
set enable_sort to 0;
set enable_material to 0;

create unlogged table mark_restore_join_table1 (a int, b int);
create unlogged table mark_restore_join_table2 (a int, b int);
create index table1_idx on mark_restore_join_table1 (a) where a % 1000 = 1;
create index table2_idx on mark_restore_join_table2 (a) where a % 1000 = 1;

-- (July 13) Original regression tests had only 2 rows, I want more:
insert into mark_restore_join_table1 select 1, i from generate_series(1, 20) i;
insert into mark_restore_join_table2 select 1, i from generate_series(1, 20) i;

vacuum analyze mark_restore_join_table1;
vacuum analyze mark_restore_join_table2;

-- Bitmap index scan:
set enable_bitmapscan to on;
set enable_indexonlyscan to off;
set enable_indexscan to off;

-- Exercise array keys mark/restore B-Tree code
select j1.ctid as j1_ctid, j2.ctid as j2_ctid, * from
  mark_restore_join_table1 j1
  inner join mark_restore_join_table2 j2 on j1.a = j2.a and j1.b = j2.b
where
  j1.a % 1000 = 1 and j2.a % 1000 = 1 and j2.a = any (array[1]);

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select j1.ctid as j1_ctid, j2.ctid as j2_ctid, * from
  mark_restore_join_table1 j1
  inner join mark_restore_join_table2 j2 on j1.a = j2.a and j1.b = j2.b
where
  j1.a % 1000 = 1 and j2.a % 1000 = 1 and j2.a = any (array[1]);

-- Exercise array keys "find extreme element" B-Tree code
select j1.ctid as j1_ctid, j2.ctid as j2_ctid, * from
  mark_restore_join_table1 j1
  inner join mark_restore_join_table2 j2 on j1.a = j2.a and j1.b = j2.b
where
  j1.a % 1000 = 1 and j2.a % 1000 = 1 and j2.a >= any (array[1, 5]);

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select j1.ctid as j1_ctid, j2.ctid as j2_ctid, * from
  mark_restore_join_table1 j1
  inner join mark_restore_join_table2 j2 on j1.a = j2.a and j1.b = j2.b
where
  j1.a % 1000 = 1 and j2.a % 1000 = 1 and j2.a >= any (array[1, 5]);

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Exercise array keys mark/restore B-Tree code
-- As above
select j1.ctid as j1_ctid, j2.ctid as j2_ctid, * from
  mark_restore_join_table1 j1
  inner join mark_restore_join_table2 j2 on j1.a = j2.a and j1.b = j2.b
where
  j1.a % 1000 = 1 and j2.a % 1000 = 1 and j2.a = any (array[1]);

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select j1.ctid as j1_ctid, j2.ctid as j2_ctid, * from
  mark_restore_join_table1 j1
  inner join mark_restore_join_table2 j2 on j1.a = j2.a and j1.b = j2.b
where
  j1.a % 1000 = 1 and j2.a % 1000 = 1 and j2.a = any (array[1]);

-- Exercise array keys "find extreme element" B-Tree code
-- As above
select j1.ctid as j1_ctid, j2.ctid as j2_ctid, * from
  mark_restore_join_table1 j1
  inner join mark_restore_join_table2 j2 on j1.a = j2.a and j1.b = j2.b
where
  j1.a % 1000 = 1 and j2.a % 1000 = 1 and j2.a >= any (array[1, 5]);

EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select j1.ctid as j1_ctid, j2.ctid as j2_ctid, * from
  mark_restore_join_table1 j1
  inner join mark_restore_join_table2 j2 on j1.a = j2.a and j1.b = j2.b
where
  j1.a % 1000 = 1 and j2.a % 1000 = 1 and j2.a >= any (array[1, 5]);

-- (September 22)
-- Test case that caused assertion failure related to not
-- having the right place in the scan for a merge join, in respect of
-- non-array equality-type scan keys.
set client_min_messages=error;
drop table if exists mark_restore_self_join;
reset client_min_messages;

create unlogged table mark_restore_self_join (a int, b int);
create index on mark_restore_self_join(a, b);

insert into mark_restore_self_join select 1, i from generate_series(1, 20) i;

vacuum analyze mark_restore_self_join;

-- Original:
select j1.ctid as j1_ctid, j2.ctid as j2_ctid, *
from
  mark_restore_self_join j1
    inner join
  mark_restore_self_join j2 on j1.a = j2.a
where j2.a = any (array[-1, 0, 1, 2, 3]) and j2.b = 5;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select j1.ctid as j1_ctid, j2.ctid as j2_ctid, *
from
  mark_restore_self_join j1
    inner join
  mark_restore_self_join j2 on j1.a = j2.a
where j2.a = any (array[-1, 0, 1, 2, 3]) and j2.b = 5;

-- Same query, but now a nestloop join:
--
-- (March 4) This variant was buggy following merge of _bt_preprocess_keys and
-- _bt_preprocess_array_keys, though it might have been a preexisiting issue
-- (coincided with getting serious about testing nestloop joins).
--
-- XXX UPDATE: Derp, this was due to a simple lack of rigor here (should have
-- been testing both scan keys from the pair being considered):

/*
--- a/src/backend/access/nbtree/nbtutils.c
+++ b/src/backend/access/nbtree/nbtutils.c
@@ -2694,7 +2694,8 @@ _bt_preprocess_keys(IndexScanDesc scan, ScanDirection dir)
             if (j == (BTEqualStrategyNumber - 1) &&
-                (xform[j].skey->sk_flags & SK_SEARCHARRAY) &&
+                ((xform[j].skey->sk_flags & SK_SEARCHARRAY) ||
+                 (cur->sk_flags & SK_SEARCHARRAY)) &&
                 !(cur->sk_flags & SK_SEARCHNULL))
             {
*/
set enable_mergejoin to off;

select j1.ctid as j1_ctid, j2.ctid as j2_ctid, *
from
  mark_restore_self_join j1
    inner join
  mark_restore_self_join j2 on j1.a = j2.a
where j2.a = any (array[-1, 0, 1, 2, 3]) and j2.b = 5;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select j1.ctid as j1_ctid, j2.ctid as j2_ctid, *
from
  mark_restore_self_join j1
    inner join
  mark_restore_self_join j2 on j1.a = j2.a
where j2.a = any (array[-1, 0, 1, 2, 3]) and j2.b = 5;

set enable_mergejoin to on;

-- (September 24) Repro for mark/restore bug affecting HEAD and all back branches
-- Per https://postgr.es/m/CAH2-WzkgP3DDRJxw6DgjCxo-cu-DKrvjEv_ArkP2ctBJatDCYg@mail.gmail.com
set client_min_messages=error;
drop table if exists amber_small;
drop table if exists amber_big;
reset client_min_messages;

create unlogged table amber_small
(
  a integer,
  b integer
);

create unlogged table amber_big
(
  a integer,
  b integer
);

insert into amber_big select 1,  2 from generate_series(1,1024);
insert into amber_big select 1,  3 from generate_series(1,1024);
insert into amber_big select 1,  5 from generate_series(1,1024);
insert into amber_big select 1,  6 from generate_series(1,1024);
insert into amber_big select 1,  7 from generate_series(1,1024);
insert into amber_big select 1,  8 from generate_series(1,1024);
insert into amber_big select 1, 10 from generate_series(1,1024);
insert into amber_big select 1, 12 from generate_series(1,1024);
insert into amber_big select 1, 13 from generate_series(1,1024);
insert into amber_big select 1, 15 from generate_series(1,1024);
insert into amber_big select 1, 17 from generate_series(1,1024);
insert into amber_big select 1, 19 from generate_series(1,1024);

insert into amber_small select 1,  1 from generate_series(1,8);
insert into amber_small select 1,  2 from generate_series(1,8);
insert into amber_small select 1,  3 from generate_series(1,8);
insert into amber_small select 1,  4 from generate_series(1,8);
insert into amber_small select 1,  5 from generate_series(1,8);
insert into amber_small select 1,  9 from generate_series(1,8);
insert into amber_small select 1, 10 from generate_series(1,8);
insert into amber_small select 1, 11 from generate_series(1,8);
insert into amber_small select 1, 12 from generate_series(1,8);
insert into amber_small select 1, 14 from generate_series(1,8);
insert into amber_small select 1, 17 from generate_series(1,8);
insert into amber_small select 1, 18 from generate_series(1,8);
insert into amber_small select 1, 19 from generate_series(1,8);

create index amber_big_idx on amber_big (a, b);
create index amber_small_idx on amber_small (a, b);

vacuum analyze amber_small;
vacuum analyze amber_big;

-- Original
select count(*), small.a small_a
from
  amber_small small
    inner join
  amber_big big
    on small.a = big.a and small.b = big.b
where small.a in (1, 3) and big.a in (1, 3)
group by small_a order by small_a;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*), small.a small_a
from
  amber_small small
    inner join
  amber_big big
    on small.a = big.a and small.b = big.b
where small.a in (1, 3) and big.a in (1, 3)
group by small_a order by small_a;

-- Same again, but this time it's a nestloop join:
--
-- (March 4) This variant was buggy following merge of _bt_preprocess_keys and
-- _bt_preprocess_array_keys, though it might have been a preexisiting issue
-- (coincided with getting serious about testing nestloop joins).
--
-- XXX UPDATE: This bug was a very recent regression from
-- March 2 or March 3, directly tied to the aforementioned merging of code
-- from _bt_preprocess_keys and _bt_preprocess_array_keys.  Basically,
-- the offset in array->scan_key was confused in some way, something to do
-- with not correctly remapping from input scan key offsets to output scan key
-- offsets in the presence of redundant/contradictory scan keys.
set enable_mergejoin to off;
select count(*), small.a small_a
from
  amber_small small
    inner join
  amber_big big
    on small.a = big.a and small.b = big.b
where small.a in (1, 3) and big.a in (1, 3)
group by small_a order by small_a;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*), small.a small_a
from
  amber_small small
    inner join
  amber_big big
    on small.a = big.a and small.b = big.b
where small.a in (1, 3) and big.a in (1, 3)
group by small_a order by small_a;
set enable_mergejoin to on;

-- (November 29)
--
-- Reveal bug in mark/restore processing that's related to November 25 test
-- case, except it involves mark/restore rather than changing the scan
-- direction
set client_min_messages=error;
drop table if exists outer_table;
drop table if exists restore_buggy_primscan_table;
reset client_min_messages;

create unlogged table outer_table                  (a int, b int);
create unlogged table restore_buggy_primscan_table (x int, y int);

-- Disable deduplication because uniform sized tuples helped with repro
-- (leaving this on is now unnecessary but can't hurt):
create index buggy_idx on restore_buggy_primscan_table (x, y) with (deduplicate_items=off);

-- Originally I didn't even have an index on outer_table, but found that sort
-- node produced unstable output depending on whether build was debug or not:
create index avoid_sort on outer_table (a, b);

insert into outer_table                  select 1, b_vals from generate_series(1006, 1580) b_vals;
insert into restore_buggy_primscan_table select 1, x_vals from generate_series(1006, 1580) x_vals;

-- "9" is the bare minimum number of tuples that'll repro:
insert into outer_table                  select 1, 1370 from generate_series(1, 9) j;
insert into restore_buggy_primscan_table select 1, 1371 from generate_series(1, 9) j;
insert into restore_buggy_primscan_table select 1, 1380 from generate_series(1, 9) j;

vacuum analyze outer_table;
vacuum analyze restore_buggy_primscan_table;

prepare restore_buggy_primscan_qry as
select count(*), o.a, o.b
  from
    outer_table o
  inner join
    restore_buggy_primscan_table bug
  on o.a = bug.x and o.b = bug.y
where
  bug.x = 1 and
  bug.y = any(array[(select array_agg(i) from generate_series(1370, 1390) i where i % 10 = 0)])
group by o.a, o.b;

execute restore_buggy_primscan_qry;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute restore_buggy_primscan_qry;
deallocate restore_buggy_primscan_qry;

-- (Jun 6 2024)
--
-- Same again, but using skip scan this time.
prepare skip_restore_buggy_primscan_qry as
select count(*), o.a, o.b
  from
    outer_table o
  inner join
    restore_buggy_primscan_table bug
  on o.a = bug.x and o.b = bug.y
where
  bug.y = any(array[(select array_agg(i) from generate_series(1370, 1390) i where i % 10 = 0)])
group by o.a, o.b;

execute skip_restore_buggy_primscan_qry;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute skip_restore_buggy_primscan_qry;

-- (Jun 6 2024)
--
-- Fill with tuples match original "x=1" tuples, but with "x=0" and "x=2"
-- (i.e. enclose original "x=1" tuples so we have to do some skipping).
insert into outer_table                  select 0, b_vals from generate_series(1006, 1580) b_vals;
insert into restore_buggy_primscan_table select 0, x_vals from generate_series(1006, 1580) x_vals;
insert into outer_table                  select 0, 1370 from generate_series(1, 9) j;
insert into restore_buggy_primscan_table select 0, 1371 from generate_series(1, 9) j;
insert into restore_buggy_primscan_table select 0, 1380 from generate_series(1, 9) j;
insert into outer_table                  select 2, b_vals from generate_series(1006, 1580) b_vals;
insert into restore_buggy_primscan_table select 2, x_vals from generate_series(1006, 1580) x_vals;
insert into outer_table                  select 2, 1370 from generate_series(1, 9) j;
insert into restore_buggy_primscan_table select 2, 1371 from generate_series(1, 9) j;
insert into restore_buggy_primscan_table select 2, 1380 from generate_series(1, 9) j;

execute skip_restore_buggy_primscan_qry;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute skip_restore_buggy_primscan_qry;

deallocate skip_restore_buggy_primscan_qry;

-- (November 30)
--
-- Get test coverage for when so->needPrimScan is set at the point of calling
-- _bt_restore_array_keys().  This is handled like the case where the scan
-- direction changes "within" a page, relying on code from _bt_readnextpage().
set client_min_messages=error;
drop table if exists outer_tab;
drop table if exists primscanmarkcov_table;
reset client_min_messages;

create unlogged table outer_tab(
  a int,
  b int
);
create index outer_tab_idx on outer_tab(a, b) with (deduplicate_items = off);

create unlogged table primscanmarkcov_table(
  a int,
  b int
);
create index interesting_coverage_idx on primscanmarkcov_table(a, b) with (deduplicate_items = off);

insert into outer_tab             select 1, i from generate_series(1530, 1780) i;
insert into primscanmarkcov_table select 1, i from generate_series(1530, 1780) i;

insert into outer_tab             select 1, 1550 from generate_series(1, 200) i;
insert into primscanmarkcov_table select 1, 1551 from generate_series(1, 200) i;

vacuum analyze outer_tab;
vacuum analyze primscanmarkcov_table ;

prepare merge_join_qry as
with range_ints as ( select i from generate_series(1530, 1780) i)
select
  count(*), buggy.a, buggy.b from
outer_tab o
  inner join
primscanmarkcov_table buggy
  on o.a = buggy.a and o.b = buggy.b
where
  o.a = 1     and     o.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])  and
  buggy.a = 1 and buggy.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])
group by buggy.a, buggy.b
order by buggy.a, buggy.b;

execute merge_join_qry;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) <-- Disable sept 28 2024
-- execute merge_join_qry;
deallocate merge_join_qry;

-- (Jun 6 2024)
--
-- Fill with tuples match original "x=1" tuples, but with "x=0" and "x=2"
-- (i.e. enclose original "x=1" tuples so we have to do some skipping).
insert into outer_tab             select 0, i from generate_series(1530, 1780) i;
insert into primscanmarkcov_table select 0, i from generate_series(1530, 1780) i;

insert into outer_tab             select 0, 1550 from generate_series(1, 200) i;
insert into primscanmarkcov_table select 0, 1551 from generate_series(1, 200) i;

insert into outer_tab             select 2, i from generate_series(1530, 1780) i;
insert into primscanmarkcov_table select 2, i from generate_series(1530, 1780) i;

insert into outer_tab             select 2, 1550 from generate_series(1, 200) i;
insert into primscanmarkcov_table select 2, 1551 from generate_series(1, 200) i;

vacuum analyze outer_tab;
vacuum analyze primscanmarkcov_table ;

prepare skip_merge_join_qry as
with range_ints as ( select i from generate_series(1530, 1780) i)
select
  count(*), buggy.a, buggy.b from
outer_tab o
  inner join
primscanmarkcov_table buggy
  on o.a = buggy.a and o.b = buggy.b
where
  o.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])  and
  buggy.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])
group by buggy.a, buggy.b
order by buggy.a, buggy.b;

execute skip_merge_join_qry;
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
-- execute skip_merge_join_qry;
deallocate skip_merge_join_qry;

-- (March 18)
--
-- Try to make a merge join go backwards with a cursor +
-- restore_buggy_primscan_qry query

-- Index-only scan (makes picture vs master clearer than a plain index scan
-- for these merge join tests):
set enable_bitmapscan to off;
set enable_indexonlyscan to on;
set enable_indexscan to off;
set work_mem = 64;
set cursor_tuple_fraction = 0.0001;

begin;
-- XXX (November 6 2024) Disable EXPLAIN ANALYZE output to suppress test
-- flappiness due to "Storage: Memory  Maximum Storage: 26kB" variations
-- across debug and release builds.
--
-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
-- declare restore_buggy_primscan_qry_cursor cursor for
-- with range_ints as ( select i from generate_series(1530, 1780) i)
-- select
--   buggy.a, buggy.b from
-- outer_tab o
--   inner join
-- primscanmarkcov_table buggy
--   on o.a = buggy.a and o.b = buggy.b
-- where
--   o.a = 1     and     o.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])  and
--   buggy.a = 1 and buggy.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])
-- order by buggy.a, buggy.b;

declare restore_buggy_primscan_qry_cursor cursor for
with range_ints as ( select i from generate_series(1530, 1780) i)
select
  buggy.a, buggy.b from
outer_tab o
  inner join
primscanmarkcov_table buggy
  on o.a = buggy.a and o.b = buggy.b
where
  o.a = 1     and     o.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])  and
  buggy.a = 1 and buggy.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])
order by buggy.a, buggy.b;

select pg_stat_get_xact_blocks_hit('buggy_idx'::regclass) as cur_blocks_hit \gset
\set old_blocks_hit :cur_blocks_hit

-- Show one row matching (a,b)=(1, 1550):
fetch forward 1 from restore_buggy_primscan_qry_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('buggy_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Skip all later such rows (201 total, so skip 200):
move forward 200 in restore_buggy_primscan_qry_cursor;

-- Show one row matching (a,b)=(1, 1600):
fetch forward 1 from restore_buggy_primscan_qry_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('buggy_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Show one row matching (a,b)=(1, 1650):
fetch forward 1 from restore_buggy_primscan_qry_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('buggy_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Show one row matching (a,b)=(1, 1700):
fetch forward 1 from restore_buggy_primscan_qry_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('buggy_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- Show one row matching (a,b)=(1, 1750):
fetch forward 1 from restore_buggy_primscan_qry_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('buggy_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

-- No more rows to show:
fetch forward 1 from restore_buggy_primscan_qry_cursor;
\set old_blocks_hit :cur_blocks_hit
select pg_stat_get_xact_blocks_hit('buggy_idx'::regclass) as cur_blocks_hit \gset
select :cur_blocks_hit - :old_blocks_hit bh;

fetch backward 1 from restore_buggy_primscan_qry_cursor;

/* restore_buggy_primscan_qry_cursor */ commit;

reset work_mem;


set enable_mergejoin = off;
set enable_memoize = off;
set enable_nestloop = on;

-- Now do nestloopjoin_qry, which is exactly the same queru as merge_join_qry,
-- but has a nestloop plan
--
-- (March 4) This variant was buggy following merge of _bt_preprocess_keys and
-- _bt_preprocess_array_keys, though it might have been a preexisiting issue
-- (coincided with getting serious about testing nestloop joins).
--
-- XXX UPDATE: Derp, this was due to a simple lack of rigor here (should have
-- been testing both scan keys from the pair being considered):

/*
--- a/src/backend/access/nbtree/nbtutils.c
+++ b/src/backend/access/nbtree/nbtutils.c
@@ -2694,7 +2694,8 @@ _bt_preprocess_keys(IndexScanDesc scan, ScanDirection dir)
             if (j == (BTEqualStrategyNumber - 1) &&
-                (xform[j].skey->sk_flags & SK_SEARCHARRAY) &&
+                ((xform[j].skey->sk_flags & SK_SEARCHARRAY) ||
+                 (cur->sk_flags & SK_SEARCHARRAY)) &&
                 !(cur->sk_flags & SK_SEARCHNULL))
             {
*/
prepare nestloopjoin_qry as
with range_ints as ( select i from generate_series(1530, 1780) i)
select
  count(*), buggy.a, buggy.b from
outer_tab o
  inner join
primscanmarkcov_table buggy
  on o.a = buggy.a and o.b = buggy.b
where
  o.a = 1     and     o.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])  and
  buggy.a = 1 and buggy.b = any (array[(select array_agg(i) from range_ints where i % 50 = 0)])
group by buggy.a, buggy.b
order by buggy.a, buggy.b;

execute nestloopjoin_qry;

-- XXX (November 6 2024) Disable EXPLAIN ANALYZE output to suppress test
-- flappiness due to "Storage: Memory  Maximum Storage: 26kB" variations
-- across debug and release builds.

-- EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
-- execute nestloopjoin_qry;

deallocate nestloopjoin_qry;

-- (November 30)
--
-- Get test coverage for when so->needPrimScan is set at the point of calling
-- _bt_restore_array_keys() for backwards scans.  More or less comparable to
-- the last test.
set client_min_messages=error;
drop table if exists backwards_prim_outer_table;
drop table if exists backwards_restore_buggy_primscan_table;
reset client_min_messages;

create unlogged table backwards_prim_outer_table             (a int, b int);
create unlogged table backwards_restore_buggy_primscan_table (x int, y int);

create index backward_prim_buggy_idx  on backwards_restore_buggy_primscan_table (x, y) with (deduplicate_items=off);
create index backwards_prim_drive_idx on backwards_prim_outer_table             (a, b) with (deduplicate_items=off);

insert into backwards_prim_outer_table                  select 0, 1360;
insert into backwards_prim_outer_table                  select 1, b_vals from generate_series(1012, 1406) b_vals where b_vals % 10 = 0;
insert into backwards_prim_outer_table                  select 1, 1370;
vacuum analyze backwards_prim_outer_table; -- Be tidy

-- Fill up "backwards_prim_drive_idx" index with 396 items, just about fitting
-- onto its only page, which is a root leaf page:
insert into backwards_restore_buggy_primscan_table select 0, 1360;
insert into backwards_restore_buggy_primscan_table select 1, x_vals from generate_series(1012, 1406) x_vals;

-- Now cause two page splits, leaving 4 leaf pages in total:
insert into backwards_restore_buggy_primscan_table select 1, 1370 from generate_series(1,250) i;

vacuum analyze backwards_restore_buggy_primscan_table; -- Be tidy

-- Now buggy index looks like this:
--
-- ┌───┬───────┬───────┬────────┬────────┬────────────┬───────┬───────┬───────────────────┬─────────┬───────────┬──────────────────┐
-- │ i │ blkno │ flags │ nhtids │ nhblks │ ndeadhblks │ nlive │ ndead │ nhtidschecksimple │ avgsize │ freespace │     highkey      │
-- ├───┼───────┼───────┼────────┼────────┼────────────┼───────┼───────┼───────────────────┼─────────┼───────────┼──────────────────┤
-- │ 1 │     1 │     1 │    203 │      1 │          0 │   204 │     0 │                 0 │      16 │     4,068 │ (x, y)=(1, 1214) │
-- │ 2 │     4 │     1 │    156 │      2 │          0 │   157 │     0 │                 0 │      16 │     5,008 │ (x, y)=(1, 1370) │
-- │ 3 │     5 │     1 │    251 │      2 │          0 │   252 │     0 │                 0 │      16 │     3,108 │ (x, y)=(1, 1371) │
-- │ 4 │     2 │     1 │     36 │      1 │          0 │    36 │     0 │                 0 │      16 │     7,428 │ ∅                │
-- └───┴───────┴───────┴────────┴────────┴────────────┴───────┴───────┴───────────────────┴─────────┴───────────┴──────────────────┘

set enable_mergejoin=on;
set enable_nestloop=off;
prepare backwards_prim_confusion_qry as
select count(*), o.a, o.b
  from
    backwards_prim_outer_table o
  inner join
    backwards_restore_buggy_primscan_table bug
  on o.a = bug.x and o.b = bug.y
where
  bug.x in (0, 1) and
  bug.y = any(array[(select array_agg(i) from generate_series(1360, 1370) i where i % 10 = 0)])
group by o.a, o.b
order by o.a desc, o.b desc;

-- These are marks are restores seen for this query (this instrumentation is from
-- commit 4e24c585 on branch saop-dynamic-skip-v7.17):
--
-- WARNING:  marking   markPos.currPage: 4294967295, markPos.nextPage: 4294967295, currPos.currPage: 5, currPos.nextPage: 2
-- WARNING:  marking:  attno: 1, cur_elem/mark_elem: 1 (value 1)
-- WARNING:  marking:  attno: 2, cur_elem/mark_elem: 1 (value 1370)
-- WARNING:  restoring markPos.currPage: 5, markPos.nextPage: 2, currPos.currPage: 4, currPos.nextPage: 5
-- WARNING:            attno: 1, cur_elem: 0 (value 0), mark_elem: 1 (value 1)
--
-- (No more interesting mark and restores for this query, the reset omitted
-- for brevity)

execute backwards_prim_confusion_qry;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute backwards_prim_confusion_qry;
deallocate backwards_prim_confusion_qry;

reset enable_nestloop;
reset enable_mergejoin;
reset enable_hashjoin;
reset enable_sort;
reset enable_material;

----------------------------------------------------------------------------------------------
-- One duplicate value per leaf page must not visit too many extra leaf pages speculatively --
----------------------------------------------------------------------------------------------

set client_min_messages=error;
drop table if exists duplicate_test;
reset client_min_messages;

create unlogged table duplicate_test(dup int4);
create index on duplicate_test (dup);
insert into duplicate_test
select val from generate_series(1, 18) val,
                generate_series(1,1000) dups_per_val;
vacuum analyze duplicate_test; -- Be tidy

-- Index-only scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to on;
set enable_indexscan to off;

-- (December 3)
--
-- Use every third leaf page (not every second) here, so that we don't
-- conflate a waste leaf page visit with a repeat root page visit.
--
-- Recall how with "i % 2 = 0"/every-second-page-and-value, we'll visit the next leaf
-- page incorrectly, but then correctly repeat the same gamble and "win" the
-- second time.  We don't want to confuse that case for a "draw" against the
-- master branch -- the extra leaf pages are actually more expensive than repeat
-- root page accessed.  Making it every third page avoids that problem,
-- painting a clear picture.
--
-- This test is per https://postgr.es/m/CAH2-WzmtV7XEWxf_rP1pw=vyDjGLi__zGOy6Me5MovR3e1kfdg@mail.gmail.com
select count(*), dup from duplicate_test
where dup = any (array[( select array_agg(val) from generate_series(1, 20) val where val % 3 = 0)])
group by dup order by dup;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*), dup from duplicate_test
where dup = any (array[( select array_agg(val) from generate_series(1, 20) val where val % 3 = 0)])
group by dup order by dup;

-- But we should still be able to avoid every additional index descents after
-- the first, once the query has all constants from the index, which are
-- gapless (high key always finds an exact match for untruncated attributes,
-- so we're good):
select count(*), dup from duplicate_test
where dup = any (array[( select array_agg(val) from generate_series(1, 40) val)])
group by dup order by dup;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 20 hits vs 81 on master
select count(*), dup from duplicate_test
where dup = any (array[( select array_agg(val) from generate_series(1, 40) val)])
group by dup order by dup;

-- (March 9)
--
-- Make sure that we can manage with a mix of different operators when
-- determining redundancy of array keys.
--
-- Goal here is to render both of the inequalities as redundant in a way that
-- leaves array without either its lower or its higher elements, like so:

/*
_bt_preprocess_keys:  inkeys[0]: [ flags: [SK_SEARCHARRAY], attno: 1 func OID: 65 ]
_bt_preprocess_keys:  inkeys[1]: [ flags: [], attno: 1 func OID: 147 ]
_bt_preprocess_keys:  inkeys[2]: [ flags: [], attno: 1 func OID: 66 ]
_bt_preprocess_keys: outkeys[0]: [ flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD], attno: 1 func OID: 65 ]
_bt_preprocess_keys: scan->numberOfKeys is 3, so->numberOfKeys on output is 1
*/
select count(*), dup from duplicate_test
where dup = any (array[( select array_agg(val) from generate_series(1, 40) val)])
and dup > 8 and dup < 11
group by dup order by dup;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 4 hits (include one vm hit)
select count(*), dup from duplicate_test
where dup = any (array[( select array_agg(val) from generate_series(1, 40) val)])
and dup > 8 and dup < 11
group by dup order by dup;

-- Variant #1 (changes the order, not the true meaning):
select count(*), dup from duplicate_test
where
dup > 8 and
dup = any(array[( select array_agg(val) from generate_series(1, 40) val)]) and
dup < 11
group by dup order by dup;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 4 hits (include one vm hit)
select count(*), dup from duplicate_test
where
dup > 8 and
dup = any(array[( select array_agg(val) from generate_series(1, 40) val)]) and
dup < 11
group by dup order by dup;

-- Variant #2 (changes the order, not the true meaning):
select count(*), dup from duplicate_test
where
dup > 8 and
dup < 11 and
dup = any(array[( select array_agg(val) from generate_series(1, 40) val)])
group by dup order by dup;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 4 hits (include one vm hit)
select count(*), dup from duplicate_test
where
dup > 8 and
dup < 11 and
dup = any(array[( select array_agg(val) from generate_series(1, 40) val)])
group by dup order by dup;

-- Variant #3 (changes the order, not the true meaning):
select count(*), dup from duplicate_test
where
dup < 11 and
dup > 8 and
dup = any(array[( select array_agg(val) from generate_series(1, 40) val)])
group by dup order by dup;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 4 hits (include one vm hit)
select count(*), dup from duplicate_test
where
dup < 11 and
dup > 8 and
dup = any(array[( select array_agg(val) from generate_series(1, 40) val)])
group by dup order by dup;

--
-- (January 17)
--
-- Matthias assertion test case
set client_min_messages=error;
drop table if exists test_matthias_assert;
reset client_min_messages;
create unlogged table test_matthias_assert as
select i a, i b, i c from generate_series(1, 1000) i;
create index on test_matthias_assert(a, b, c);
vacuum analyze test_matthias_assert; -- be tidy

-- This was the assertion failure:
--
-- TRAP: failed Assert("!foundRequiredOppositeDirOnly"), File: "../source/src/backend/access/nbtree/nbtutils.c", Line: 1408, PID: 30438
-- 0   postgres                            0x0000000100e0fb74 ExceptionalCondition + 236
-- 1   postgres                            0x00000001006c45d8 _bt_advance_array_keys + 1024
-- 2   postgres                            0x00000001006c37c0 _bt_checkkeys + 716
-- 3   postgres                            0x00000001006b7720 _bt_readpage + 1232

select count(*) from test_matthias_assert
where a = any ('{1,2,3}') and b > 1 and c > 1
and b = any ('{1,2,3}');
explain (analyze, buffers, timing off, summary off)
select count(*) from test_matthias_assert
where a = any ('{1,2,3}') and b > 1 and c > 1
and b = any ('{1,2,3}');

--
-- (January 17)
--
-- Matthias test case for cross-type opfamily where redundant
set client_min_messages=error;
drop table if exists test_matthias_type_opclass_redundant;
reset client_min_messages;
create unlogged table test_matthias_type_opclass_redundant as
select generate_series(1, 10000, 1::bigint) num;
create index on test_matthias_type_opclass_redundant(num); /* bigint typed */
vacuum analyze test_matthias_type_opclass_redundant; -- be tidy

--
-- Qual might be broken due to application of smallint compare operator on int
-- values that do equal mod 2^16, but do not equal in their own type
--
select num from test_matthias_type_opclass_redundant
where num = any ('{1}'::smallint[])
  and num = any ('{1}'::int[])
  and num = any ('{65537}'::int[]);
explain (analyze, buffers, timing off, summary off)
select num from test_matthias_type_opclass_redundant
where num = any ('{1}'::smallint[])
  and num = any ('{1}'::int[])
  and num = any ('{65537}'::int[]);

-- Variant 1 (for good luck)
select num from test_matthias_type_opclass_redundant
where num = any ('{1}'::int[])
  and num = any ('{1}'::smallint[])
  and num = any ('{65537}'::int[]);
explain (analyze, buffers, timing off, summary off)
select num from test_matthias_type_opclass_redundant
where num = any ('{1}'::int[])
  and num = any ('{1}'::smallint[])
  and num = any ('{65537}'::int[]);

-- Variant 2 (for good luck)
select num from test_matthias_type_opclass_redundant
where num = any ('{65537}'::int[])
  and num = any ('{1}'::smallint[])
  and num = any ('{1}'::int[]);
explain (analyze, buffers, timing off, summary off)
select num from test_matthias_type_opclass_redundant
where num = any ('{65537}'::int[])
  and num = any ('{1}'::smallint[])
  and num = any ('{1}'::int[]);

-- (February 1) Assertion failure within _bt_preprocess_keys:
--
-- 2024-02-01 08:39:04.230 PST [15721][client backend] [[unknown]][3/2357:0] LOCATION:  PerformAuthentication, postinit.c:299
-- TRAP: failed Assert("(xform[j].skey->sk_flags & SK_SEARCHARRAY) == 0"), File: "../source/src/backend/access/nbtree/nbtutils.c", Line: 2587, PID: 15721
-- 0   postgres                            0x0000000103367f78 ExceptionalCondition + 236
-- 1   postgres                            0x0000000102c14cb4 _bt_preprocess_keys + 2724
prepare assert_failure_preprocess as
select num from test_matthias_type_opclass_redundant
where num = any ('{65537}'::int[])
  and num >= any ('{1}'::smallint[])
  and num >= any ('{1}'::int[]);

execute assert_failure_preprocess;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute assert_failure_preprocess;
deallocate assert_failure_preprocess;

--------------------
-- Opfamily tests --
--------------------

set client_min_messages=error;
drop table if exists opfamily_test;

drop operator family if exists test_family using btree cascade;
drop function if exists my_int2_sort(int2,int2) cascade;
drop function if exists my_int4_sort(int4,int4) cascade;
drop function if exists my_int8_sort(int8,int8) cascade;
drop function if exists my_int8_int2_sort(int8,int2) cascade;
drop function if exists my_int8_int4_sort(int8,int4) cascade;
drop function if exists my_int4_int8_sort(int4,int8) cascade;
reset client_min_messages;

-- Same-type procs:
create function my_int2_sort(int2,int2) returns int language sql
  as $$ select case when $1 = $2 then 0 when $1 > $2 then 1 else -1 end; $$;
create function my_int4_sort(int4,int4) returns int language sql
  as $$ select case when $1 = $2 then 0 when $1 > $2 then 1 else -1 end; $$;
create function my_int8_sort(int8,int8) returns int language sql
  as $$ select case when $1 = $2 then 0 when $1 > $2 then 1 else -1 end; $$;

-- Cross type procs (note that index will be int8):
create function my_int8_int2_sort(int8,int2) returns int language sql
  as $$ select case when $1 = $2 then 0 when $1 > $2 then 1 else -1 end; $$;
create function my_int8_int4_sort(int8,int4) returns int language sql
  as $$ select case when $1 = $2 then 0 when $1 > $2 then 1 else -1 end; $$;
create function my_int4_int8_sort(int4,int8) returns int language sql
  as $$ select case when $1 = $2 then 0 when $1 > $2 then 1 else -1 end; $$;

create operator family test_family using btree;

create operator class test_int2_ops for type int2 using btree family test_family as
  operator 1 < (int2,int2),
  operator 2 <= (int2,int2),
  operator 3 = (int2,int2),
  operator 4 >= (int2,int2),
  operator 5 > (int2,int2),
  function 1 my_int2_sort(int2,int2);
  -- Will add cross-type operators + function for int2 later on

create operator class test_int4_ops for type int4 using btree family test_family as
  operator 1 < (int4,int4),
  operator 2 <= (int4,int4),
  operator 3 = (int4,int4),
  operator 4 >= (int4,int4),
  operator 5 > (int4,int4),
  function 1 my_int4_sort(int4,int4);
  -- Will add cross-type operators + function for int4 later on

create operator class test_int8_ops for type int8 using btree family test_family as
  operator 1 < (int8,int8),
  operator 2 <= (int8,int8),
  operator 3 = (int8,int8),
  operator 4 >= (int8,int8),
  operator 5 > (int8,int8),
  function 1 my_int8_sort(int8,int8);

create unlogged table opfamily_test(foo int8);
create index on opfamily_test(foo test_int8_ops);
insert into opfamily_test values (365), (366), (367), (32767), (8589934591), (8589934592);
vacuum analyze opfamily_test; -- Be tidy

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;
select * from opfamily_test where foo = any ('{365,367}'::int8[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from opfamily_test where foo = any ('{365,367}'::int8[]);

-- Seqscan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to off;
set enable_seqscan to on;
select * from opfamily_test where foo = any ('{365,367}'::int4[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from opfamily_test where foo = any ('{365,367}'::int4[]);
set enable_seqscan to off;

-- Add the missing cross-type operator, but neglect to add a support function 1 to go
-- along with it:
alter operator family test_family using btree add
  operator 3 = (int8, int4);

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- Planner gives us an index scan this time, fails at runtime in nbtree
-- preprocessing:
-- (The error matches between master and patch, though it actually comes from
-- _bt_first in the case of master and from array preprocessing in the case
-- of the patch)
EXPLAIN
select * from opfamily_test where foo = any ('{365,367}'::int4[]);
select * from opfamily_test where foo = any ('{365,367}'::int4[]);

-- Add missing support function:
alter operator family test_family using btree add
  function 1 my_int8_int4_sort(int8, int4);

-- Now both planner and preprocessing don't error out, query works:
EXPLAIN
select * from opfamily_test where foo = any ('{365,367}'::int4[]);
select * from opfamily_test where foo = any ('{365,367}'::int4[]);

-- (March 10) Don't fail to include both scan keys when there is a lack of
-- cross-type support that proves to _bt_preprocess_keys that it can just
-- discard the non-array type.
--
-- Create needed cross-type operators + proc:
alter operator family test_family using btree add
  operator 2 <= (int8, int4);
alter operator family test_family using btree add
  operator 3 = (int8, int2);
alter operator family test_family using btree add
  function 1 my_int8_int2_sort(int8,int2);

-- Difficulty here is that there are many common cases where this'll
-- accidentally fail to fail, provided we at least keep the original array
-- (discarding the non-array scan key can break during this test because of
-- basic confusion about data types, that likely wouldn't ever happen with =
-- operator).
EXPLAIN
select * from opfamily_test where foo = any ('{32767}'::int2[]) and foo <= (-1)::int4;
select * from opfamily_test where foo = any ('{32767}'::int2[]) and foo <= (-1)::int4;

-- These are just for good luck:
EXPLAIN
select * from opfamily_test where  foo <= (-1)::int4 and foo = any ('{32767}'::int2[]);
select * from opfamily_test where  foo <= (-1)::int4 and foo = any ('{32767}'::int2[]);
EXPLAIN
select * from opfamily_test where foo = any ('{32767}'::int2[]) and foo = (-1)::int4;
select * from opfamily_test where foo = any ('{32767}'::int2[]) and foo = (-1)::int4;
EXPLAIN
select * from opfamily_test where foo = (-1)::int4 and foo = any ('{32767}'::int2[]);
select * from opfamily_test where foo = (-1)::int4 and foo = any ('{32767}'::int2[]);

-- Cross-type redundancies across arrays test
--
-- (December 5) Test caused this assertion failure:
--
-- TRAP: failed Assert("so->arrayKeyData[prev->scan_key].sk_func.fn_oid == cur->sk_func.fn_oid"), File: "../source/src/backend/access/nbtree/nbtutils.c", Line: 446, PID: 1212966
-- postgres: pg regression [local] SELECT(ExceptionalCondition+0x75)[0x55f0f2bca415]
-- postgres: pg regression [local] SELECT(_bt_preprocess_array_keys+0x7f9)[0x55f0f278fc39]
select *
from opfamily_test
where
  foo = any ('{365,366}'::int4[]) and
  foo = any ('{366,367,8589934591,8589934592}'::int8[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from opfamily_test
where
  foo = any ('{365,366}'::int4[]) and
  foo = any ('{366,367,8589934591,8589934592}'::int8[]);

-- Variant for good luck
--
-- (January 27) This test proved that I needed to be more careful about the
-- order in which _bt_preprocess_keys appended output scan keys -- it needs
-- to match the input order exactly (we must not allow "swapping" of any pair
-- of not-provably-redundant scan keys), so that our current scan key -> order
-- proc mapping state works reliably later on.
--
-- Recall this stuff (inconsistent arguments between current search type scan
-- key and array scan keys/preprocessing's input scan keys):
--
-- petergeoghegan@regression:5432 [37539]=# select *
-- from opfamily_test
-- where
-- foo = any ('{366,367,8589934591,8589934592}'::int8[]) and
-- foo = any ('{365,366}'::int4[]);
-- ERROR:  3, ikey: 0, cur.sk_argument: 365, skeyarray.sk_argument: 366
select *
from opfamily_test
where
  foo = any ('{366,367,8589934591,8589934592}'::int8[]) and
  foo = any ('{365,366}'::int4[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from opfamily_test
where
  foo = any ('{366,367,8589934591,8589934592}'::int8[]) and
  foo = any ('{365,366}'::int4[]);

-- Other variants of original December 5 "Cross-type redundancies across arrays
-- test" test case (January 27)
select *
from opfamily_test
where
  foo = any ('{365,366}'::int8[]) and
  foo = any ('{366,367,2147480000,2147480001}'::int4[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from opfamily_test
where
  foo = any ('{365,366}'::int8[]) and
  foo = any ('{366,367,2147480000,2147480001}'::int4[]);

select *
from opfamily_test
where
  foo = any ('{366,367,2147480000,2147480001}'::int4[]) and
  foo = any ('{365,366}'::int8[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from opfamily_test
where
  foo = any ('{366,367,2147480000,2147480001}'::int4[]) and
  foo = any ('{365,366}'::int8[]);

--------------------------------
-- BooleanTest/BoolExpr tests --
--------------------------------
set client_min_messages=error;
drop table if exists boolindex;
reset client_min_messages;
create unlogged table boolindex (b bool, i int, unique(b, i), junk float);
insert into boolindex select (i % 2 = 0), i from generate_series(1, 10) i;

-- Index scan:
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;

-- "where b in ()" variants
select * from boolindex where b in (true, false) order by b, i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where b in (true, false) order by b, i limit 10;

select * from boolindex where b in (true, false) order by i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where b in (true, false) order by i limit 10;

-- "where b" variants (Just more Var coverage)
select * from boolindex where b and i in (2,4,5) order by b, i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where b and i in (2,4,5) order by b, i limit 10;

select * from boolindex where b and i in (2,4,6) order by b, i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where b and i in (2,4,6) order by b, i limit 10;

-- "where b = true" variants (Just more Var coverage)
select * from boolindex where b = true and i in (2,4,5) order by b, i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where b = true and i in (2,4,5) order by b, i limit 10;

select * from boolindex where b = true and i in (2,4,6) order by b, i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where b = true and i in (2,4,6) order by b, i limit 10;

-- "where b is true" variants (BooleanTest coverage)
select * from boolindex where b is true and i in (2,4,5) order by b, i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where b is true and i in (2,4,5) order by b, i limit 10;

select * from boolindex where b is true and i in (2,4,6) order by b, i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where b is true and i in (2,4,6) order by b, i limit 10;

-- "where b = false" variants (BoolExpr coverage)
select * from boolindex where b = false and i in (2,4,5) order by b, i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where b = false and i in (2,4,5) order by b, i limit 10;

select * from boolindex where b = false and i in (2,4,6) order by b, i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where b = false and i in (2,4,6) order by b, i limit 10;

-- "where not b" variants (more BoolExpr coverage)
select * from boolindex where not b and i in (2,4,5) order by b, i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where not b and i in (2,4,5) order by b, i limit 10;

select * from boolindex where not b and i in (2,4,6) order by b, i limit 10;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from boolindex where not b and i in (2,4,6) order by b, i limit 10;

reset enable_sort;

select 1
from pg_catalog.pg_collation c
where c.collencoding in (-1, 2) and c.collname ~ E'^(no\\.such\\.collation\\$)$';
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select 1
from pg_catalog.pg_collation c
where c.collencoding in (-1, 2) and c.collname ~ E'^(no\\.such\\.collation\\$)$';

-- (February 2) Need to use same-type comparator for sorting, even when we
-- also need to use cross-type comparator during index scans, for binary
-- searches
set client_min_messages=error;
drop table if exists use_proper_monotype_sort;
reset client_min_messages;
create unlogged table use_proper_monotype_sort(collname name);
create index on use_proper_monotype_sort (collname );
insert into use_proper_monotype_sort values ('zh_CN'), ('ja_JP'), ('C'), ('zh_CN');
vacuum analyze use_proper_monotype_sort; -- Be tidy

-- Clearly this works (and has for a long time now):
select * from use_proper_monotype_sort c
where c.collname = any ('{zh_CN,ja_JP,C,zh_CN}');
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from use_proper_monotype_sort c
where c.collname = any ('{zh_CN,ja_JP,C,zh_CN}');

-- We should expect the same from this equivalent spelling, but that'll only
-- actually happen when it has a cross-type ORDER proc for the scan's binary
-- searches, while at the same time using a same-type "text" comparator for
-- its initial sort (cannot use the same ORDER proc for both):
select * from use_proper_monotype_sort c
where c.collname = any ('{zh_CN,ja_JP,C,zh_CN}'::text[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from use_proper_monotype_sort c
where c.collname = any ('{zh_CN,ja_JP,C,zh_CN}'::text[]);

-- Variant that combines the same elements of previous test case with
-- redundancy:
select * from use_proper_monotype_sort c
where c.collname = any ('{ja_JP,C}'::name[])
and c.collname = any ('{zh_CN,ja_JP,C}'::text[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from use_proper_monotype_sort c
where c.collname = any ('{ja_JP,C}'::name[])
and c.collname = any ('{zh_CN,ja_JP,C}'::text[]);

-- Variant that combines the same elements of previous test case with
-- contradictory scan keys:
select * from use_proper_monotype_sort c
where c.collname = any ('{ja_JP,C}'::name[])
and c.collname = any ('{zh_CN}'::text[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from use_proper_monotype_sort c
where c.collname = any ('{ja_JP,C}'::name[])
and c.collname = any ('{zh_CN}'::text[]);

-- This test independently proves that merging (not just sorting) has to use
-- the same-type ORDER proc as its comparator, too
select * from use_proper_monotype_sort c
where c.collname = any ('{ja_JP,C}'::text[])
and c.collname = any ('{C,ja_JP}'::text[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from use_proper_monotype_sort c
where c.collname = any ('{ja_JP,C}'::text[])
and c.collname = any ('{C,ja_JP}'::text[]);

-- (March 10) Variant that aims to make sure we use the appropriate cross-type
-- operator when non-array is considered against array in _bt_preprocess_keys
select * from use_proper_monotype_sort c
where c.collname = any ('{ja_JP,C}'::name[])
and c.collname = 'ja_JP'::text;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from use_proper_monotype_sort c
where c.collname = any ('{ja_JP,C}'::name[])
and c.collname = 'ja_JP'::text;

-- (April 7) Post-commit assertion failure, per report from Alexander Lakhin:
-- https://postgr.es/m/0539d3d3-a402-0a49-ed5e-26429dffc4bd@gmail.com
set client_min_messages=error;
drop table if exists lakhin;
reset client_min_messages;
create unlogged table lakhin (a int, b int);
create index lakhin_idx on lakhin (a, b);
insert into lakhin (a, b) select g, g from generate_series(0, 999) g;
analyze lakhin;
select * from lakhin where a < any (array[1]) and b < any (array[1]);

-- (April 18) Post-commit assertion failure, per report from Donghang Lin:
-- https://postgr.es/m/CAA=D8a2sHK6CAzZ=0CeafC-Y-MFXbYxnRSHvZTi=+JHu6kAa8Q@mail.gmail.com
set client_min_messages=error;
drop table if exists desc_assert_failure;
reset client_min_messages;
create unlogged table desc_assert_failure(a int);
insert into desc_assert_failure select 1 from generate_series(1,10);
create index on desc_assert_failure (a desc);
select * from desc_assert_failure where a IN (1,2) and a IN (1,2,3);

-- (April 21) Post-commit assertion failure, per report from Richar Guo:
-- https://postgr.es/m/CAMbWs48f5rDOwxaT76Zd40m7n9iGZQcjEk7vG_5p3YWNh6oPfA@mail.gmail.com
set client_min_messages=error;
drop table if exists guo_assertion_failure;
reset client_min_messages;
create unlogged table guo_assertion_failure(c int4range);
create unique index on guo_assertion_failure (c);
select * from guo_assertion_failure where c in ('(1, 100]'::int4range, '(50, 300]'::int4range);

-- (April 22) Post-commit assertion failure, per report from Alexander Lakhin:
-- https://postgr.es/m/ef0f7c8b-a6fa-362e-6fd6-054950f947ca@gmail.com
set client_min_messages=error;
drop table if exists lakhin_cursor;
reset client_min_messages;
create unlogged table lakhin_cursor (a text, b text);
insert into lakhin_cursor (a, b) select 'a', repeat('b', 100) from generate_series(1, 500) g;
create index lakhin_cursor_idx on lakhin_cursor using btree(a);

-- Original test case:
begin;
declare c cursor for select a from lakhin_cursor where a = 'a';
fetch from c;
fetch relative 0 from c;
commit;

-- Mix in a SAOP for good luck:
begin;
declare c cursor for select a from lakhin_cursor where a in ('0', 'a', 'b', 'c');
fetch from c;
fetch relative 0 from c;
commit;
-- Mix in a SAOP for good luck again:
begin;
declare c cursor for select a from lakhin_cursor where a in ('0', 'b', 'c');
fetch from c;
fetch relative 0 from c;
commit;

-- (June 20 2024)
--
-- has_required_opposite_direction_only issue with MDAM table when I allow
-- some inequalities to appear at the end of output scan keys during
-- preprocessing
--
-- Recall how the SAOP project assertion gated by this condition would fail:
--
-- "if (has_required_opposite_direction_only && pstate->finaltup &&
-- 	(all_required_satisfied || oppodir_inequality_sktrig))"
--
-- This was because "so->keyData[opsktrig].sk_strategy ==
-- BTEqualStrategyNumber", contrary to what the assertion expected
-- This was the warning I'd see when I changed the assertion into a WARNING:
--
-- WARNING:  has_required_opposite_direction_only opsktrig: 2, sktrig: 1
--
set client_min_messages=error;
drop table if exists sales_mdam_oppodir_issue;
reset client_min_messages;

create unlogged table sales_mdam_oppodir_issue
(
  dept int4,
  sdate date,
  item_class serial,
  store int4,
  item int4,
  total_sales numeric
);
create index oppodir_mdam_idx on sales_mdam_oppodir_issue(dept, sdate, item_class, store);

select setseed(0.5);

insert into sales_mdam_oppodir_issue (dept, sdate, item_class, store, total_sales)
select
  dept,
  '1995-01-01'::date + sdate,
  item_class,
  store,
  (random() * 500.0) as total_sales
from
  generate_series(5, 7) dept,
  generate_series(1, 5) sdate,
  generate_series(1, 15) item_class,
  generate_series(1, 25) store;

prepare tenth as
select sdate, item_class, store, sum(total_sales)
from sales_mdam_oppodir_issue
where
  dept between 5 and 7
  and sdate between '1995-01-01' and '1995-01-05'
  and item_class between 1 and 15
  and store between 15 and 25
group by sdate, item_class, store;

-- Here is what the failure looked like, if I make it into an ERROR and dump
-- instrumentation up until that point:

/*
2024-06-20 13:34:35.656 EDT [1633957][client backend] [pg_regress/skip_scan][0/396:0] LOCATION:  _bt_advance_array_keys, nbtutils.c:3566
 2024-06-20 13:34:35.656 EDT [1633957][client backend] [pg_regress/skip_scan][0/396:0] STATEMENT:  execute tenth;
 2024-06-20 13:34:35.656 EDT [1633957][client backend] [pg_regress/skip_scan][0/397:0] ERROR:  XX000:
	👾  btbeginscan to begin scan of index "oppodir_mdam_idx" in worker -1
	♻️  btrescan
	btrescan: BTScanPosInvalidate() called for markPos
	_bt_preprocess_keys:  scan->keyData[0]: [ strategy: >=, attno: 1/"dept", func: int4ge, flags: [] ]
	                      scan->keyData[1]: [ strategy: <=, attno: 1/"dept", func: int4le, flags: [] ]
	                      scan->keyData[2]: [ strategy: >=, attno: 2/"sdate", func: date_ge, flags: [] ]
	                      scan->keyData[3]: [ strategy: <=, attno: 2/"sdate", func: date_le, flags: [] ]
	                      scan->keyData[4]: [ strategy: >=, attno: 3/"item_class", func: int4ge, flags: [] ]
	                      scan->keyData[5]: [ strategy: <=, attno: 3/"item_class", func: int4le, flags: [] ]
	                      scan->keyData[6]: [ strategy: >=, attno: 4/"store", func: int4ge, flags: [] ]
	                      scan->keyData[7]: [ strategy: <=, attno: 4/"store", func: int4le, flags: [] ]
	_bt_preprocess_keys:    so->keyData[0]: [ strategy: = , attno: 1/"dept", func: int4eq, flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD, SK_BT_SKIP] ]
	                        so->keyData[1]: [ strategy: = , attno: 2/"sdate", func: date_eq, flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD, SK_BT_SKIP] ]
	                        so->keyData[2]: [ strategy: = , attno: 3/"item_class", func: int4eq, flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD, SK_BT_SKIP] ]
	                        so->keyData[3]: [ strategy: >=, attno: 4/"store", func: int4ge, flags: [SK_BT_REQBKWD] ]
	                        so->keyData[4]: [ strategy: <=, attno: 4/"store", func: int4le, flags: [SK_BT_REQFWD] ]
	_bt_preprocess_keys: scan->numberOfKeys is 8, so->numberOfKeys on output is 5, so->numArrayKeys on output is 3

	➕     ➕     ➕
	_bt_first: sk_attno 1. val: 5, func: btint4cmp
	           sk_attno 2. val: 01-01-1995, func: date_cmp
	           sk_attno 3. val: 1, func: btint4cmp
	           sk_attno 4. val: 15, func: btint4cmp
	           with strat_total='>=', inskey.keys=4, inskey.nextkey=0, inskey.backward=0
	🔽  ==================== _bt_search begin at root 3 level 1 ====================
	_bt_moveright: blk 3 is rightmost
	_bt_search: sk > (dept, sdate, item_class, store)=(), sk <= (dept, sdate, item_class, store)=(5, 01-02-1995, 12)
	🔽  -------------------- descended to child blk 1 level 0 --------------------
	_bt_moveright: (dept, sdate, item_class, store)=(5, 01-02-1995, 12), high key no move right
	⏹️  ==================== _bt_search end ====================
	_bt_readpage: 🍀  1 with 276 offsets/tuples (leftsib 0, rightsib 19) ➡️
	 _bt_readpage first: (dept, sdate, item_class, store)=(5, 01-02-1995, 1, 1), TID='(0,1)', 0x7f4b16e65fc0, from non-pivot offnum 2 started page
	  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-02-1995, 1, 1) with TID (0,1), 0x7f4b16e65fc0
	_bt_advance_array_keys, sktrig: 1, tuple: (dept, sdate, item_class, store)=(5, 01-02-1995, 1, 1), 0x7f4b16e65fc0
	  numberOfKeys: 5

	  - sk_attno: 1, cur_elem:    0, num_elems:   -1, val: 5
	  - sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-01-1995            <--
	  - sk_attno: 3, cur_elem:    0, num_elems:   -1, val: 1

	  + sk_attno: 1, cur_elem:    0, num_elems:   -1, val: 5
	  + sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-02-1995
	  + sk_attno: 3, cur_elem:    0, num_elems:   -1, val: 1

	  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-02-1995, 1, 1) with TID (0,1), 0x7f4b16e65fc0
	  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-02-1995, 12) with TID -inf, 0x7f4b16e65fd8

	, has_required_opposite_direction_only opsktrig: 2, sktrig: 1
 2024-06-20 13:34:35.656 EDT [1633957][client backend] [pg_regress/skip_scan][0/397:0] LOCATION:  _bt_advance_array_keys, nbtutils.c:3566
 2024-06-20 13:34:35.656 EDT [1633957][client backend] [pg_regress/skip_scan][0/397:0] STATEMENT:  EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
	execute tenth;
*/
execute tenth;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute tenth;
deallocate tenth;

-- (June 20)
--
-- This simplified variant doesn't even require the use of skip arrays.  It
-- exposed a bug on HEAD.
prepare simplied_tenth_no_skipping as
select
  dept,
  sdate,
  item_class,
  store,
  total_sales
from
  sales_mdam_oppodir_issue
where
  dept = 5
  and sdate in ('0001-01-01', '1995-01-02')
  and item_class = 1
  and store >= 555555
  order by dept, sdate, item_class, store;

execute simplied_tenth_no_skipping;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute simplied_tenth_no_skipping;
deallocate simplied_tenth_no_skipping;

-- (August 26 2024) Post-commit assertion failure, per another report from Alexander Lakhin:
-- https://postgr.es/m/6c68ac42-bbb5-8b24-103e-af0e279c536f@gmail.com
set enable_sort = off;
select conname
from pg_constraint
where conname in ('pkey', 'id')
order by conname desc;
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
select conname
from pg_constraint
where conname in ('pkey', 'id')
order by conname desc;

-- (October 29 2024) Post-commit bug with cursor that changes direction,
-- discovered after largely unrelated nbtree backwards scan optimization and
-- refactoring work
--
-- https://postgr.es/m/CAH2-Wznv49bFsE2jkt4GuZ0tU2C91dEST=50egzjY2FeOcHL4Q@mail.gmail.com

-- Setup:
set client_min_messages=error;
drop table if exists btfirst_array_confusion_test;
reset client_min_messages;
create unlogged table btfirst_array_confusion_test(
  district int4,
  warehouse int4,
  orderid int4,
  orderline int4
);
create index btfirst_array_confusion_test_idx
            on
            btfirst_array_confusion_test(district, warehouse, orderid, orderline) with (fillfactor = 30);

-- Load:
insert into btfirst_array_confusion_test
select district, warehouse, orderid, orderline
from
  generate_series(1, 2) district,
  generate_series(1, 5) warehouse,
  generate_series(1, 10) orderid,
  generate_series(1, 10) orderline
order by
district, warehouse, orderid, orderline;

-- This is :rootitems, just to avoid test regressions:
select itemoffset, ctid, itemlen, nulls from bt_page_items('btfirst_array_confusion_test_idx',
  (select fastroot::int4 from bt_metap('btfirst_array_confusion_test_idx')));

-- pg@regression:5432 [309395]=# :rootitems
-- ┌────────────┬────────┬─────────┬───────┬──────┬──────────────────────────────────────────────────┬──────┬──────┬──────┐
-- │ itemoffset │  ctid  │ itemlen │ nulls │ vars │                       data                       │ dead │ htid │ tids │
-- ├────────────┼────────┼─────────┼───────┼──────┼──────────────────────────────────────────────────┼──────┼──────┼──────┤
-- │          1 │ (1,0)  │       8 │ f     │ f    │ (district, warehouse, orderid, orderline)=()     │ ∅    │ ∅    │ ∅    │
-- │          2 │ (2,2)  │      16 │ f     │ f    │ (district, warehouse, orderid, orderline)=(1, 2) │ ∅    │ ∅    │ ∅    │
-- │          3 │ (4,2)  │      16 │ f     │ f    │ (district, warehouse, orderid, orderline)=(1, 3) │ ∅    │ ∅    │ ∅    │
-- │          4 │ (5,2)  │      16 │ f     │ f    │ (district, warehouse, orderid, orderline)=(1, 4) │ ∅    │ ∅    │ ∅    │
-- │          5 │ (6,2)  │      16 │ f     │ f    │ (district, warehouse, orderid, orderline)=(1, 5) │ ∅    │ ∅    │ ∅    │
-- │          6 │ (7,1)  │      16 │ f     │ f    │ (district, warehouse, orderid, orderline)=(2)    │ ∅    │ ∅    │ ∅    │
-- │          7 │ (8,2)  │      16 │ f     │ f    │ (district, warehouse, orderid, orderline)=(2, 2) │ ∅    │ ∅    │ ∅    │
-- │          8 │ (9,2)  │      16 │ f     │ f    │ (district, warehouse, orderid, orderline)=(2, 3) │ ∅    │ ∅    │ ∅    │
-- │          9 │ (10,2) │      16 │ f     │ f    │ (district, warehouse, orderid, orderline)=(2, 4) │ ∅    │ ∅    │ ∅    │
-- └────────────┴────────┴─────────┴───────┴──────┴──────────────────────────────────────────────────┴──────┴──────┴──────┘
-- (9 rows)
--
-- Importantly, this test case involves cursor scrolling confined to this leaf
-- page (once bug is fixed this is what we expect):
--
-- 	_bt_readpage: 🍀  1 with 101 offsets/tuples (leftsib 0, rightsib 2) ➡️
--	 _bt_readpage first: (district, warehouse, orderid, orderline)=(1, 1, 9, 1), TID='(0,81)', 0x7f91ea04b848, from non-pivot offnum 82 started page
--	 _bt_advance_array_keys, sktrig: 2, tuple: (district, warehouse, orderid, orderline)=(1, 1, 10, 1), 0x7f91ea04b758

-- Index scan without materialization for cursor query:
set enable_seqscan to off;
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_indexscan to on;
set work_mem = 64;
set enable_sort = off;

begin;
declare btfirst_array_confusion_test_cursor cursor for
select * from btfirst_array_confusion_test
where district in (1, 2) and warehouse = 1 and orderid = 9
order by district, warehouse, orderid, orderline;

-- Fetch all 10 orderlines for the first order returned by cursor, so that we
-- read all relevant index tuples from the first leaf page read, and so leave
-- so->currPos.itemIndex right at the end of the page (not one before or one after the
-- end; precisely at the boundaries between two adjoining pages):
fetch forward 10 from btfirst_array_confusion_test_cursor;

-- Fetch 9 orderlines before the last one returned by previous fetch
-- (_bt_first confusion happens here when run against buggy server, as
-- evidenced by this statement returning 10 rows rather than just 9):
fetch backward 10 from btfirst_array_confusion_test_cursor; -- returns an extra row with bug

-- Note: _bt_first becoming confused by the prior fetch statement relies on
-- the fact that the page we almost (but didn't quite) move right from is also
-- the first page visited by the entire top-level index scan for the cursor.
-- This is a necessary condition, since we don't try to call _bt_first again
-- when the scan direction changes unless the scan direction changes on the
-- first page for the top-level scan.

-- Repeat the first fetch, expect the same 10 order + orderline rows as with
-- the first fetch (but don't get them with buggy server with confused array
-- state following previous fetch):
fetch forward 10 from btfirst_array_confusion_test_cursor; -- returns no rows with bug

/* btfirst_array_confusion_test_cursor */ commit;

-- (December 18, 2024)
--
-- This revealed a bug on HEAD, where so->scanBehind wasn't reset on an
-- !sktrigrequired call to _bt_advance_array_keys, meaning that we denied
-- the _bt_check_compare recheck call the opportunity to return 'true'.
--
-- See: https://postgr.es/m/CAH2-WzkJKncfqyAUTeuB5GgRhT1vhsWO2q11dbZNqKmvjopP_g@mail.gmail.com
set client_min_messages=error;
drop table if exists dec_bug_test;
reset client_min_messages;
create unlogged table dec_bug_test(
  leading_singleval int4,
  second_twovals int4,
  inequal_one_ten_range int4,
  nonrequired_equal_one_ten_range int4
);

create index dec_bug_test_idx on dec_bug_test(leading_singleval, second_twovals, inequal_one_ten_range, nonrequired_equal_one_ten_range) with (fillfactor = 30);

insert into dec_bug_test
select leading_singleval, second_twovals, inequal_one_ten_range, nonrequired_equal_one_ten_range
from
  generate_series(1, 1) leading_singleval,
  generate_series(1, 5) second_twovals,
  generate_series(1, 10) inequal_one_ten_range,
  generate_series(1, 10) nonrequired_equal_one_ten_range
order by
leading_singleval,
second_twovals,
inequal_one_ten_range,
nonrequired_equal_one_ten_range;

-- prewarm
select count(*) from dec_bug_test;
vacuum analyze dec_bug_test;
---------------------------------------------------------------------------------

-- Index scan
set enable_bitmapscan to off;
set enable_indexonlyscan to off;
set enable_seqscan to off;
set enable_indexscan to on;

prepare dec_bug_test_qry as
select * from dec_bug_test
where
leading_singleval = 1
  and second_twovals in (1, 2)
  and inequal_one_ten_range <= 10
  and nonrequired_equal_one_ten_range in (1, 2)
order by
  leading_singleval,
  second_twovals,
  inequal_one_ten_range,
  nonrequired_equal_one_ten_range;

set skipscan_prefix_cols = 0; -- don't allow skip scan patch to suppress coverage of issue
execute dec_bug_test_qry;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute dec_bug_test_qry;

reset skipscan_prefix_cols; -- now do it again, this time with skip scan enabled
execute dec_bug_test_qry;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute dec_bug_test_qry;

deallocate dec_bug_test_qry;

-- (January 20 2025) RowCompare unsatisfiable qual + NULL test
--
-- Per https://postgr.es/m/CAH2-WzmySVXst2hFrOATC-zw1Byg1XC-jYUS314=mzuqsNwk+Q@mail.gmail.com
set client_min_messages=error;
drop table if exists rowcompare_test;
reset client_min_messages;

create unlogged table rowcompare_test as
select i as first, i as second, i as third
from generate_series(1, 100) i;

create index on rowcompare_test (first, second, third);

-- Usable-by-_bt_first (also required) RowCompare key case:
select *
from rowcompare_test
where first = 1 and (second, third) > (null, 0);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from rowcompare_test
where first = 1 and (second, third) > (null, 0);

-- Not-usable-by-_bt_first (also non-required) RowCompare key case:
select *
from rowcompare_test
where (second, third) > (null, 0);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from rowcompare_test
where (second, third) > (null, 0);

-- reproducer for a crash, triggered by incorrect currentPrefetchBlock when
-- initializing the stream on the second batch
set client_min_messages=error;
drop table if exists t_6;
reset client_min_messages;
create table t_6 (a bigint) with (fillfactor = 56);
create index on t_6 (a) with (fillfactor = 56, deduplicate_items = off);
insert into t_6 select (i / 103) from generate_series(1, 1000) s(i) order by i + mod(i::bigint * 35543, 553), md5(i::text);
begin;
declare c_6 scroll cursor for select * from t_6 where a in (0,1,2,3,4,5,6,7,8,9) order by a;
fetch backward 29 from c_6;
fetch backward 44 from c_6;
fetch backward 18 from c_6;
fetch forward 13 from c_6;
fetch forward 86 from c_6;
fetch backward 73 from c_6;
fetch forward 69 from c_6;
fetch forward 30 from c_6;
fetch forward 6 from c_6;
fetch forward 75 from c_6;
rollback;

-- dataset that fill the batch queue, forcing stream reset before loading
-- more batches
set client_min_messages=error;
drop table if exists test_stream_reset;
reset client_min_messages;
create table test_stream_reset (a bigint, b bigint);
insert into test_stream_reset select 1, i from generate_series(1,1000000) s(i);
create index on test_stream_reset (a) with (deduplicate_items=off, fillfactor=10);
analyze test_stream_reset;
-- evict data, to force look-ahead
select 1 from pg_buffercache_evict_relation('test_stream_reset');
select * from (select * from test_stream_reset order by a offset 1000000);

-- 2026-01-11 12:33
--
-- Hash index hang bug
--
set client_min_messages=error;
drop table if exists t_0;
reset client_min_messages;

create table t_0 (a bigint) with (fillfactor = 54);
create index on t_0 using hash (a) with (fillfactor = 43);
insert into t_0 select (i / 199) from generate_series(1, 100000) s(i) order by i + mod(i::bigint * 142821, 284), md5(i::text);
vacuum freeze t_0;
analyze t_0;
begin;
set enable_seqscan = off;
set enable_bitmapscan = off;
set cursor_tuple_fraction = 1.0;

declare c_0 scroll cursor for select * from t_0 where a = 383;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch forward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
select 1 from pg_buffercache_evict_all();
fetch backward from c_0;
commit;


-- 2026-01-14 10:56
--
-- Proof that we need "INDEX_SCAN_BATCH_LOADED(scan, prefetchPos->batch + 1)"
-- handling within read stream callback
set client_min_messages=error;
drop table if exists t_11;
reset client_min_messages;
create table t_11 (a bigint, b bigint) with (fillfactor = 80);
create index on t_11 (a, b) with (fillfactor = 12, deduplicate_items = on);
insert into t_11
select (i / 331), (i / 59)
from
  generate_series(1, 100000) s(i)
order by i + mod(i::bigint * 997416, 244), md5(i::text);
vacuum freeze t_11;
analyze t_11;
set enable_seqscan = off;
set enable_bitmapscan = off;
set enable_indexonlyscan = off;
set cursor_tuple_fraction = 1.0;
begin;
declare c_11 scroll cursor for
  select *
  from t_11
  where
    a in (0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100, 105, 110, 115, 120, 125, 130, 135, 140, 145, 150, 155, 160, 165, 170, 175, 180, 185, 190, 195, 200, 205, 210, 215, 220, 225, 230, 235, 240, 245, 250, 255, 260, 265, 270, 275, 280, 285, 290, 295, 300)
    and b in (0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100, 105, 110, 115, 120, 125, 130, 135, 140, 145, 150, 155, 160, 165, 170, 175, 180, 185, 190, 195, 200, 205, 210, 215, 220, 225, 230, 235, 240, 245, 250, 255, 260, 265, 270, 275, 280, 285, 290, 295, 300, 305, 310, 315, 320, 325, 330, 335, 340, 345, 350, 355, 360, 365, 370, 375, 380, 385, 390, 395, 400, 405, 410, 415, 420, 425, 430, 435, 440, 445, 450, 455, 460, 465, 470, 475, 480, 485, 490, 495, 500, 505, 510, 515, 520, 525, 530, 535, 540, 545, 550, 555, 560, 565, 570, 575, 580, 585, 590, 595, 600, 605, 610, 615, 620, 625, 630, 635, 640, 645, 650, 655, 660, 665, 670, 675, 680, 685, 690, 695, 700, 705, 710, 715, 720, 725, 730, 735, 740, 745, 750, 755, 760, 765, 770, 775, 780, 785, 790, 795, 800, 805, 810, 815, 820, 825, 830, 835, 840, 845, 850, 855, 860, 865, 870, 875, 880, 885, 890, 895, 900, 905, 910, 915, 920, 925, 930, 935, 940, 945, 950, 955, 960, 965, 970, 975, 980, 985, 990, 995, 1000, 1005, 1010, 1015, 1020, 1025, 1030, 1035, 1040, 1045, 1050, 1055, 1060, 1065, 1070, 1075, 1080, 1085, 1090, 1095, 1100, 1105, 1110, 1115, 1120, 1125, 1130, 1135, 1140, 1145, 1150, 1155, 1160, 1165, 1170, 1175, 1180, 1185, 1190, 1195, 1200, 1205, 1210, 1215, 1220, 1225, 1230, 1235, 1240, 1245, 1250, 1255, 1260, 1265, 1270, 1275, 1280, 1285, 1290, 1295, 1300, 1305, 1310, 1315, 1320, 1325, 1330, 1335, 1340, 1345, 1350, 1355, 1360, 1365, 1370, 1375, 1380, 1385, 1390, 1395, 1400, 1405, 1410, 1415, 1420, 1425, 1430, 1435, 1440, 1445, 1450, 1455, 1460, 1465, 1470, 1475, 1480, 1485, 1490, 1495, 1500, 1505, 1510, 1515, 1520, 1525, 1530, 1535, 1540, 1545, 1550, 1555, 1560, 1565, 1570, 1575, 1580, 1585, 1590, 1595, 1600, 1605, 1610, 1615, 1620, 1625, 1630, 1635, 1640, 1645, 1650, 1655, 1660, 1665, 1670, 1675, 1680, 1685, 1690)
  order by a desc, b desc;
select 1 from pg_buffercache_evict_all();
fetch forward 41 from c_11;
select 1 from pg_buffercache_evict_all();
fetch forward 96 from c_11;
select 1 from pg_buffercache_evict_all();
fetch forward 96 from c_11;
select 1 from pg_buffercache_evict_all();
fetch forward 39 from c_11;
select 1 from pg_buffercache_evict_all();
fetch backward 92 from c_11;
commit;

-- 2026-01-14 15:43
--
-- Test case that independently failed when I tried to be cute about testing
-- "INDEX_SCAN_BATCH_LOADED(scan, prefetchPos->batch + 1)" at the start of
-- read stream callback, reinitializing prefetchPos using scanPos when this
-- condition holds.
set client_min_messages=error;
drop table if exists t_6;
reset client_min_messages;

set enable_seqscan = off;
set enable_bitmapscan = off;
set enable_indexonlyscan = off;
set cursor_tuple_fraction = 1.0;

create table t_6 (a bigint, b bigint) with (fillfactor = 45);
create index on t_6 (a, b) with (fillfactor = 29, deduplicate_items = off);

insert into t_6
  select (i / 59), (i / 83)
  from generate_series(1, 100000) s(i)
  order by i + mod(i::bigint * 355508, 426), md5(i::text);

vacuum freeze t_6;
analyze t_6;

begin;
declare c_6 scroll cursor for
  select *
  from t_6
  where
    a in (0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220, 230, 240, 250, 260, 270, 280, 290, 300, 310, 320, 330, 340, 350, 360, 370, 380, 390, 400, 410, 420, 430, 440, 450, 460, 470, 480, 490, 500, 510, 520, 530, 540, 550, 560, 570, 580, 590, 600, 610, 620, 630, 640, 650, 660, 670, 680, 690, 700, 710, 720, 730, 740, 750, 760, 770, 780, 790, 800, 810, 820, 830, 840, 850, 860, 870, 880, 890, 900, 910, 920, 930, 940, 950, 960, 970, 980, 990, 1000, 1010, 1020, 1030, 1040, 1050, 1060, 1070, 1080, 1090, 1100, 1110, 1120, 1130, 1140, 1150, 1160, 1170, 1180, 1190, 1200, 1210, 1220, 1230, 1240, 1250, 1260, 1270, 1280, 1290, 1300, 1310, 1320, 1330, 1340, 1350, 1360, 1370, 1380, 1390, 1400, 1410, 1420, 1430, 1440, 1450, 1460, 1470, 1480, 1490, 1500, 1510, 1520, 1530, 1540, 1550, 1560, 1570, 1580, 1590, 1600, 1610, 1620, 1630, 1640, 1650, 1660, 1670, 1680, 1690)
    and b in (0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220, 230, 240, 250, 260, 270, 280, 290, 300, 310, 320, 330, 340, 350, 360, 370, 380, 390, 400, 410, 420, 430, 440, 450, 460, 470, 480, 490, 500, 510, 520, 530, 540, 550, 560, 570, 580, 590, 600, 610, 620, 630, 640, 650, 660, 670, 680, 690, 700, 710, 720, 730, 740, 750, 760, 770, 780, 790, 800, 810, 820, 830, 840, 850, 860, 870, 880, 890, 900, 910, 920, 930, 940, 950, 960, 970, 980, 990, 1000, 1010, 1020, 1030, 1040, 1050, 1060, 1070, 1080, 1090, 1100, 1110, 1120, 1130, 1140, 1150, 1160, 1170, 1180, 1190, 1200)
  order by a desc, b desc;

select 1 from pg_buffercache_evict_all();
fetch forward 80 from c_6;
select 1 from pg_buffercache_evict_all();
fetch backward 17 from c_6;
select 1 from pg_buffercache_evict_all();
fetch forward 51 from c_6;
commit;

--
-- 2026-01-19 19:09
--
-- Showed bug in uint8-based ring buffer design

set client_min_messages=error;
drop table if exists t_4;
reset client_min_messages;

create table t_4(
  a bigint,
  b bigint,
  c bigint,
  d bigint,
  e bigint,
  f bigint
)
with (fillfactor = 80);

create index on t_4(a, b, c, d, e, f) with (fillfactor = 21, deduplicate_items = off);

insert into t_4
select (i / 283), (i / 31), (i / 151), (i / 257), (i / 167), (i / 541)
from generate_series(1, 100000) s(i)
order by i + mod(i::bigint * 197755, 221), md5(i::text);

vacuum freeze t_4;
analyze t_4;
set enable_seqscan = off;
set enable_bitmapscan = off;
set enable_indexonlyscan = off;
set cursor_tuple_fraction = 1.0;

begin;
declare c_4 scroll cursor for
  select
    *
  from
    t_4
  where
    a in (0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252, 256, 260, 264, 268, 272, 276, 280, 284, 288, 292, 296, 300, 304, 308, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352)
    and b in (0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252, 256, 260, 264, 268, 272, 276, 280, 284, 288, 292, 296, 300, 304, 308, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 356, 360, 364, 368, 372, 376, 380, 384, 388, 392, 396, 400, 404, 408, 412, 416, 420, 424, 428, 432, 436, 440, 444, 448, 452, 456, 460, 464, 468, 472, 476, 480, 484, 488, 492, 496, 500, 504, 508, 512, 516, 520, 524, 528, 532, 536, 540, 544, 548, 552, 556, 560, 564, 568, 572, 576, 580, 584, 588, 592, 596, 600, 604, 608, 612, 616, 620, 624, 628, 632, 636, 640, 644, 648, 652, 656, 660, 664, 668, 672, 676, 680, 684, 688, 692, 696, 700, 704, 708, 712, 716, 720, 724, 728, 732, 736, 740, 744, 748, 752, 756, 760, 764, 768, 772, 776, 780, 784, 788, 792, 796, 800, 804, 808, 812, 816, 820, 824, 828, 832, 836, 840, 844, 848, 852, 856, 860, 864, 868, 872, 876, 880, 884, 888, 892, 896, 900, 904, 908, 912, 916, 920, 924, 928, 932, 936, 940, 944, 948, 952, 956, 960, 964, 968, 972, 976, 980, 984, 988, 992, 996, 1000, 1004, 1008, 1012, 1016, 1020, 1024, 1028, 1032, 1036, 1040, 1044, 1048, 1052, 1056, 1060, 1064, 1068, 1072, 1076, 1080, 1084, 1088, 1092, 1096, 1100, 1104, 1108, 1112, 1116, 1120, 1124, 1128, 1132, 1136, 1140, 1144, 1148, 1152, 1156, 1160, 1164, 1168, 1172, 1176, 1180, 1184, 1188, 1192, 1196, 1200, 1204, 1208, 1212, 1216, 1220, 1224, 1228, 1232, 1236, 1240, 1244, 1248, 1252, 1256, 1260, 1264, 1268, 1272, 1276, 1280, 1284, 1288, 1292, 1296, 1300, 1304, 1308, 1312, 1316, 1320, 1324, 1328, 1332, 1336, 1340, 1344, 1348, 1352, 1356, 1360, 1364, 1368, 1372, 1376, 1380, 1384, 1388, 1392, 1396, 1400, 1404, 1408, 1412, 1416, 1420, 1424, 1428, 1432, 1436, 1440, 1444, 1448, 1452, 1456, 1460, 1464, 1468, 1472, 1476, 1480, 1484, 1488, 1492, 1496, 1500, 1504, 1508, 1512, 1516, 1520, 1524, 1528, 1532, 1536, 1540, 1544, 1548, 1552, 1556, 1560, 1564, 1568, 1572, 1576, 1580, 1584, 1588, 1592, 1596, 1600, 1604, 1608, 1612, 1616, 1620, 1624, 1628, 1632, 1636, 1640, 1644, 1648, 1652, 1656, 1660, 1664, 1668, 1672, 1676, 1680, 1684, 1688, 1692, 1696, 1700, 1704, 1708, 1712, 1716, 1720, 1724, 1728, 1732, 1736, 1740, 1744, 1748, 1752, 1756, 1760, 1764, 1768, 1772, 1776, 1780, 1784, 1788, 1792, 1796, 1800, 1804, 1808, 1812, 1816, 1820, 1824, 1828, 1832, 1836, 1840, 1844, 1848, 1852, 1856, 1860, 1864, 1868, 1872, 1876, 1880, 1884, 1888, 1892, 1896, 1900, 1904, 1908, 1912, 1916, 1920, 1924, 1928, 1932, 1936, 1940, 1944, 1948, 1952, 1956, 1960, 1964, 1968, 1972, 1976, 1980, 1984, 1988, 1992, 1996, 2000, 2004, 2008, 2012, 2016, 2020, 2024, 2028, 2032, 2036, 2040, 2044, 2048, 2052, 2056, 2060, 2064, 2068, 2072, 2076, 2080, 2084, 2088, 2092, 2096, 2100, 2104, 2108, 2112, 2116, 2120, 2124, 2128, 2132, 2136, 2140, 2144, 2148, 2152, 2156, 2160, 2164, 2168, 2172, 2176, 2180, 2184, 2188, 2192, 2196, 2200, 2204, 2208, 2212, 2216, 2220, 2224, 2228, 2232, 2236, 2240, 2244, 2248, 2252, 2256, 2260, 2264, 2268, 2272, 2276, 2280, 2284, 2288, 2292, 2296, 2300, 2304, 2308, 2312, 2316, 2320, 2324, 2328, 2332, 2336, 2340, 2344, 2348, 2352, 2356, 2360, 2364, 2368, 2372, 2376, 2380, 2384, 2388, 2392, 2396, 2400, 2404, 2408, 2412, 2416, 2420, 2424, 2428, 2432, 2436, 2440, 2444, 2448, 2452, 2456, 2460, 2464, 2468, 2472, 2476, 2480, 2484, 2488, 2492, 2496, 2500, 2504, 2508, 2512, 2516, 2520, 2524, 2528, 2532, 2536, 2540, 2544, 2548, 2552, 2556, 2560, 2564, 2568, 2572, 2576, 2580, 2584, 2588, 2592, 2596, 2600, 2604, 2608, 2612, 2616, 2620, 2624, 2628, 2632, 2636, 2640, 2644, 2648, 2652, 2656, 2660, 2664, 2668, 2672, 2676, 2680, 2684, 2688, 2692, 2696, 2700, 2704, 2708, 2712, 2716, 2720, 2724, 2728, 2732, 2736, 2740, 2744, 2748, 2752, 2756, 2760, 2764, 2768, 2772, 2776, 2780, 2784, 2788, 2792, 2796, 2800, 2804, 2808, 2812, 2816, 2820, 2824, 2828, 2832, 2836, 2840, 2844, 2848, 2852, 2856, 2860, 2864, 2868, 2872, 2876, 2880, 2884, 2888, 2892, 2896, 2900, 2904, 2908, 2912, 2916, 2920, 2924, 2928, 2932, 2936, 2940, 2944, 2948, 2952, 2956, 2960, 2964, 2968, 2972, 2976, 2980, 2984, 2988, 2992, 2996, 3000, 3004, 3008, 3012, 3016, 3020, 3024, 3028, 3032, 3036, 3040, 3044, 3048, 3052, 3056, 3060, 3064, 3068, 3072, 3076, 3080, 3084, 3088, 3092, 3096, 3100, 3104, 3108, 3112, 3116, 3120, 3124, 3128, 3132, 3136, 3140, 3144, 3148, 3152, 3156, 3160, 3164, 3168, 3172, 3176, 3180, 3184, 3188, 3192, 3196, 3200, 3204, 3208, 3212, 3216, 3220, 3224)
    and c in (0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252, 256, 260, 264, 268, 272, 276, 280, 284, 288, 292, 296, 300, 304, 308, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 356, 360, 364, 368, 372, 376, 380, 384, 388, 392, 396, 400, 404, 408, 412, 416, 420, 424, 428, 432, 436, 440, 444, 448, 452, 456, 460, 464, 468, 472, 476, 480, 484, 488, 492, 496, 500, 504, 508, 512, 516, 520, 524, 528, 532, 536, 540, 544, 548, 552, 556, 560, 564, 568, 572, 576, 580, 584, 588, 592, 596, 600, 604, 608, 612, 616, 620, 624, 628, 632, 636, 640, 644, 648, 652, 656, 660)
    and d in (0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252, 256, 260, 264, 268, 272, 276, 280, 284, 288, 292, 296, 300, 304, 308, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 356, 360, 364, 368, 372, 376, 380, 384, 388)
    and e in (0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252, 256, 260, 264, 268, 272, 276, 280, 284, 288, 292, 296, 300, 304, 308, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 356, 360, 364, 368, 372, 376, 380, 384, 388, 392, 396, 400, 404, 408, 412, 416, 420, 424, 428, 432, 436, 440, 444, 448, 452, 456, 460, 464, 468, 472, 476, 480, 484, 488, 492, 496, 500, 504, 508, 512, 516, 520, 524, 528, 532, 536, 540, 544, 548, 552, 556, 560, 564, 568, 572, 576, 580, 584, 588, 592, 596)
    and f in (0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184)
  order by a asc, b asc, c asc, d asc, e asc, f asc;

select 1 from pg_buffercache_evict_all();
fetch backward 75 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 64 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 40 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 37 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 74 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 72 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 23 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 69 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 68 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 44 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 87 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 86 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 70 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 86 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 96 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 72 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 19 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 6 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 51 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 69 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 54 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 34 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 81 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 4 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 34 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 14 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 89 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 85 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 53 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 34 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 71 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 9 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 5 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 75 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 11 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 54 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 84 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 65 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 53 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 85 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 46 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 9 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 82 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 72 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 90 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 73 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 24 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 96 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 46 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 11 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 95 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 92 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 16 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 54 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 22 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 67 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 85 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 66 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 60 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 93 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 43 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 33 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 78 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 10 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 31 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 78 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 33 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 36 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 85 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 17 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 47 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 70 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 95 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 77 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 83 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 89 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 99 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 51 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 95 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 48 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 68 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 16 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 87 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 68 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 28 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 18 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 95 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 91 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 34 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 58 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 67 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 41 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 34 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 46 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 54 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 9 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 5 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 34 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 8 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 43 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 89 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 26 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 78 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 55 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 33 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 90 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 77 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 37 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 20 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 37 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 82 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 25 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 33 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 91 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 72 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 55 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 9 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 46 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 55 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 27 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 47 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 62 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 4 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 35 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 50 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 66 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 34 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 78 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 54 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 27 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 33 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 27 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 85 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 53 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 59 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 19 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 3 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 11 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 12 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 63 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 73 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 60 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 69 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 34 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 4 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 77 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 46 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 10 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 19 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 18 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 17 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 20 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 33 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 1 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 53 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 9 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 69 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 10 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 32 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 69 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 3 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 65 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 51 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 17 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 28 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 46 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 20 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 96 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 92 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 42 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 68 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 15 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 80 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 48 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 93 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 50 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 11 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 86 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 57 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 100 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 11 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 13 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 31 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 96 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 49 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 42 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 8 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 73 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 54 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 44 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 60 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 9 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 76 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 27 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 95 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 70 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 55 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 23 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 2 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 59 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 50 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 71 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 92 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 13 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 17 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 37 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 36 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 8 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 69 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 60 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 83 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 60 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 10 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 26 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 75 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 86 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 71 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 87 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 89 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 48 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 97 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 86 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 90 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 3 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 11 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 95 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 43 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 53 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 52 from c_4;
select 1 from pg_buffercache_evict_all();
fetch backward 38 from c_4;
select 1 from pg_buffercache_evict_all();
fetch forward 16 from c_4;

commit;

-- 2026-01-21 19:25
-- Wrong answer bug from Tomas
set client_min_messages=error;
drop table if exists t_23;
reset client_min_messages;

create table t_23 (a bigint, b bigint, c bigint, d bigint, e bigint) with (fillfactor = 70);
create index on t_23 (a, b, c, d, e) with (fillfactor = 33, deduplicate_items = off);

insert into t_23
select (i / 277), (i / 199), (i / 233), (i / 271), (i / 71)
from generate_series(1, 100000) s(i)
order by i + mod(i::bigint * 946263, 469), md5(i::text);

vacuum freeze t_23;
analyze t_23;
begin;
set enable_seqscan = off;
set enable_bitmapscan = off;
set enable_indexonlyscan = off;
set cursor_tuple_fraction = 1.0;

declare c_23 scroll cursor for
  select *
  from t_23
  where
    a in (0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252, 256, 260, 264, 268, 272, 276, 280, 284, 288, 292, 296, 300, 304, 308, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 356, 360)
    and b in (0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252, 256, 260, 264, 268, 272, 276, 280, 284, 288, 292, 296, 300, 304, 308, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 356, 360, 364, 368, 372, 376, 380, 384, 388, 392, 396, 400, 404, 408, 412, 416, 420, 424, 428, 432, 436, 440, 444, 448, 452, 456, 460, 464, 468, 472, 476, 480, 484, 488, 492, 496, 500)
    and c in (0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252, 256, 260, 264, 268, 272, 276, 280, 284, 288, 292, 296, 300, 304, 308, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 356, 360, 364, 368, 372, 376, 380, 384, 388, 392, 396, 400, 404, 408, 412, 416, 420, 424, 428)
    and d in (0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252, 256, 260, 264, 268, 272, 276, 280, 284, 288, 292, 296, 300, 304, 308, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 356, 360, 364, 368)
    and e in (0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252, 256, 260, 264, 268, 272, 276, 280, 284, 288, 292, 296, 300, 304, 308, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 356, 360, 364, 368, 372, 376, 380, 384, 388, 392, 396, 400, 404, 408, 412, 416, 420, 424, 428, 432, 436, 440, 444, 448, 452, 456, 460, 464, 468, 472, 476, 480, 484, 488, 492, 496, 500, 504, 508, 512, 516, 520, 524, 528, 532, 536, 540, 544, 548, 552, 556, 560, 564, 568, 572, 576, 580, 584, 588, 592, 596, 600, 604, 608, 612, 616, 620, 624, 628, 632, 636, 640, 644, 648, 652, 656, 660, 664, 668, 672, 676, 680, 684, 688, 692, 696, 700, 704, 708, 712, 716, 720, 724, 728, 732, 736, 740, 744, 748, 752, 756, 760, 764, 768, 772, 776, 780, 784, 788, 792, 796, 800, 804, 808, 812, 816, 820, 824, 828, 832, 836, 840, 844, 848, 852, 856, 860, 864, 868, 872, 876, 880, 884, 888, 892, 896, 900, 904, 908, 912, 916, 920, 924, 928, 932, 936, 940, 944, 948, 952, 956, 960, 964, 968, 972, 976, 980, 984, 988, 992, 996, 1000, 1004, 1008, 1012, 1016, 1020, 1024, 1028, 1032, 1036, 1040, 1044, 1048, 1052, 1056, 1060, 1064, 1068, 1072, 1076, 1080, 1084, 1088, 1092, 1096, 1100, 1104, 1108, 1112, 1116, 1120, 1124, 1128, 1132, 1136, 1140, 1144, 1148, 1152, 1156, 1160, 1164, 1168, 1172, 1176, 1180, 1184, 1188, 1192, 1196, 1200, 1204, 1208, 1212, 1216, 1220, 1224, 1228, 1232, 1236, 1240, 1244, 1248, 1252, 1256, 1260, 1264, 1268, 1272, 1276, 1280, 1284, 1288, 1292, 1296, 1300, 1304, 1308, 1312, 1316, 1320, 1324, 1328, 1332, 1336, 1340, 1344, 1348, 1352, 1356, 1360, 1364, 1368, 1372, 1376, 1380, 1384, 1388, 1392, 1396, 1400, 1404, 1408)
  order by a asc, b asc, c asc, d asc, e asc;
fetch forward 34 from c_23;
fetch backward 76 from c_23;
fetch forward 50 from c_23;
fetch forward 26 from c_23;
commit;

set client_min_messages=error;
drop table if exists t_14;
reset client_min_messages;

create table t_14 (a bigint) with (fillfactor = 10);
create index on t_14 (a) with (fillfactor = 83, deduplicate_items = off);

insert into t_14
select (i / 181)
from generate_series(1, 100000) s(i)
order by i + mod(i::bigint * 884007, 485), md5(i::text);

vacuum freeze t_14;
analyze t_14;

begin;
set enable_seqscan = off;
set enable_bitmapscan = off;
set enable_indexonlyscan = on;
set cursor_tuple_fraction = 1.0;
declare c_14 scroll cursor for
  select
    *
  from
    t_14
  where
    a in (0, 6, 12, 18, 24, 30, 36, 42, 48, 54, 60, 66, 72, 78, 84, 90, 96, 102, 108, 114, 120, 126, 132, 138, 144, 150, 156, 162, 168, 174, 180, 186, 192, 198, 204, 210, 216, 222, 228, 234, 240, 246, 252, 258, 264, 270, 276, 282, 288, 294, 300, 306, 312, 318, 324, 330, 336, 342, 348, 354, 360, 366, 372, 378, 384, 390, 396, 402, 408, 414, 420, 426, 432, 438, 444, 450, 456, 462, 468, 474, 480, 486, 492, 498, 504, 510, 516, 522, 528, 534, 540, 546, 552)
  order by a asc;
select 1 from pg_buffercache_evict_all();
fetch forward 73 from c_14;
select 1 from pg_buffercache_evict_all();
fetch forward 56 from c_14;
select 1 from pg_buffercache_evict_all();
fetch forward 43 from c_14;
select 1 from pg_buffercache_evict_all();
fetch forward 98 from c_14;
select 1 from pg_buffercache_evict_all();
fetch forward 25 from c_14;
select 1 from pg_buffercache_evict_all();
fetch forward 83 from c_14;
select 1 from pg_buffercache_evict_all();
fetch backward 27 from c_14;
commit;

--
-- 2026-01-29 19:59
--
-- Bug that I ran into while trying to make read stream incrementally pause
--
set client_min_messages=error;
drop table if exists wrong_blknum_table;
reset client_min_messages;
create table wrong_blknum_table(
  a bigint,
  b bigint,
  c bigint,
  d bigint,
  e bigint,
  f bigint,
  g bigint
)
with (
  fillfactor = 87
);

create index wrong_blknum_table_idx on wrong_blknum_table(a, b, c, d, e, f, g) with (fillfactor = 87, deduplicate_items = on);

insert into wrong_blknum_table
select (i / 67), (i / 37), (i / 59), (i / 37), (i / 2), (i / 7), (i / 11)
from generate_series(1, 1000) s(i)
order by i + mod(i::bigint * 725554, 4), md5(i::text);

vacuum freeze wrong_blknum_table;
analyze wrong_blknum_table;

set enable_seqscan = off;
set enable_bitmapscan = off;
set enable_indexonlyscan = off;
set cursor_tuple_fraction = 1.0;

begin;
declare c_13 scroll cursor for
  select *
  from wrong_blknum_table
  order by a asc, b asc, c asc, d asc, e asc, f asc, g asc;

select 1 from pg_buffercache_evict_all();
fetch forward 444 from c_13;
select 1 from pg_buffercache_evict_all();
fetch backward 444 from c_13;
select 1 from pg_buffercache_evict_all();
fetch forward 445 from c_13;
select 1 from pg_buffercache_evict_all();
fetch backward 445 from c_13;
commit;

--
-- 2026-02-04 13:04 Merge join NoMovementScanDirection bug from Tomas' new stress test
--
-- (gdb) bt
-- #0  0x00007936ed482e5c in __pthread_kill_implementation () from /lib64/libc.so.6
-- #1  0x00007936ed428f0e in raise () from /lib64/libc.so.6
-- #2  0x00007936ed4106d0 in abort () from /lib64/libc.so.6
-- #3  0x0000000000bc8326 in ExceptionalCondition (conditionName=0xcf9af8 "direction != NoMovementScanDirection", fileName=0xcf96a0 "heapam_handler.c", lineNumber=783) at assert.c:127
-- #4  0x000000000047ac9d in heapam_getnext_stream (stream=0x44c8168, callback_private_data=0x44c7b98, per_buffer_data=0x0) at heapam_handler.c:783
-- #5  0x0000000000985bae in read_stream_get_block (stream=0x44c8168, per_buffer_data=0x0) at read_stream.c:199
-- #6  0x0000000000986319 in read_stream_look_ahead (stream=0x44c8168) at read_stream.c:465
-- #7  0x0000000000986cde in read_stream_next_buffer (stream=0x44c8168, per_buffer_data=0x0) at read_stream.c:900
-- #8  0x00000000009871eb in read_stream_reset (stream=0x44c8168) at read_stream.c:1105
-- #9  0x0000000000479cee in heapam_index_fetch_reset (scan=0x44c7ff8) at heapam_handler.c:126
-- #10 0x0000000000496f8b in table_index_fetch_reset (scan=0x44c7ff8) at ../../../../src/include/access/tableam.h:1217
-- #11 0x0000000000497e15 in index_restrpos (scan=0x44c7b98) at indexam.c:476
-- #12 0x000000000074219d in ExecIndexRestrPos (node=0x4493968) at nodeIndexscan.c:899
--
set client_min_messages=error;
drop table if exists t_1_1;
drop table if exists t_1_2;
reset client_min_messages;

create unlogged table t_1_1(
  a bigint, b bigint, c bigint, d bigint,
  e bigint, f bigint, g bigint, h bigint
) with (fillfactor = 42);

create unlogged table t_1_2(
  a bigint, b bigint, c bigint, d bigint,
  e bigint, f bigint, g bigint, h bigint
) with (fillfactor = 42);

create index on t_1_1 (a, b, c, d, e, f, g, h) with (fillfactor = 95, deduplicate_items = off);
create index on t_1_2 (a, b, c, d, e, f, g, h) with (fillfactor = 95, deduplicate_items = off);

insert into t_1_1
select (i / 229), (i / 461), (i / 359), (i / 383), (i / 137), (i / 163), (i / 251), (i / 491)
from generate_series(1, 100000) s(i)
order by i + mod(i::bigint * 699664, 971), md5(i::text);

insert into t_1_2
select (i / 229), (i / 461), (i / 359), (i / 383), (i / 137), (i / 163), (i / 251), (i / 491)
from generate_series(1, 100000) s(i)
order by i + mod(i::bigint * 699664, 971), md5(i::text);

vacuum analyze t_1_1;
vacuum analyze t_1_2;

set enable_seqscan = off;
set enable_bitmapscan = off;
set enable_nestloop = off;
set enable_hashjoin = off;
set enable_indexonlyscan = off;
set cursor_tuple_fraction = 1.0;

begin;
declare c_1 scroll cursor for select * from t_1_1 join t_1_2 using (a, b, c, d, e, f, g, h);
select 1 from pg_buffercache_evict_all();

fetch forward 24003 from c_1;
select 1 from pg_buffercache_evict_all();
fetch forward 88 from c_1;
commit;

--
-- 2026-02-04 14:21 Another cursor test case failure
--
set client_min_messages=error;
drop table if exists t_2;
reset client_min_messages;

create unlogged table t_2 (a bigint, b bigint, c bigint) with (fillfactor = 67);
create index on t_2 (a, b, c) with (fillfactor = 12, deduplicate_items = off);

insert into t_2
select (i / 11), (i / 53), (i / 53)
from generate_series(1, 1000) s(i)
order by i + mod(i::bigint * 662056, 8), md5(i::text);

vacuum (freeze, analyze) t_2;

set enable_seqscan = off;
set enable_bitmapscan = off;
set enable_indexonlyscan = off;
set cursor_tuple_fraction = 1.0;

begin;
declare c_2 scroll cursor for select * from t_2 order by a asc, b asc, c asc;

select 1 from pg_buffercache_evict_all();
fetch forward 231 from c_2;
select 1 from pg_buffercache_evict_all();
fetch backward 231 from c_2;
select 1 from pg_buffercache_evict_all();
fetch forward 232 from c_2;
select 1 from pg_buffercache_evict_all();
fetch forward 232 from c_2;
commit;

--
-- 2026-02-23 21:09 Assertion failure when lastItem visibility info hasn't
-- been set when we go to release leaf page buffer pin
--
set client_min_messages=error;
drop table if exists lastitem_vis_notset;
reset client_min_messages;

create table lastitem_vis_notset (a int);
create index on lastitem_vis_notset (a);
insert into lastitem_vis_notset values (1), (2), (3);
vacuum freeze lastitem_vis_notset;

begin;
set enable_seqscan = off;
declare visnotset scroll cursor for select * from lastitem_vis_notset order by a;
fetch forward 1 from visnotset;
fetch backward 1 from visnotset;
fetch forward 2 from visnotset;
commit;
