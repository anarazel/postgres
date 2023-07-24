set work_mem='100MB';
set effective_cache_size='24GB';
set random_page_cost=2.0;
set track_io_timing to off;
set enable_seqscan to off;
set enable_indexonlyscan to off;
set client_min_messages=error;
set vacuum_freeze_min_age = 0;
set cursor_tuple_fraction=1.000;
reset client_min_messages;

-- Set log_btree_verbosity to 1 without depending on having that patch
-- applied (HACK, just sets commit_siblings instead when we don't have that
-- patch available):
select set_config((select coalesce((select name from pg_settings where name = 'log_btree_verbosity'), 'commit_siblings')), '1', false);

set client_min_messages=error;
create extension if not exists btree_gist;
drop table if exists multi_test_gist;
reset client_min_messages;

create unlogged table multi_test_gist(
  a int,
  b int
);

create index multi_test_gist_idx on multi_test_gist using gist(a, b);

insert into multi_test_gist
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

vacuum analyze multi_test_gist;

-- Simpler case
-- Should only need to scan root page (3) plus a single leaf page (4)
--
-- This means that _bt_checkkeys() continuescan handling mustn't get confused
-- about boundary conditions  in the presence of relatively complicated cases,
-- which this is -- multiple so->numArrayKeys is fairly rare.
select * from multi_test_gist where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);

-- Harder case
-- Should only need to scan root page (3) plus a single leaf page (4).  This
-- is a bit trickier for _bt_checkkeys()-adjacent logic.
select * from multi_test_gist where a in (123, 182, 183) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (123, 182, 183) and b in (1,2);

-- Hard case
-- Also needs to scan root page (3) plus leaf page 4 (like "Simpler case").
-- But this time we can't avoid going to a second leaf page -- leaf page 5.
-- That's where matches exceeding (184, -inf) are located.
select * from multi_test_gist where a in (182, 183, 184) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (182, 183, 184) and b in (1,2);

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
select * from multi_test_gist where a in (182, 183, 245) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (182, 183, 245) and b in (1,2); -- 4 buffer hits

-- Harder luck case
-- Two _bt_search descents this time (we _bt_first once we
-- reach page 5 because its high key indicates that it's time to quit gambling)
--
-- XXX UPDATE (December 3) Not anymore, no more moving to right page when our
-- high key lacks an exact match for non-truncated columns.
select * from multi_test_gist where a in (182, 183, 306) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (182, 183, 306) and b in (1,2); -- 4 buffer hits

-- Not-SK_BT_REQFWD-but-still-insertion-scankey case
--
-- This is an example of how insertion scankey can have an attribute/value for
-- "b", even though "b" entry in search-type scankey doesn't end up SK_BT_REQFWD:
-- (_bt_array_continuescan actually encounters this directly, too)
select * from multi_test_gist where a in (3,4,5) and b > 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b > 0;
-- Variant (for good luck)
select * from multi_test_gist where a in (3,4,5) and b >= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b >= 0;
-- This time we make "a" touch a boundary, in the style of "harder case":
select * from multi_test_gist where a in (123, 182, 183) and b > 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (123, 182, 183) and b > 0; -- 2 buffer hits
-- This time we make "a" touch a boundary "inside the high key":
-- XXX UPDATE (February 28): This is 4 index buffer hits due to the
-- restriction on inequalities required in the opposite-to-scan direction
-- only.
select * from multi_test_gist where a in (123, 182, 183, 184) and b > 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (123, 182, 183, 184) and b > 0; -- 4 buffer hits

-- This time we make "b" search-type scankey required:
--
-- This is an example of the opposite: where an insertion scan key lacks an
-- entry corresponding to a search-type scankey's SK_BT_REQFWD entry.
-- (_bt_array_continuescan actually encounters this directly, too)
select * from multi_test_gist where a in (3,4,5) and b < 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b < 0;
-- Variant (for good luck)
select * from multi_test_gist where a in (3,4,5) and b < 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b < 1;
-- This time we make "a" touch a boundary, in the style of "harder case":
select * from multi_test_gist where a in (123, 182, 183) and b < 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (123, 182, 183) and b < 3; -- 2 buffer hits
-- This time we make "a" touch a boundary "inside the high key":
-- XXX UPDATE (February 28): This is only 3 index buffer hits due to not
-- running afoul of the restriction on inequalities required in the
-- opposite-to-scan direction only (this inequality is required in the scan
-- direction, so we're good) .
select * from multi_test_gist where a in (123, 182, 183, 184) and b < 3;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (123, 182, 183, 184) and b < 3; -- 3 buffer hits

-- Simpler case
-- As above.
select * from multi_test_gist where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);

-- Now as a backwards scan
select * from multi_test_gist where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12)
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12)
order by a desc, b desc;

-- Hard case
-- As above.
select * from multi_test_gist where a in (182, 183, 184) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (182, 183, 184) and b in (1,2);
-- Hard case backwards scan variant (just for coverage):
select * from multi_test_gist where a in (182, 183, 184) and b in (1,2) order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (182, 183, 184) and b in (1,2) order by a desc, b desc;

