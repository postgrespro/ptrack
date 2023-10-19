[![Build Status](https://travis-ci.com/postgrespro/ptrack.svg?branch=master)](https://travis-ci.com/postgrespro/ptrack)
[![codecov](https://codecov.io/gh/postgrespro/ptrack/branch/master/graph/badge.svg)](https://codecov.io/gh/postgrespro/ptrack)
[![GitHub release](https://img.shields.io/github/v/release/postgrespro/ptrack?include_prereleases)](https://github.com/postgrespro/ptrack/releases/latest)

# ptrack

## Overview

Ptrack is a block-level incremental backup engine for PostgreSQL. You can [effectively use](https://postgrespro.github.io/pg_probackup/#pbk-setting-up-ptrack-backups) `ptrack` engine for taking incremental backups with [pg_probackup](https://github.com/postgrespro/pg_probackup) backup and recovery manager for PostgreSQL.

It is designed to allow false positives (i.e. block/page is marked in the `ptrack` map, but actually has not been changed), but to never allow false negatives (i.e. loosing any `PGDATA` changes, excepting hint-bits).

Currently, `ptrack` codebase is split between small PostgreSQL core patch and extension. All public SQL API methods and main engine are placed in the `ptrack` extension, while the core patch contains only certain hooks and modifies binary utilities to ignore `ptrack.map.*` files.

This extension is compatible with PostgreSQL [11](https://github.com/postgrespro/ptrack/blob/master/patches/REL_11_STABLE-ptrack-core.diff), [12](https://github.com/postgrespro/ptrack/blob/master/patches/REL_12_STABLE-ptrack-core.diff), [13](https://github.com/postgrespro/ptrack/blob/master/patches/REL_13_STABLE-ptrack-core.diff), [14](https://github.com/postgrespro/ptrack/blob/master/patches/REL_14_STABLE-ptrack-core.diff).

## Installation

1) Get latest `ptrack` sources:

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

6) Compile and install `ptrack` extension

```shell
USE_PGXS=1 make -C /path/to/ptrack/ install
```

7) Run PostgreSQL and create `ptrack` extension

```sql
postgres=# CREATE EXTENSION ptrack;
```

## Configuration

The only one configurable option is `ptrack.map_size` (in MB). Default is `0`, which means `ptrack` is turned off. In order to reduce number of false positives it is recommended to set `ptrack.map_size` to `1 / 1000` of expected `PGDATA` size (i.e. `1000` for a 1 TB database).

To disable `ptrack` and clean up all remaining service files set `ptrack.map_size` to `0`.

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

Usually, you have to only install new version of `ptrack` and do `ALTER EXTENSION ptrack UPDATE;`. However, some specific actions may be required as well:

#### Upgrading from 2.0.0 to 2.1.*:

* Put `shared_preload_libraries = 'ptrack'` into `postgresql.conf`.
* Rename `ptrack_map_size` to `ptrack.map_size`.
* Do `ALTER EXTENSION ptrack UPDATE;`.
* Restart your server.

#### Upgrading from 2.1.* to 2.2.*:

Since version 2.2 we use a different algorithm for tracking changed pages. Thus, data recorded in the `ptrack.map` using pre 2.2 versions of `ptrack` is incompatible with newer versions. After extension upgrade and server restart old `ptrack.map` will be discarded with `WARNING` and initialized from the scratch.

#### Upgrading from 2.2.* to 2.3.*:

* Stop your server
* Update ptrack binaries
* Remove global/ptrack.map.mmap if it exist in server data directory
* Start server
* Do `ALTER EXTENSION ptrack UPDATE;`.

#### Upgrading from 2.3.* to 2.4.*:

* Stop your server
* Update ptrack binaries
* Start server
* Do `ALTER EXTENSION ptrack UPDATE;`.

## Limitations

1. You can only use `ptrack` safely with `wal_level >= 'replica'`. Otherwise, you can lose tracking of some changes if crash-recovery occurs, since [certain commands are designed not to write WAL at all if wal_level is minimal](https://www.postgresql.org/docs/12/populate.html#POPULATE-PITR), but we only durably flush `ptrack` map at checkpoint time.

2. The only one production-ready backup utility, that fully supports `ptrack` is [pg_probackup](https://github.com/postgrespro/pg_probackup).

3. You cannot resize `ptrack` map in runtime, only on postmaster start. Also, you will loose all tracked changes, so it is recommended to do so in the maintainance window and accompany this operation with full backup.

4. You will need up to `ptrack.map_size * 2` of additional disk space, since `ptrack` uses additional temporary file for durability purpose. See [Architecture section](#Architecture) for details.

## Benchmarks

Briefly, an overhead of using `ptrack` on TPS usually does not exceed a couple of percent (~1-3%) for a database of dozens to hundreds of gigabytes in size, while the backup time scales down linearly with backup size with a coefficient ~1. It means that an incremental `ptrack` backup of a database with only 20% of changed pages will be 5 times faster than a full backup. More details [here](benchmarks).

## Architecture

We use a single shared hash table in `ptrack`. Due to the fixed size of the map there may be false positives (when some block is marked as changed without being actually modified), but not false negative results. However, these false postives may be completely eliminated by setting a high enough `ptrack.map_size`.

All reads/writes are made using atomic operations on `uint64` entries, so the map is completely lockless during the normal PostgreSQL operation. Because we do not use locks for read/write access, `ptrack` keeps a map (`ptrack.map`) since the last checkpoint intact and uses up to 1 additional temporary file:

* temporary file `ptrack.map.tmp` to durably replace `ptrack.map` during checkpoint.

Map is written on disk at the end of checkpoint atomically block by block involving the CRC32 checksum calculation that is checked on the next whole map re-read after crash-recovery or restart.

To gather the whole changeset of modified blocks in `ptrack_get_pagemapset()` we walk the entire `PGDATA` (`base/**/*`, `global/*`, `pg_tblspc/**/*`) and verify using map whether each block of each relation was modified since the specified LSN or not.

## Contribution

Feel free to [send pull requests](https://github.com/postgrespro/ptrack/compare), [fill up issues](https://github.com/postgrespro/ptrack/issues/new), or just reach one of us directly (e.g. <[Alexey Kondratov](mailto:a.kondratov@postgrespro.ru?subject=[GitHub]%20Ptrack), [@ololobus](https://github.com/ololobus)>) if you are interested in `ptrack`.

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

### TODO

* Should we introduce `ptrack.map_path` to allow `ptrack` service files storage outside of `PGDATA`? Doing that we will avoid patching PostgreSQL binary utilities to ignore `ptrack.map.*` files.
* Can we resize `ptrack` map on restart but keep the previously tracked changes?
* Can we write a formal proof, that we never loose any modified page with `ptrack`? With TLA+?
