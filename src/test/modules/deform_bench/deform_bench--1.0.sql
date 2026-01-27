/* deform_bench--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION deform_bench" to load this file. \quit

CREATE FUNCTION deform_bench(tableoid Oid, attnum int[]) RETURNS FLOAT
AS 'MODULE_PATHNAME', 'deform_bench'
LANGUAGE C VOLATILE STRICT;
