-- Unique checking with and without deduplication, and LP_DEAD-bit-set tuple
-- deletion with posting list tuples.  The loop below runs in one transaction
-- for a while, which is why this lives outside the main regression tests:
-- there it would delay every concurrent CREATE INDEX CONCURRENTLY.

CREATE TABLE dedup_unique_test_table (a int) WITH (autovacuum_enabled=false);
CREATE UNIQUE INDEX dedup_unique ON dedup_unique_test_table (a) WITH (deduplicate_items=on);
CREATE UNIQUE INDEX plain_unique ON dedup_unique_test_table (a) WITH (deduplicate_items=off);
-- Generate enough garbage tuples in index to ensure that even the unique index
-- with deduplication enabled has to check multiple leaf pages during unique
-- checking (at least with a BLCKSZ of 8192 or less)
DO $$
BEGIN
    FOR r IN 1..1350 LOOP
        DELETE FROM dedup_unique_test_table;
        INSERT INTO dedup_unique_test_table SELECT 1;
    END LOOP;
END$$;

-- Exercise the LP_DEAD-bit-set tuple deletion code with a posting list tuple.
-- The implementation prefers deleting existing items to merging any duplicate
-- tuples into a posting list, so we need an explicit test to make sure we get
-- coverage (note that this test also assumes BLCKSZ is 8192 or less):
DROP INDEX plain_unique;
DELETE FROM dedup_unique_test_table WHERE a = 1;
INSERT INTO dedup_unique_test_table SELECT i FROM generate_series(0,450) i;

DROP TABLE dedup_unique_test_table;