-- Hard luck case
-- As above.
select * from multi_test_gist where a in (182, 183, 245) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (182, 183, 245) and b in (1,2);

-- Harder luck case
-- As above.
select * from multi_test_gist where a in (182, 183, 306) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (182, 183, 306) and b in (1,2);

-- Not-SK_BT_REQFWD-but-still-insertion-scankey case
-- As above.
select * from multi_test_gist where a in (3,4,5) and b > 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b > 0;
-- As a backwards scan:
select * from multi_test_gist where a in (3,4,5) and b > 0
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b > 0
order by a desc, b desc;

-- Variant (for good luck)
select * from multi_test_gist where a in (3,4,5) and b >= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b >= 0;
-- As a backwards scan:
select * from multi_test_gist where a in (3,4,5) and b >= 0
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b >= 0
order by a desc, b desc;

-- This time we make "b" search-type scankey required:
select * from multi_test_gist where a in (3,4,5) and b < 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b < 0;
-- With <= instead of <:
select * from multi_test_gist where a in (3,4,5) and b <= -1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b <= -1;
-- As a backwards scan:
select * from multi_test_gist where a in (3,4,5) and b < 0
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b < 0
order by a desc, b desc;
-- As a backwards scan with <= instead of <:
select * from multi_test_gist where a in (3,4,5) and b <= -1
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b <= -1
order by a desc, b desc;

-- Variant (for good luck)
select * from multi_test_gist where a in (3,4,5) and b < 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b < 1;
-- Variant (for good luck) with <= instead of <
select * from multi_test_gist where a in (3,4,5) and b <= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b <= 0;
-- As a backwards scan:
select * from multi_test_gist where a in (3,4,5) and b < 1
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b < 1
order by a desc, b desc;
-- As a backwards scan with <= instead of <:
select * from multi_test_gist where a in (3,4,5) and b <= 0
order by a desc, b desc;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b <= 0
order by a desc, b desc;

-- Simpler case
-- As above.
select * from multi_test_gist where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (183) and b in (1,2,3,4,5,6,7,8,9,10,11,12);

-- Hard case
-- As above.
select * from multi_test_gist where a in (182, 183, 184) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (182, 183, 184) and b in (1,2);

-- Hard luck case
-- As above.
select * from multi_test_gist where a in (182, 183, 245) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (182, 183, 245) and b in (1,2);

-- Harder luck case
-- As above.
select * from multi_test_gist where a in (182, 183, 306) and b in (1,2);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (182, 183, 306) and b in (1,2);

-- Not-SK_BT_REQFWD-but-still-insertion-scankey case
-- As above.
select * from multi_test_gist where a in (3,4,5) and b > 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b > 0;
-- Variant (for good luck)
select * from multi_test_gist where a in (3,4,5) and b >= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b >= 0;
-- This time we make "b" search-type scankey required:
select * from multi_test_gist where a in (3,4,5) and b < 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b < 0;
-- Variant (for good luck)
select * from multi_test_gist where a in (3,4,5) and b < 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (3,4,5) and b < 1;

-- (November 5) when _bt_preprocess_array_keys has two arrays against the same
-- column that have no intersecting elements, preprocessing will leave the
-- arrays empty.
--
-- This test makes sure that both scan keys are eliminated (not just the
-- second).
select * from multi_test_gist where a in (180,345) and a in (230, 300);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a in (180,345) and a in (230, 300);

