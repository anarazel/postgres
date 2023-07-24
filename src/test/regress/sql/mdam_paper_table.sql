-- See also: original "big" MDAM test table + query file:
-- microbenchmarks/mdam_paper_table.sql

set work_mem='100MB';
set default_statistics_target=2000;
set effective_cache_size='24GB';
set random_page_cost=2.0;
set track_io_timing to off;
set enable_seqscan to off;
-- set statement_timeout='4s';
set client_min_messages=error;

-- Set log_btree_verbosity to 1 without depending on having that patch
-- applied (HACK, just sets commit_siblings instead when we don't have that
-- patch available):
select set_config((select coalesce((select name from pg_settings where name = 'log_btree_verbosity'), 'commit_siblings')), '1', false);

-- set skipscan_skipsupport_enabled=false;
set vacuum_freeze_min_age = 0;
set cursor_tuple_fraction=1.000;

-- Consistently prefer bitmap scans:
set enable_indexscan=off;
set enable_sort=on;
set enable_hashagg=off;

-- Set aio stuff explicitly:
set io_combine_limit = 16;
set effective_io_concurrency = 100;

drop table if exists small_sales_mdam_paper;
reset client_min_messages;

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

select setseed(0.5);

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

vacuum analyze small_sales_mdam_paper;

-- (Feb 1 2025) Basic smoke test.
--
-- This was regressed (in that it'd be a full index scan) during work on
-- fixing Heikki's regression with no-suffix-truncation due to
-- CREATE INDEX case (see microbenchmarks/heikki-skipscan-adversarial.sql)
select count(*) from small_sales_mdam_paper where sdate = '1995-01-02';
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from small_sales_mdam_paper where sdate = '1995-01-02';

-- This very similar case was _not_ regressed when this happened, presumably
-- because the lack of matching tuples accidentally avoids the confusion:
-- (Note that "where sdate = '1995-01-01'" will find no matching tuples)
select count(*) from small_sales_mdam_paper where sdate = '1995-01-01';
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
select count(*) from small_sales_mdam_paper where sdate = '1995-01-01';

-- skip dept, sdate range, in lists:
prepare first as
select sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  -- dept omitted here
  sdate between '1995-01-01' and '1995-01-05'
  and item_class in (1, 15)
  and store in (15, 25, 45)
group by sdate, item_class, store;

execute first;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- win: patch 243, master 2371
execute first;
deallocate first;

-- No skip dept, sdate range, in lists:
prepare second as
select sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept = 7
  and sdate between '1995-01-01' and '1995-01-05'
  and item_class in (1, 15)
  and store in (15, 25, 45)
group by sdate, item_class, store;

execute second;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- tiny win (24 patch vs 25 master)
execute second;
deallocate second;

-- Backwards scan variant: No skip dept, sdate range, in lists:
set enable_indexscan = on;
set enable_bitmapscan to off;
prepare third as
select dept, sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept = 7
  and sdate between '1995-01-01' and '1995-01-05'
  and item_class in (1, 15)
  and store in (15, 25, 45)
group by dept, sdate, item_class, store
order by dept desc, sdate desc, item_class desc, store desc;

execute third;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- backwards scan, master now wins: one buffer hit more (52 patch vs 48 master)
execute third;
deallocate third;
set enable_indexscan = off;
set enable_bitmapscan to on;

-- No skip dept, sdate range, in lists (similar to last):
prepare fourth as
select sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept = 7
  and sdate between '1995-01-15' and '1995-01-30' -- Different date range compared to last
  and item_class in (1, 15)
  and store in (15, 25, 45)
group by sdate, item_class, store;

execute fourth;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- close to parity, but we lose by a little now (96 patch vs 86 master)
execute fourth;
deallocate fourth;

-- dept range, sdate range, in lists:
prepare fifth as
select sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept between 5 and 7 -- only difference with last test is that it was "dept = 7" here instead
  and sdate between '1995-01-01' and '1995-01-05'
  and item_class in (1, 15)
  and store in (15, 25, 45)
group by sdate, item_class, store;

execute fifth;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- much larger win now (72 patch vs 710 master)
execute fifth;
deallocate fifth;

--------------------------------------
-- dept range, skip sdate, in lists --
--------------------------------------
prepare sixth as
select sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept between 5 and 7
  and item_class in (1, 15)
  and store in (15, 25, 45)
group by sdate, item_class, store;

--
-- master "beats" patch for 'sixth' query: 815 hits for patch vs only 710 for master.
--
-- However, that's a pretty dubious characterization.  While there are many
-- individual cases where we skip ahead to the very next page using _bt_first,
-- those will still manage to skip by hundreds of index tuples, suggesting
-- that we're at least breaking even on more relevant metrics like CPU
-- cycles/latency.
--
-- Note also that this'll frequently skip over a whole page, which still looks
-- like a regression if we just go on buffers hit here (an extra 2 internal
-- page hits looks more expensive than the leaf page hit that we've saved,
-- but isn't -- not in any practical sense).  That happens quite frequently,
-- for maybe half of all primitive scans seen.  We shouldn't assume that a
-- case that seems to me to be clear win (when I look at tree dump
-- instrumentation) is actually a loss, based on a naive reading of the buffer
-- hits shown here.
execute sixth;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute sixth;
deallocate sixth;

-- dept range, = sdate, in lists:
prepare seventh as
select sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept between 5 and 7
  and sdate = '1995-01-10'
  and item_class in (1, 15)
  and store in (15, 25, 45)
group by sdate, item_class, store;

execute seventh;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- big win now (18 patch vs 667 master)
execute seventh;
deallocate seventh;

----------------------------------
-- dept =, skip sdate, in lists --
----------------------------------
prepare eighth as
select sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept = 6 -- not a range, unlike last query
  and item_class in (1, 15)
  and store in (15, 25, 45)
group by sdate, item_class, store;

-- master "beats" patch for 'eighth' query: 273 hits for patch, vs 238 for master.
-- However, this seems quite acceptable here; this query is very similar to
-- the "sixth" query, so the same rationale for why this is okay applies here
-- too.
execute eighth;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute eighth;
deallocate eighth;


-- Skip dept, sdate range, in lists:
prepare ninth as
select sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  sdate between '1995-01-01' and '1995-01-05'
  and item_class in (5, 10, 15)
  and store in (25, 45)
group by sdate, item_class, store;

execute ninth;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF)
execute ninth;
deallocate ninth;

-- dept range, sdate range, item_class range, store range:
prepare tenth as
select sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept between 5 and 7
  and sdate between '1995-01-01' and '1995-01-05'
  and item_class between 1 and 15
  and store between 15 and 25
group by sdate, item_class, store;

-- (XXX UPDATE August 6) Doing well on this test hinges upon suppressing the
-- Postgres 17 required-in-opposite-direction new primitive scan logic in
-- _bt_advance_array_keys.
--
-- The issue isn't specific to skip arrays, it's just that it only looks like
-- a regression when comparing skip arrays to similar full index scan.
execute tenth;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- Nice win: 83 hits for patch vs 710 for master
execute tenth;
deallocate tenth;

-- dept range, sdate range, item_class range, store range:
prepare eleventh as
select sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept between 5 and 7
  and sdate between '1995-01-01' and '1995-01-05'
  and item_class between 1 and 15
  and store = 10 -- Not a range, unlike last time, but otherwise the same
group by sdate, item_class, store;

execute eleventh;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- nice win, 89 for patch vs 710 for master
execute eleventh;
deallocate eleventh;

-- dept range, sdate range, item_class range, store range:
prepare twelfth as
select sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept between 5 and 7
  and sdate between '1995-01-01' and '1995-01-05'
  and item_class between 1 and 15
  and store = -1 -- Not a range, no matching tuples
group by sdate, item_class, store;

execute twelfth;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- nice win, 89 for patch vs 710 for master
execute twelfth;
deallocate twelfth;


-- Backwards scan variant: dept range, sdate range, item_class range, store range:
set enable_indexscan = on;
set enable_bitmapscan to off;
prepare thirteenth as
select dept, sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept between 5 and 7
  and sdate between '1995-01-01' and '1995-01-05'
  and item_class between 1 and 15
  and store between 15 and 25
group by dept, sdate, item_class, store
order by dept desc, sdate desc, item_class desc, store desc;

execute thirteenth;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- 2163 hits for patch vs 2476 for master
execute thirteenth;
deallocate thirteenth;

-- Backwards scan variant: dept range, sdate range, item_class range, store range:
prepare fourteenth as
select dept, sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept between 5 and 7
  and sdate between '1995-01-01' and '1995-01-05'
  and item_class between 1 and 15
  and store = 10 -- Not a range, unlike last time, but otherwise the same
group by dept, sdate, item_class, store
order by dept desc, sdate desc, item_class desc, store desc;

execute fourteenth;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- good: 202 hits for patch vs 509 for master
execute fourteenth;
deallocate fourteenth;
set enable_indexscan = off;
set enable_bitmapscan to on;

prepare dont_repeat_leaf_readpage as
select dept, sdate, item_class, store, sum(total_sales)
from small_sales_mdam_paper
where
  dept in (5, 7)
  and sdate between '1995-01-02' and '1995-01-10'
  and item_class >= 3
  and store <= 3
group by dept, sdate, item_class, store
order by dept, sdate, item_class, store;

-- (XXX August 5 UPDATE) This gets 124 index hits vs 100 for master, all of
-- which are extra internal page accesses from doing extra primitive index
-- scans.  It's possible to reach parity with master on that measure, but it
-- looks very complicated.  I'd rather not go that way, especially considering
-- that the wall clock time for this query is clearly lower than it is on
-- master in spite of the "regression".
--
-- (July 31) This test showed an issue with repeat leaf page accesses, which
-- became evident when I enabled the relevant BMS based instrumentation.
--
-- Ultimate fix for this bug was to teach _bt_tuple_before_array_skeys to call
-- _bt_binsrch_skiparray_skey for -inf scan keys, as a substitute for the
-- call to _bt_compare_array_skey performed when current array element is set
-- in search-type scan key (the call that wasn't and couldn't be made when
-- scan key is marked SK_BT_NEG_INF).
--
-- First leaf page repeatedly accessed was one of Blocknumber 1535, shown
-- here:
/*
ERROR:  pageNum 1535 already visited


👾  btbeginscan to begin scan of index "small_mdam_idx" in worker -1
♻️  btrescan
btrescan: BTScanPosInvalidate() called for markPos
🚜  btgetbitmap
_bt_preprocess_keys:  scan->keyData[0]: [ strategy: = , attno: 1/"dept", func: int4eq, flags: [SK_SEARCHARRAY] ]
                      scan->keyData[1]: [ strategy: >=, attno: 2/"sdate", func: date_ge, flags: [] ]
                      scan->keyData[2]: [ strategy: <=, attno: 2/"sdate", func: date_le, flags: [] ]
                      scan->keyData[3]: [ strategy: >=, attno: 3/"item_class", func: int4ge, flags: [] ]
                      scan->keyData[4]: [ strategy: <=, attno: 4/"store", func: int4le, flags: [] ]
_bt_preprocess_keys:    so->keyData[0]: [ strategy: = , attno: 1/"dept", func: int4eq, flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD] ]
                        so->keyData[1]: [ strategy: = , attno: 2/"sdate", func: date_eq, flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD, SK_BT_SKIP] ]
                        so->keyData[2]: [ strategy: = , attno: 3/"item_class", func: int4eq, flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD, SK_BT_SKIP] ]
                        so->keyData[3]: [ strategy: <=, attno: 4/"store", func: int4le, flags: [SK_BT_REQFWD] ]
_bt_preprocess_keys: scan->numberOfKeys is 5, so->numberOfKeys on output is 4, so->numArrayKeys on output is 3

*** SNIP ***
_bt_readpage: 🍀  766 with 201 offsets/tuples (leftsib 1534, rightsib 1535) ➡️
 _bt_readpage first: (dept, sdate, item_class, store)=(5, 01-04-1995, 17, 1), TID='(3,89)', 0x7f8294632fc8, from non-pivot offnum 2 started page
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-04-1995, 17, 1) with TID (3,89), 0x7f8294632fc8
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-04-1995, 17, 2) with TID (69,113), 0x7f8294632fb0
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-04-1995, 17, 3) with TID (136,1), 0x7f8294632f98
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-04-1995, 17, 4) with TID (202,25), 0x7f8294632f80
*** SNIP ***
_bt_advance_array_keys, sktrig: 1, pivot tuple: (dept, sdate, item_class, store)=(5, 01-05-1995), 0x7f8294632fe0

  - sk: 0, sk_attno: 1, cur_elem:    0, num_elems:    2, val: 5
  - sk: 1, sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-04-1995            <--
  - sk: 2, sk_attno: 3, cur_elem:    0, num_elems:   -1, val: 21

  + sk: 0, sk_attno: 1, cur_elem:    0, num_elems:    2, val: 5
  + sk: 1, sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-05-1995
  + sk: 2, sk_attno: 3, cur_elem:    0, num_elems:   -1, val: ????? SK_BT_NEG_INF

  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995) with TID -inf, 0x7f8294632fe0
 _bt_readpage final: (dept, sdate, item_class, store)=(5, 01-05-1995), 0x7f8294632fe0, continuescan high key check did not set so->currPos.moreRight=false ➡️  🟢
 _bt_readpage stats: currPos.firstItem: 0, currPos.lastItem: 11, nmatching: 12 ✅
_bt_readpage: 🍀  1535 with 201 offsets/tuples (leftsib 766, rightsib 387) ➡️        <---------- XXX first access of page 1535
 _bt_readpage first: (dept, sdate, item_class, store)=(5, 01-05-1995, 1, 1), TID='(5,1)', 0x7f8295584fc0, from non-pivot offnum 2 started page
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 1, 1) with TID (5,1), 0x7f8295584fc0
_bt_advance_array_keys, sktrig: 2, tuple: (dept, sdate, item_class, store)=(5, 01-05-1995, 1, 1), 0x7f8295584fc0

  - sk: 0, sk_attno: 1, cur_elem:    0, num_elems:    2, val: 5
  - sk: 1, sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-05-1995
  - sk: 2, sk_attno: 3, cur_elem:    0, num_elems:   -1, val: ????? SK_BT_NEG_INF            <--

  + sk: 0, sk_attno: 1, cur_elem:    0, num_elems:    2, val: 5
  + sk: 1, sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-05-1995
  + sk: 2, sk_attno: 3, cur_elem:    0, num_elems:   -1, val: ????? SK_BT_NEG_INF            XXX No change!

 _bt_readpage final: (dept, sdate, item_class, store)=(5, 01-05-1995, 1, 1), TID='(5,1)', 0x7f8295584fc0, from non-pivot offnum 2 set so->currPos.moreRight=false ➡️  🛑
 _bt_readpage stats: currPos.firstItem: 0, currPos.lastItem: -1, nmatching: 0 ❌
_bt_readnextpage: ScanDirectionIsForward() case ran out of pages to the right ➡️  🛑
_bt_readnextpage: BTScanPosInvalidate() called for currPos
_bt_steppage: _bt_readnextpage() returns false so we do too

➕     ➕     ➕
_bt_first: sk_attno 1. val: 5, func: btint4cmp, flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD]
           sk_attno 2. val: 01-05-1995, func: date_cmp, flags: [SK_SEARCHARRAY, SK_BT_REQFWD, SK_BT_REQBKWD, SK_BT_SKIP]
           sk_attno 3. val: 3, func: btint4cmp, flags: []
           with strat_total='>=', inskey.keys=3, inskey.nextkey=0, inskey.backward=0
🔽  ==================== _bt_search begin at root 343 level 2 ====================
_bt_moveright: blk 343 is rightmost
_bt_search: sk > (dept, sdate, item_class, store)=(4, 01-22-1995), sk <= (dept, sdate, item_class, store)=(5, 01-18-1995)
🔽  -------------------- descended to child blk 469 level 1 --------------------
_bt_moveright: (dept, sdate, item_class, store)=(5, 01-18-1995), high key no move right
_bt_search: sk > (dept, sdate, item_class, store)=(5, 01-05-1995), sk <= (dept, sdate, item_class, store)=(5, 01-05-1995, 5)
🔽  -------------------- descended to child blk 1535 level 0 --------------------
_bt_moveright: (dept, sdate, item_class, store)=(5, 01-05-1995, 5), high key no move right
⏹️  ==================== _bt_search end ====================
_bt_readpage: 🍀  1535 with 201 offsets/tuples (leftsib 766, rightsib 387) ➡️          <-------------------- XXX second access of page 1535
 _bt_readpage first: (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 1), TID='(5,3)', 0x7f8295584900, from non-pivot offnum 102 started page
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 1) with TID (5,3), 0x7f8295584900
_bt_advance_array_keys, sktrig: 2, tuple: (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 1), 0x7f8295584900

  - sk: 0, sk_attno: 1, cur_elem:    0, num_elems:    2, val: 5
  - sk: 1, sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-05-1995
  - sk: 2, sk_attno: 3, cur_elem:    0, num_elems:   -1, val: ????? SK_BT_NEG_INF            <--

  + sk: 0, sk_attno: 1, cur_elem:    0, num_elems:    2, val: 5
  + sk: 1, sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-05-1995
  + sk: 2, sk_attno: 3, cur_elem:    0, num_elems:   -1, val: 3

  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 1) with TID (5,3), 0x7f8295584900
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 2) with TID (71,27), 0x7f82955848e8
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 3) with TID (137,51), 0x7f82955848d0
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 4) with TID (203,75), 0x7f82955848b8
_bt_advance_array_keys, sktrig: 3, tuple: (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 4), 0x7f82955848b8

  - sk: 0, sk_attno: 1, cur_elem:    0, num_elems:    2, val: 5
  - sk: 1, sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-05-1995
  - sk: 2, sk_attno: 3, cur_elem:    0, num_elems:   -1, val: 3

  + sk: 0, sk_attno: 1, cur_elem:    0, num_elems:    2, val: 5
  + sk: 1, sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-05-1995
  + sk: 2, sk_attno: 3, cur_elem:    0, num_elems:   -1, val: 4

  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 5) with TID (269,99), 0x7f82955848a0
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 6) with TID (335,123), 0x7f8295584888
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 7) with TID (402,11), 0x7f8295584870
   _bt_readpage: look ahead skipping forward 6 items, from 108 to 114
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 13) with TID (799,19), 0x7f82955847e0
   _bt_readpage: look ahead skipping forward 11 items, from 114 to 125
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 24) with TID (1527,11), 0x7f82955846d8
   _bt_readpage: look ahead skipping forward 21 items, from 125 to 146
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 45) with TID (2916,107), 0x7f8295583f10
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 46) with TID (2982,131), 0x7f8295583eb0
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 47) with TID (3049,19), 0x7f8295583e50
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 48) with TID (3115,43), 0x7f8295583df0
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 49) with TID (3181,67), 0x7f8295583d90
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 3, 50) with TID (3247,91), 0x7f8295583d30
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 4, 1) with TID (5,4), 0x7f82955845a0
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 4, 2) with TID (71,28), 0x7f8295584588
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 4, 3) with TID (137,52), 0x7f8295584570
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 4, 4) with TID (203,76), 0x7f8295584558
_bt_advance_array_keys, sktrig: 3, tuple: (dept, sdate, item_class, store)=(5, 01-05-1995, 4, 4), 0x7f8295584558

  - sk: 0, sk_attno: 1, cur_elem:    0, num_elems:    2, val: 5
  - sk: 1, sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-05-1995
  - sk: 2, sk_attno: 3, cur_elem:    0, num_elems:   -1, val: 4

  + sk: 0, sk_attno: 1, cur_elem:    0, num_elems:    2, val: 5
  + sk: 1, sk_attno: 2, cur_elem:    0, num_elems:   -1, val: 01-05-1995
  + sk: 2, sk_attno: 3, cur_elem:    0, num_elems:   -1, val: 5

   _bt_readpage: look ahead skipping forward 47 items, from 155 to 202
  _bt_checkkeys: comparing (dept, sdate, item_class, store)=(5, 01-05-1995, 5) with TID -inf, 0x7f8295584fd8
 _bt_readpage final: (dept, sdate, item_class, store)=(5, 01-05-1995, 5), 0x7f8295584fd8, continuescan high key check did not set so->currPos.moreRight=false ➡️  🟢
*/

execute dont_repeat_leaf_readpage;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) -- Master branch gets only 100 index page hits
execute dont_repeat_leaf_readpage;
deallocate dont_repeat_leaf_readpage;
