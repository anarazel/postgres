/* contrib/pg_buffercache/pg_buffercache--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_bufferache" to load this file. \quit

CREATE FUNCTION pg_buffercache_stats(
    OUT valid_count bigint,
    OUT dirty_count bigint,
    OUT unused_count bigint,
    OUT io_in_progress_count bigint,
    OUT partially_valid_count bigint,
    OUT header_locked_count bigint
    )
RETURNS record
AS 'MODULE_PATHNAME', 'pg_buffercache_stats'
LANGUAGE C STRICT;