-- (February 2) Same again, but this time use bigint arrays -- still should be
-- detected as contradictory within _bt_preprocess_array_keys (not later
-- _bt_preprocess_keys start-of-primscan code paths).
--
-- This doesn't run afoul of any of the implementation restrictions inside
-- _bt_preprocess_array_keys because the array elements themselves are of the
-- same type -- that's all that matters there (the mere presence of cross-type
-- operators does _not_ matter, that's orthogonal).
select * from multi_test_gist where a = any('{180,345}'::bigint[]) and a = any('{230,300}'::bigint[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a = any('{180,345}'::bigint[]) and a = any('{230,300}'::bigint[]);

-- (February 2) Note, however, that this variant cannot use that same
-- _bt_preprocess_array_keys merging/contradictory keys detection.
-- It must use the slightly less efficient handling in _bt_preprocess_keys
-- (works at the level of individual primitive index scans).
--
-- (March 10) UPDATE: Actually, it can, since we now have _bt_preprocess_keys
-- "operate on whole arrays".  This includes having _bt_preprocess_array_keys
-- merge together arrays of different types when necessary.
select * from multi_test_gist where a = any('{180,345}'::bigint[]) and a = any('{230,300}'::integer[]);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select * from multi_test_gist where a = any('{180,345}'::bigint[]) and a = any('{230,300}'::integer[]);

-- (February 27) This is why the "optimistically go to next leaf page given
-- uncertainty with truncated required scan key attributes" might be worth the
-- complexity:
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test_gist
where
  a = any (array[( select array_agg(i) from a)])
  and b in (0, 1, 2, 3);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 8 buffer hits, not 14
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test_gist
where
  a = any (array[( select array_agg(i) from a)])
  and b in (0, 1, 2, 3);

-- (February 28) This similar case also gets to jump from leaf page to leaf
-- page (a little less compelling than the last example, but it still works).
-- While it involves an inequality, it's required in the same direction as the
-- scan so we're good:
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test_gist
where
  a = any (array[( select array_agg(i) from a)])
  and b < 4;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 8 buffer hits, not 14
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test_gist
where
  a = any (array[( select array_agg(i) from a)])
  and b < 4;

-- (February 28) This similar case doesn't get to jump from leaf page to leaf
-- page, even though it's almost the same query as the last one (but I can
-- live with this one).  It's unsupported because it involves an inequality
-- marked required in the opposite direction only (how is _bt_check_compare
-- supposed to even notice this when we arrive on the next page having
-- speculated?):
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test_gist
where
  a = any (array[( select array_agg(i) from a)])
  and b >= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 14 buffer hits, not 8
with a as (
  select i from generate_series(1, 400) i
)
select count(*)
from
  multi_test_gist
where
  a = any (array[( select array_agg(i) from a)])
  and b >= 0;

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
  multi_test_gist
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
  multi_test_gist
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
  multi_test_gist
where
  a = any($1);

execute null_array_confusion (NULL);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
execute null_array_confusion (NULL);
deallocate null_array_confusion;

prepare null_array_elements_coverage as
select a, b
from
  multi_test_gist
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
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a = 181;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a = 181;

-- Simple contradictory, but flip order
select *
from multi_test_gist
where
  a = 181
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test_gist
where
  a = 181
  and a in (1, 99, 182, 183, 184);

-- Simple redundant
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a = 182;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '182' (redundant qual)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a = 182;

-- Simple redundant, but flip order
select *
from multi_test_gist
where
  a = 182
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '182' (redundant qual)
select *
from multi_test_gist
where
  a = 182
  and a in (1, 99, 182, 183, 184);

---------------------------------
-- '>' operator/strategy tests --
---------------------------------

-- Simple > contradictory
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a > 184;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a > 184;

-- Simple > contradictory, but flip order
select *
from multi_test_gist
where
  a > 184
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test_gist
where
  a > 184
  and a in (1, 99, 182, 183, 184);

-- Simple > redundant
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a > 183;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '184' (redundant qual)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a > 183;

-- Simple > redundant, but flip order
select *
from multi_test_gist
where
  a > 183
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '184' (redundant qual)
select *
from multi_test_gist
where
  a > 183
  and a in (1, 99, 182, 183, 184);

----------------------------------
-- '>=' operator/strategy tests --
----------------------------------

-- Simple >= contradictory
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a >= 185;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a >= 185;

-- Simple >= contradictory, but flip order
select *
from multi_test_gist
where
  a >= 185
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test_gist
where
  a >= 185
  and a in (1, 99, 182, 183, 184);

-- Simple >= redundant
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a >= 184;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '184' (redundant qual)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a >= 184;

-- Simple >= redundant, but flip order
select *
from multi_test_gist
where
  a >= 184
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '184' (redundant qual)
select *
from multi_test_gist
where
  a >= 184
  and a in (1, 99, 182, 183, 184);

---------------------------------
-- '<' operator/strategy tests --
---------------------------------

-- Simple < contradictory
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a < 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a < 1;

-- Simple < contradictory, but flip order
select *
from multi_test_gist
where
  a < 1
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test_gist
where
  a < 1
  and a in (1, 99, 182, 183, 184);

-- Simple < redundant
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a < 2;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '1' (redundant qual)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a < 2;

-- Simple < redundant, but flip order
select *
from multi_test_gist
where
  a < 2
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '1' (redundant qual)
select *
from multi_test_gist
where
  a < 2
  and a in (1, 99, 182, 183, 184);

----------------------------------
-- '<=' operator/strategy tests --
----------------------------------

-- Simple <= contradictory
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a <= 0;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a <= 0;

-- Simple <= contradictory, but flip order
select *
from multi_test_gist
where
  a <= 0
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test_gist
where
  a <= 0
  and a in (1, 99, 182, 183, 184);

-- Simple <= redundant
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a <= 1;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '1' (redundant qual)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a <= 1;

-- Simple <= redundant, but flip order
select *
from multi_test_gist
where
  a <= 1
  and a in (1, 99, 182, 183, 184);
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- only 2 buffer hits for '1' (redundant qual)
select *
from multi_test_gist
where
  a <= 1
  and a in (1, 99, 182, 183, 184);

-- (March 11) This was a bug that you didn't catch right away when you added
-- code to exclude array entries using another scankey on same att with >
-- strategy (basically an off-by-one thing)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a > 1000;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 0 buffer hits (contradictory qual)
select *
from multi_test_gist
where
  a in (1, 99, 182, 183, 184)
  and a > 1000;

-- (March 11) This was another bug that you didn't catch right away,
-- discovered shortly after the one exercised by today's previous test case
select *
from multi_test_gist
where
  a in (1, 99, 182, 184)
  and a < 188;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select *
from multi_test_gist
where
  a in (1, 99, 182, 184)
  and a < 188;

