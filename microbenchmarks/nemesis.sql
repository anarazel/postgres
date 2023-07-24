--
-- Adversarial test case for SAOP patch
--
-- The test case (which is highly unrealistic) stresses the SAOP patch.  In
-- particular it maximizes time spent with a buffer lock held (held when
-- scanning each leaf page using _bt_readpage).  This is not intended to be
-- any kind of general-purpose benchmark -- it's just a narrowly focussed
-- stress-test.
--
-- All nbtree SAOP quals use binary searches internally (with the patch).
-- These are unusually expensive, as B-Tree index quals go.  We try to
-- maximize/exaggerate their cost.  Our approach is to maximize the number of
-- binary searches per page, and to make each of those binary searches as
-- expensive as possible.  Any trick I could think of (short of creating my
-- own custom datatype with exceptionally expensive comparisons) was thrown
-- into the mix.
--
-- NOTE: this query isn't expected to ever finish on the master branch (I lost
-- patience after 6 or 7 minutes).
--
-- NOTE: the query also doesn't complete in anything like a reasonable time
-- frame when allowed to use a (parallel) sequential scan.  (To be fair, the
-- hash table optimization added by commit 50e17ad281 can't be used here
-- because we don't use Consts in the SAOP clauses.)

\set VERBOSITY verbose
\pset pager off
\x on

-- Set array sizes for later:
-- \set array_size 4_500_000 -- <-- highest value known to work on horse
\set array_size 500_000

-- Use numeric for everything, so that the additional memory indirection
-- (numeric is a pass-by-value type) makes our binary searches more expensive
drop table if exists nemesis;
create unlogged table nemesis(
  pk numeric,
  hash1 numeric,
  hash2 numeric,
  hash3 numeric,
  hash4 numeric,
  hash5 numeric,
  hash6 numeric,
  hash7 numeric,
  hash8 numeric,
  hash9 numeric,
  hash10 numeric,
  hash11 numeric,
  hash12 numeric,
  hash13 numeric,
  hash14 numeric,
  hash15 numeric,
  hash16 numeric,
  hash17 numeric,
  hash18 numeric,
  hash19 numeric,
  hash20 numeric,
  hash21 numeric,
  hash22 numeric,
  hash23 numeric,
  hash24 numeric,
  hash25 numeric,
  hash26 numeric,
  hash27 numeric,
  hash28 numeric,
  hash29 numeric,
  hash30 numeric,
  hash31 numeric,
  hash32 numeric
);

-- Bulk load with random data
insert into nemesis
select
  i,
  hashint8(i + hashint4(1)::int8),
  hashint8(i + hashint4(2)::int8),
  hashint8(i + hashint4(3)::int8),
  hashint8(i + hashint4(4)::int8),
  hashint8(i + hashint4(5)::int8),
  hashint8(i + hashint4(6)::int8),
  hashint8(i + hashint4(7)::int8),
  hashint8(i + hashint4(8)::int8),
  hashint8(i + hashint4(9))::int8,
  hashint8(i + hashint4(10)::int8),
  hashint8(i + hashint4(11)::int8),
  hashint8(i + hashint4(12)::int8),
  hashint8(i + hashint4(13)::int8),
  hashint8(i + hashint4(14)::int8),
  hashint8(i + hashint4(15)::int8),
  hashint8(i + hashint4(16)::int8),
  hashint8(i + hashint4(17)::int8),
  hashint8(i + hashint4(18)::int8),
  hashint8(i + hashint4(19)::int8),
  hashint8(i + hashint4(20)::int8),
  hashint8(i + hashint4(21)::int8),
  hashint8(i + hashint4(22)::int8),
  hashint8(i + hashint4(23)::int8),
  hashint8(i + hashint4(24)::int8),
  hashint8(i + hashint4(25)::int8),
  hashint8(i + hashint4(26)::int8),
  hashint8(i + hashint4(27)::int8),
  hashint8(i + hashint4(28)::int8),
  hashint8(i + hashint4(29)::int8),
  hashint8(i + hashint4(30)::int8),
  hashint8(i + hashint4(31)::int8),
  hashint8(i + hashint4(32)::int8)
from
  generate_series(1, :array_size) i;

-- Create index after bulk load, with fillfactor=100, so that each leaf page
-- is packed to the brim.  That way there'll be more binary searches per
-- _bt_readpage call: each call has more tuples to process at a time.
--
-- This index has the maximum 32 (aka INDEX_MAX_KEYS) index columns.
-- Here are what two adjacent index tuples from our index look like:
--
-- (hash1, hash2, hash3, ..., hash32)=(-2146618007,   867158713, -980517796,  ...,  -473819985)
-- (hash1, hash2, hash3, ..., hash32)=(-2146617376, -1990434553,  283108503,  ...,  -652505082)
--
-- There is no locality, except with the first column, "hash1".  The other 31
-- columns contain data that is completely random: it has no relationship with
-- other datums from the same tuple, nor is it correlated in any way with
-- datums from adjacent tuples.  We aim to minimize locality of access (every
-- form you can think of) during our index scan.
--
-- XXX We can only fit 26 of these tuples on an 8KiB leaf page (plus a much
-- smaller truncated high key), even with fillfactor=100 (and no use of
-- deduplication).  This tuple shape seems likely to be just about the best
-- way of maximizing the number of binary searches required per _bt_readpage
-- call, despite the attendant "downside" of having larger tuples that force
-- us to place fewer individual tuples on each page.
create index nemesis_idx on nemesis (
  hash1,
  hash2,
  hash3,
  hash4,
  hash5,
  hash6,
  hash7,
  hash8,
  hash9,
  hash10,
  hash11,
  hash12,
  hash13,
  hash14,
  hash15,
  hash16,
  hash17,
  hash18,
  hash19,
  hash20,
  hash21,
  hash22,
  hash23,
  hash24,
  hash25,
  hash26,
  hash27,
  hash28,
  hash29,
  hash30,
  hash31,
  hash32
)
with (fillfactor=100);

-- Be tidy: make sure that we can do an index-only scan (the fact that this is
-- an unlogged table also helps make that happen reliably)
VACUUM nemesis;

-- The query/test itself performs 26*32=832 binary searches per _bt_readpage call.
--
-- Here it's important that every tuple actually satisfy our expensive quals
-- for all 32 index attributes.  If that didn't happen (if an earlier binary
-- search didn't find a precise match) then we wouldn't have to do real binary
-- searches for later attributes from that same tuple.
--
-- Reusing the bounds from earlier lower-order attributes will always be
-- impossible, because (barring the odd hash collision) no hash1/most
-- significant column value will be repeated.
--
-- XXX As with many of my stress tests targeting this same patch, this
-- stress-test query effectively degenerates into a full index scan (it scans
-- every leaf page exactly once) with the patch applied.  This number of
-- leaf pages in the index becomes the "logical I/O worst case" for _every_
-- index scan with the patch.  So logical I/O (buffers hit/read/whatever) isn't
-- a cost we need to worry about here.

prepare nemesis_query as
select count(*)         -- count(*) matches our :array_size (index quals satisfied by every index tuple)
from nemesis
where
  hash1 = any (array[( select array_agg(hashint8(i + hashint4(1)::int8)) from generate_series(1, :array_size) i)])
  and
  hash2 = any (array[( select array_agg(hashint8(i + hashint4(2)::int8)) from generate_series(1, :array_size) i)])
  and
  hash3 = any (array[( select array_agg(hashint8(i + hashint4(3)::int8)) from generate_series(1, :array_size) i)])
  and
  hash4 = any (array[( select array_agg(hashint8(i + hashint4(4)::int8)) from generate_series(1, :array_size) i)])
  and
  hash5 = any (array[( select array_agg(hashint8(i + hashint4(5)::int8)) from generate_series(1, :array_size) i)])
  and
  hash6 = any (array[( select array_agg(hashint8(i + hashint4(6)::int8)) from generate_series(1, :array_size) i)])
  and
  hash7 = any (array[( select array_agg(hashint8(i + hashint4(7)::int8)) from generate_series(1, :array_size) i)])
  and
  hash8 = any (array[( select array_agg(hashint8(i + hashint4(8)::int8)) from generate_series(1, :array_size) i)])
  and
  hash9 = any (array[( select array_agg(hashint8(i + hashint4(9)::int8)) from generate_series(1, :array_size) i)])
  and
  hash10 = any (array[( select array_agg(hashint8(i + hashint4(10)::int8)) from generate_series(1, :array_size) i)])
  and
  hash11 = any (array[( select array_agg(hashint8(i + hashint4(11)::int8)) from generate_series(1, :array_size) i)])
  and
  hash12 = any (array[( select array_agg(hashint8(i + hashint4(12)::int8)) from generate_series(1, :array_size) i)])
  and
  hash13 = any (array[( select array_agg(hashint8(i + hashint4(13)::int8)) from generate_series(1, :array_size) i)])
  and
  hash14 = any (array[( select array_agg(hashint8(i + hashint4(14)::int8)) from generate_series(1, :array_size) i)])
  and
  hash15 = any (array[( select array_agg(hashint8(i + hashint4(15)::int8)) from generate_series(1, :array_size) i)])
  and
  hash16 = any (array[( select array_agg(hashint8(i + hashint4(16)::int8)) from generate_series(1, :array_size) i)])
  and
  hash17 = any (array[( select array_agg(hashint8(i + hashint4(17)::int8)) from generate_series(1, :array_size) i)])
  and
  hash18 = any (array[( select array_agg(hashint8(i + hashint4(18)::int8)) from generate_series(1, :array_size) i)])
  and
  hash19 = any (array[( select array_agg(hashint8(i + hashint4(19)::int8)) from generate_series(1, :array_size) i)])
  and
  hash20 = any (array[( select array_agg(hashint8(i + hashint4(20)::int8)) from generate_series(1, :array_size) i)])
  and
  hash21 = any (array[( select array_agg(hashint8(i + hashint4(21)::int8)) from generate_series(1, :array_size) i)])
  and
  hash22 = any (array[( select array_agg(hashint8(i + hashint4(22)::int8)) from generate_series(1, :array_size) i)])
  and
  hash23 = any (array[( select array_agg(hashint8(i + hashint4(23)::int8)) from generate_series(1, :array_size) i)])
  and
  hash24 = any (array[( select array_agg(hashint8(i + hashint4(24)::int8)) from generate_series(1, :array_size) i)])
  and
  hash25 = any (array[( select array_agg(hashint8(i + hashint4(25)::int8)) from generate_series(1, :array_size) i)])
  and
  hash26 = any (array[( select array_agg(hashint8(i + hashint4(26)::int8)) from generate_series(1, :array_size) i)])
  and
  hash27 = any (array[( select array_agg(hashint8(i + hashint4(27)::int8)) from generate_series(1, :array_size) i)])
  and
  hash28 = any (array[( select array_agg(hashint8(i + hashint4(28)::int8)) from generate_series(1, :array_size) i)])
  and
  hash29 = any (array[( select array_agg(hashint8(i + hashint4(29)::int8)) from generate_series(1, :array_size) i)])
  and
  hash30 = any (array[( select array_agg(hashint8(i + hashint4(30)::int8)) from generate_series(1, :array_size) i)])
  and
  hash31 = any (array[( select array_agg(hashint8(i + hashint4(31)::int8)) from generate_series(1, :array_size) i)])
  and
  hash32 = any (array[( select array_agg(hashint8(i + hashint4(32)::int8)) from generate_series(1, :array_size) i)])
  ;

-- Now run the query:
execute nemesis_query;

-- Using custom instrumentation to measure how much time is spent in
-- _bt_readpage (and determining our exposure to buffer-lock-held-too-long
-- problems) is left as an exercise for the reader
