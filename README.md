[![GitHub release](https://img.shields.io/github/v/release/postgrespro/ptrack?include_prereleases)](https://github.com/postgrespro/ptrack/releases/latest)

## PTRACK 

## Overview 

PTRACK allows speed up incremental backups for the huge PostgreSQL databases. PTRACK store changes of physical blocks in the memory. You can [effectively use](https://postgrespro.github.io/pg_probackup/#pbk-setting-up-ptrack-backups) `PTRACK` engine for taking incremental backups by [pg_probackup](https://github.com/postgrespro/pg_probackup).

Current patch are available for PostgreSQL [11](https://github.com/postgrespro/ptrack/blob/master/patches/REL_11_STABLE-ptrack-core.diff), [12](https://github.com/postgrespro/ptrack/blob/master/patches/REL_12_STABLE-ptrack-core.diff), [13](https://github.com/postgrespro/ptrack/blob/master/patches/REL_13_STABLE-ptrack-core.diff), [14](https://github.com/postgrespro/ptrack/blob/master/patches/REL_14_STABLE-ptrack-core.diff), [15](https://github.com/postgrespro/ptrack/blob/master/patches/REL_15_STABLE-ptrack-core.diff)

## Enterprise edition

Enterprise PTRACK are part of [Postgres Pro Enterprise](https://postgrespro.ru/products/postgrespro/enterprise) and offers the capability to track more than 100 000 tables and indexes concurrently without any degradation in speed [CFS (compressed file system)](https://postgrespro.ru/docs/enterprise/current/cfs).
According to benchmarks, it operated up to 5 times faster and useful for ERP and DWH with huge amount of tables and relations between them.


## Installation

1) Get latest `PTRACK` sources:

```shell
git clone https://github.com/postgrespro/ptrack.git
```

2) Get latest PostgreSQL sources:

```shell
git clone https://github.com/postgres/postgres.git -b REL_14_STABLE && cd postgres
```

3) Apply PostgreSQL core patch:

```shell
git apply -3 ../ptrack/patches/REL_14_STABLE-ptrack-core.diff
```

4) Compile and install PostgreSQL

5) Set `ptrack.map_size` (in MB)

```shell
echo "shared_preload_libraries = 'ptrack'" >> postgres_data/postgresql.conf
echo "ptrack.map_size = 64" >> postgres_data/postgresql.conf
```

6) Compile and install `PTRACK` extension

```shell
USE_PGXS=1 make -C /path/to/ptrack/ install
```

7) Run PostgreSQL and create `PTRACK` extension

```sql
postgres=# CREATE EXTENSION ptrack;
```

## Configuration

The only one configurable option is `ptrack.map_size` (in MB). Default is `0`, which means `PTRACK` is turned off. In order to reduce number of false positives it is recommended to set `ptrack.map_size` to `1 / 1000` of expected `PGDATA` size (i.e. `1000` for a 1 TB database).

To disable `PTRACK` and clean up all remaining service files set `ptrack.map_size` to `0`.

## Public SQL API

 * ptrack_version() — returns ptrack version string.
 * ptrack_init_lsn() — returns LSN of the last ptrack map initialization.
 * ptrack_get_pagemapset(start_lsn pg_lsn) — returns a set of changed data files with a number of changed blocks and their bitmaps since specified `start_lsn`.
 * ptrack_get_change_stat(start_lsn pg_lsn) — returns statistic of changes (number of files, pages and size in MB) since specified `start_lsn`.

Usage example:

```sql
postgres=# SELECT ptrack_version();
 ptrack_version 
----------------
 2.4
(1 row)

postgres=# SELECT ptrack_init_lsn();
 ptrack_init_lsn 
-----------------
 0/1814408
(1 row)

postgres=# SELECT * FROM ptrack_get_pagemapset('0/185C8C0');
        path         | pagecount |                pagemap                 
---------------------+-----------+----------------------------------------
 base/16384/1255     |         3 | \x001000000005000000000000
 base/16384/2674     |         3 | \x0000000900010000000000000000
 base/16384/2691     |         1 | \x00004000000000000000000000
 base/16384/2608     |         1 | \x000000000000000400000000000000000000
 base/16384/2690     |         1 | \x000400000000000000000000
(5 rows)

postgres=# SELECT * FROM ptrack_get_change_stat('0/285C8C8');
 files | pages |        size, MB        
-------+-------+------------------------
    20 |    25 | 0.19531250000000000000
(1 row)
```

## Upgrading

Usually, you have to only install new version of `PTRACK` and do `ALTER EXTENSION 'ptrack' UPDATE;`. However, some specific actions may be required as well:

#### Upgrading from 2.0.0 to 2.1.*:

* Put `shared_preload_libraries = 'ptrack'` into `postgresql.conf`.
* Rename `ptrack_map_size` to `ptrack.map_size`.
* Do `ALTER EXTENSION 'ptrack' UPDATE;`.
* Restart your server.

