\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.5'" to load this file. \quit

CREATE FUNCTION pg_buffercache_invalidate(IN int, IN bool default true)
RETURNS bool
AS 'MODULE_PATHNAME', 'pg_buffercache_invalidate'
LANGUAGE C PARALLEL SAFE;
