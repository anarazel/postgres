--
-- FILENODE_MAPPING
-- Check that relation OIDs map to filenodes and back for every relation,
-- after everything the other tests did to them.
--

-- Check that we map relation oids to filenodes and back correctly.  Only
-- display bad mappings so the test output doesn't change all the time.  A
-- filenode function call can return NULL for a relation dropped concurrently
-- with the call's surrounding query, so ignore a NULL mapped_oid for
-- relations that no longer exist after all calls finish.
-- Temporary relations are ignored, as not supported by pg_filenode_relation().
CREATE TEMP TABLE filenode_mapping AS
SELECT
    oid, mapped_oid, reltablespace, relfilenode, relname
FROM pg_class,
    pg_filenode_relation(reltablespace, pg_relation_filenode(oid)) AS mapped_oid
WHERE relkind IN ('r', 'i', 'S', 't', 'm')
  AND relpersistence != 't'
  AND mapped_oid IS DISTINCT FROM oid;
SELECT m.* FROM filenode_mapping m LEFT JOIN pg_class c ON c.oid = m.oid
WHERE c.oid IS NOT NULL OR m.mapped_oid IS NOT NULL;
