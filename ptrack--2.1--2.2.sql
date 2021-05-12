/* ptrack/ptrack--2.1--2.2.sql */

-- Complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION ptrack UPDATE;" to load this file.\ quit

CREATE FUNCTION ptrack_get_change_stat(start_lsn pg_lsn)
	RETURNS TABLE (
		files bigint,
		pages bigint,
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
		   block_size*changed_pages/(1024.0*1024)
	FROM
		(SELECT count(path) AS changed_files,
				sum(
					length(replace(right((pagemap)::text, -1)::varbit::text, '0', ''))
				) AS changed_pages
		 FROM ptrack_get_pagemapset(start_lsn)) s;
END
$func$ LANGUAGE plpgsql;

CREATE FUNCTION ptrack_get_change_file_stat(start_lsn pg_lsn)
	RETURNS TABLE (
		file_path text,
		pages int,
		"size, MB" numeric
	) AS
$func$
DECLARE
block_size bigint;
BEGIN
	block_size := (SELECT setting FROM pg_settings WHERE name = 'block_size');

	RETURN QUERY
	SELECT s.path,
		   changed_pages,
		   block_size*changed_pages/(1024.0*1024)
	FROM
		(SELECT path,
				length(replace(right((pagemap)::text, -1)::varbit::text, '0', ''))
					AS changed_pages
		 FROM ptrack_get_pagemapset(start_lsn)) s
	ORDER BY (changed_pages, s.path) DESC;
END
$func$ LANGUAGE plpgsql;
