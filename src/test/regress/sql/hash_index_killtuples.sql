-- Basic test of gist's killtuples / pruning during split implementation. This
-- is not guaranteed to reach those paths, concurrent activity could prevent
-- cleanup of dead tuples or such. But it's likely to reach them, which seems
-- better than nothing.
set enable_seqscan=off;
set enable_bitmapscan=off;
set enable_material=off;

-- Hash indexes are fairly dense, 1000 tuples isn't enough to easily reach the
-- killtuples logic. Might need to be adjusted upward for larger page sizes.
\set high 2000

CREATE TABLE test_hash_killtuples (
	k int8,
	v int DEFAULT 0
);

CREATE INDEX ON test_hash_killtuples USING hash(k);

-- create index entries pointing to deleted tuples
INSERT INTO test_hash_killtuples(k, v) SELECT generate_series(1, :high), 0;
-- via deletes
DELETE FROM test_hash_killtuples WHERE k < (:high / 2);
-- and updates
UPDATE test_hash_killtuples SET k = k + 1, v = 1 WHERE k > (:high / 2);

---- make the index see tuples are dead via killtuples
PREPARE engage_killtuples AS
SELECT count(*) FROM generate_series(1, :high + 1) g(i) WHERE EXISTS (SELECT * FROM test_hash_killtuples thk WHERE thk.k = g.i);
EXPLAIN (COSTS OFF) EXECUTE engage_killtuples;
EXECUTE engage_killtuples;

-- create new index entries, in the same ranges, this should see the dead entries and prune the pages
INSERT INTO test_hash_killtuples(k, v) SELECT generate_series(1, :high * 0.3), 2;
UPDATE test_hash_killtuples SET k = k + 1, v = 3 WHERE k > (:high * 0.6);

-- do some basic cross checking of index scans vs sequential scan
-- use prepared statement, so we can explain and execute without repeating the statement
PREPARE check_query AS SELECT thk.v, min(thk.k), max(thk.k), count(*),
  brute = ROW(thk.v, min(thk.k), max(thk.k), count(*)) AS are_equal
FROM (
    SELECT v, min(k), max(k), count(*)
    FROM test_hash_killtuples GROUP BY v ORDER BY v
  ) brute,
  -- hash only does equality lookups, so simulate a range scan using generate_series
  generate_series(brute.min, brute.max) g(i),
  test_hash_killtuples thk
WHERE
  thk.k = g.i
GROUP BY brute, thk.v
ORDER BY thk.v;

EXPLAIN (COSTS OFF) EXECUTE check_query;
EXECUTE check_query;

-- Leave the table/index around, so that an index that has been affected by
-- killtuples could be seen by amcheck or such.
