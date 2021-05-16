/* ptrack/ptrack--2.1--2.2.sql */

-- Complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION ptrack UPDATE;" to load this file.\ quit

DROP FUNCTION ptrack_get_pagemapset(start_lsn pg_lsn);
CREATE FUNCTION ptrack_get_pagemapset(start_lsn pg_lsn)
RETURNS TABLE (path			text,
			   pagecount	bigint,
			   pagemap		bytea)
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION ptrack_get_change_stat(start_lsn pg_lsn)
	RETURNS TABLE (
		files bigint,
		pages numeric,
		"size, MB" numeric
	) AS
$func$
DECLARE
block_size bigint;
BEGIN
	block_size := (SELECT setting FROM pg_settings WHERE name = 'block_size');

	RETURN QUERY
	SELECT changed_files,
		   changed_pages,
		   block_size * changed_pages / (1024.0 * 1024)
	FROM
		(SELECT count(path) AS changed_files,
				sum(pagecount) AS changed_pages
		 FROM ptrack_get_pagemapset(start_lsn)) s;
END
$func$ LANGUAGE plpgsql;