#### Upgrading from 2.1.* to 2.2.*:

Since version 2.2 we use a different algorithm for tracking changed pages. Thus, data recorded in the `ptrack.map` using pre 2.2 versions of `PTRACK` is incompatible with newer versions. After extension upgrade and server restart old `ptrack.map` will be discarded with `WARNING` and initialized from the scratch.

#### Upgrading from 2.2.* to 2.3.*:

* Stop your server
* Update ptrack binaries
* Remove global/ptrack.map.mmap if it exist in server data directory
* Start server
* Do `ALTER EXTENSION 'ptrack' UPDATE;`.

#### Upgrading from 2.3.* to 2.4.*:

* Stop your server
* Update `PTRACK` binaries
* Start server
* Do `ALTER EXTENSION 'ptrack' UPDATE;`.

## Limitations

1. You can only use `PTRACK` safely with `wal_level >= 'replica'`. Otherwise, you can lose tracking of some changes if crash-recovery occurs, since [certain commands are designed not to write WAL at all if wal_level is minimal](https://www.postgresql.org/docs/12/populate.html#POPULATE-PITR), but we only durably flush `PTRACK` map at checkpoint time.

2. The only one production-ready backup utility, that fully supports `PTRACK` is [pg_probackup](https://github.com/postgrespro/pg_probackup).

3. You cannot resize `PTRACK` map in runtime, only on postmaster start. Also, you will lose all tracked changes, so it is recommended to do so in the maintainance window and accompany this operation with full backup.

4. You will need up to `ptrack.map_size * 2` of additional disk space, since `PTRACK` uses additional temporary file for durability purpose. See [Architecture section](#Architecture) for details.

## Benchmarks

Briefly, an overhead of using `PTRACK` on TPS usually does not exceed a couple of percent (~1-3%) for a database of dozens to hundreds of gigabytes in size, while the backup time scales down linearly with backup size with a coefficient ~1. It means that an incremental `PTRACK` backup of a database with only 20% of changed pages will be 5 times faster than a full backup. More details [here](benchmarks).

## Architecture

It is designed to permit false positives (i.e., block/page is marked as altered in the `PTRACK` map when it hasn't actually been changed), but it never tolerates false negatives (i.e., it never loses any PGDATA modifications, barring hint-bits).

At present, the PTRACK codebase is divided between a small PostgreSQL core patch and an extension. The public SQL API methods and the main engine are housed in the `PTRACK` extension, whereas the core patch only includes specific hooks and modifies binary utilities to disregard ptrack.map.* files.

In `PTRACK`, we use a single shared hash table. Due to the fixed size of the map, there can be false positives (when a block is marked as changed without actual modification), but false negatives are not allowed. Nevertheless, these false positives can be completely removed by setting a sufficiently high ptrack.map_size.

All reads/writes are performed using atomic operations on uint64 entries, making the map completely lockless during standard PostgreSQL operation. Since we do not utilize locks for read/write access, `PTRACK` maintains a map (ptrack.map) from the last checkpoint unaltered and uses a maximum of one additional temporary file.

* temporary file `ptrack.map.tmp` to durably replace `ptrack.map` during checkpoint.

Map is written on disk at the end of checkpoint atomically block by block involving the CRC32 checksum calculation that is checked on the next whole map re-read after crash-recovery or restart.

To gather the whole changeset of modified blocks in `ptrack_get_pagemapset()` we walk the entire `PGDATA` (`base/**/*`, `global/*`, `pg_tblspc/**/*`) and verify using map whether each block of each relation was modified since the specified LSN or not.

## Contribution

Feel free to [send pull requests](https://github.com/postgrespro/ptrack/compare), [fill up issues](https://github.com/postgrespro/ptrack/issues/new).
See also the list of [authors](AUTHORS.md) who participated in this project.

### Tests

Everything is tested automatically with [travis-ci.com](https://travis-ci.com/postgrespro/ptrack) and [codecov.io](https://codecov.io/gh/postgrespro/ptrack), but you can also run tests locally via `Docker`:

```sh
export PG_BRANCH=REL_14_STABLE
export TEST_CASE=all
export MODE=paranoia

./make_dockerfile.sh

docker-compose build
docker-compose run tests
```

Available test modes (`MODE`) are `basic` (default) and `paranoia` (per-block checksum comparison of `PGDATA` content before and after backup-restore process). Available test cases (`TEST_CASE`) are `tap` (minimalistic PostgreSQL [tap test](https://github.com/postgrespro/ptrack/blob/master/t/001_basic.pl)), `all` or any specific [pg_probackup test](https://github.com/postgrespro/pg_probackup/blob/master/tests/ptrack.py), e.g. `test_ptrack_simple`.

## Acknowledgments

PTRACK development is supported by Postgres Professional
You can ask any question about contributing or usage in [Russian chat](https://t.me/pg_probackup) or [Internation chat](https://t.me/pg_probackup_eng) of [pg_probackup](https://github.com/postgrespro/pg_probackup). 

