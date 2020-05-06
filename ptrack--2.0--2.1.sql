/* ptrack/ptrack--2.0--2.1.sql */

-- Complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION ptrack UPDATE;" to load this file. \quit

DROP FUNCTION ptrack_version();
DROP FUNCTION pg_ptrack_get_pagemapset(pg_lsn);
DROP FUNCTION pg_ptrack_control_lsn();
DROP FUNCTION pg_ptrack_get_block(oid, oid, oid, int8);

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
