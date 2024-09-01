/* src/test/modules/test_aio/test_aio--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_aio" to load this file. \quit


CREATE FUNCTION grow_rel(rel regclass, nblocks int)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;


CREATE FUNCTION corrupt_rel_block(rel regclass, blockno int)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION read_corrupt_rel_block(rel regclass, blockno int)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION invalidate_rel_block(rel regclass, blockno int)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION handle_get_and_error()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION handle_get_twice()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION handle_get()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION handle_get_release()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION handle_release_last()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;


CREATE FUNCTION bb_get_and_error()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION bb_get_twice()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION bb_get()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION bb_get_release()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION bb_release_last()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;


CREATE OR REPLACE FUNCTION redact(p_sql text)
RETURNS bool
LANGUAGE plpgsql
AS $$
    DECLARE
	err_state text;
        err_msg text;
    BEGIN
        EXECUTE p_sql;
	RETURN true;
    EXCEPTION WHEN OTHERS THEN
        GET STACKED DIAGNOSTICS
	    err_state = RETURNED_SQLSTATE,
	    err_msg = MESSAGE_TEXT;
	err_msg = regexp_replace(err_msg, '(file|relation) "?base/[0-9]+/[0-9]+"?', '\1 base/<redacted>');
        RAISE NOTICE 'wrapped error: %', err_msg
	    USING ERRCODE = err_state;
	RETURN false;
    END;
$$;


CREATE FUNCTION inj_io_short_read_attach(result int)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION inj_io_short_read_detach()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
