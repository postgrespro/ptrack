-- Complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION ptrack" to load this file. \quit

CREATE FUNCTION ptrack_version()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION pg_ptrack_control_lsn()
RETURNS pg_lsn
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION pg_ptrack_get_pagemapset(start_lsn pg_lsn)
RETURNS TABLE (path		text,
			   pagemap	bytea)
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION pg_ptrack_get_block(tablespace_oid	oid,
									db_oid			oid,
									relfilenode		oid,
									blockno			int8)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
