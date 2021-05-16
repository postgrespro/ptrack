/* ptrack/ptrack--2.1.sql */

-- Complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION ptrack" to load this file. \quit

CREATE FUNCTION ptrack_version()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION ptrack_init_lsn()
RETURNS pg_lsn
AS 'MODULE_PATHNAME'
LANGUAGE C VOLATILE;

CREATE FUNCTION ptrack_get_pagemapset(start_lsn pg_lsn)
RETURNS TABLE (path		text,
			   pagemap	bytea)
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
