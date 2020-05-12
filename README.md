[![Build Status](https://travis-ci.com/postgrespro/ptrack.svg?branch=master)](https://travis-ci.com/postgrespro/ptrack)
[![codecov](https://codecov.io/gh/postgrespro/ptrack/branch/master/graph/badge.svg)](https://codecov.io/gh/postgrespro/ptrack)
[![GitHub release](https://img.shields.io/github/v/release/postgrespro/ptrack?include_prereleases)](https://github.com/postgrespro/ptrack/releases/latest)

# ptrack

## Overview

Ptrack is a block-level incremental backup engine for PostgreSQL. You can [effectively use](https://postgrespro.github.io/pg_probackup/#pbk-setting-up-ptrack-backups) `ptrack` engine for taking incremental backups with [pg_probackup](https://github.com/postgrespro/pg_probackup) backup and recovery manager for PostgreSQL.

It is designed to allow false positives (i.e. block/page is marked in the `ptrack` map, but actually has not been changed), but to never allow false negatives (i.e. loosing any `PGDATA` changes, excepting hint-bits).

Currently, `ptrack` codebase is split between small PostgreSQL core patch and extension. All public SQL API methods and main engine are placed in the `ptrack` extension, while the core patch contains only certain hooks and modifies binary utilities to ignore `ptrack.map.*` files.

## Installation

1) Get latest PostgreSQL sources:

```shell
git clone https://github.com/postgres/postgres.git -b REL_12_STABLE && cd postgres
```

2) Apply PostgreSQL core patch:

```shell
git apply -3 ptrack/patches/REL_12_STABLE-ptrack-core.diff
```

3) Compile and install PostgreSQL

4) Set `ptrack.map_size` (in MB)

```shell
echo "shared_preload_libraries = 'ptrack'" >> postgres_data/postgresql.conf
echo "ptrack.map_size = 64" >> postgres_data/postgresql.conf
```

5) Compile and install `ptrack` extension

```shell
USE_PGXS=1 make -C /path/to/ptrack/ install
```

6) Run PostgreSQL and create `ptrack` extension

```sql
CREATE EXTENSION ptrack;
```

## Configuration

The only one configurable option is `ptrack.map_size` (in MB). Default is `-1`, which means `ptrack` is turned off. To completely avoid false positives it is recommended to set `ptrack.map_size` to `1 / 1000` of expected `PGDATA` size (i.e. `1000` for a 1 TB database), since a single 8 byte `ptrack` map record tracks changes in a standard 8 KB PostgreSQL page. To disable `ptrack` and clean up all remaining service files set `ptrack.map_size` to `0`.

## Public SQL API

 * ptrack_version() — returns ptrack version string.
 * ptrack_init_lsn() — returns LSN of the last ptrack map initialization.
 * ptrack_get_pagemapset('LSN') — returns a set of changed data files with bitmaps of changed blocks since specified LSN.

Usage example:

```sql
postgres=# SELECT ptrack_version();
 ptrack_version 
----------------
 2.1
(1 row)

postgres=# SELECT ptrack_init_lsn();
 ptrack_init_lsn 
-----------------
 0/1814408
(1 row)

postgres=# SELECT ptrack_get_pagemapset('0/186F4C8');
           ptrack_get_pagemapset           
-------------------------------------------
 (global/1262,"\\x0100000000000000000000")
 (global/2672,"\\x0200000000000000000000")
 (global/2671,"\\x0200000000000000000000")
(3 rows)
```

## Limitations

1. You can only use `ptrack` safely with `wal_level >= 'replica'`. Otherwise, you can lose tracking of some changes if crash-recovery occurs, since [certain commands are designed not to write WAL at all if wal_level is minimal](https://www.postgresql.org/docs/12/populate.html#POPULATE-PITR), but we only durably flush `ptrack` map at checkpoint time.

2. The only one production-ready backup utility, that fully supports `ptrack` is [pg_probackup](https://github.com/postgrespro/pg_probackup).

3. Currently, you cannot resize `ptrack` map in runtime, only on postmaster restart. Also, you will loose all tracked changes, so it is recommended to do so in the maintainance window and accompany this operation with full backup. See [TODO](#TODO) for details.

4. You will need up to `ptrack.map_size * 3` of additional disk space, since `ptrack` uses two additional temporary files for durability purpose. See [TODO](#Architecture) for details.

## Architecture

We use a single shared hash table in `ptrack`, which is mapped in memory from the file on disk using `mmap`. Due to the fixed size of the map there may be false positives (when some block is marked as changed without being actually modified), but not false negative results. However, these false postives may be completely eliminated by setting a high enough `ptrack.map_size`.

All reads/writes are made using atomic operations on `uint64` entries, so the map is completely lockless during the normal PostgreSQL operation. Because we do not use locks for read/write access and cannot control `mmap` eviction back to disk, `ptrack` keeps a map (`ptrack.map`) since the last checkpoint intact and uses up to 2 additional temporary files:

* working copy `ptrack.map.mmap` for doing `mmap` on it (there is a [TODO](#TODO) item);
* temporary file `ptrack.map.tmp` to durably replace `ptrack.map` during checkpoint.

Map is written on disk at the end of checkpoint atomically block by block involving the CRC32 checksum calculation that is checked on the next whole map re-read after crash-recovery or restart.

To gather the whole changeset of modified blocks in `ptrack_get_pagemapset()` we walk the entire `PGDATA` (`base/**/*`, `global/*`, `pg_tblspc/**/*`) and verify using map whether each block of each relation was modified since the specified LSN or not.

## Contribution

Feel free to [send pull requests](https://github.com/postgrespro/ptrack/compare), [fill up issues](https://github.com/postgrespro/ptrack/issues/new), or just reach one of us directly (e.g. <[Alexey Kondratov](mailto:a.kondratov@postgrespro.ru?subject=[GitHub]%20Ptrack), [@ololobus](https://github.com/ololobus)>) if you are interested in `ptrack`.

### Tests

Everything is tested automatically with [travis-ci.com](https://travis-ci.com/postgrespro/ptrack) and [codecov.io](https://codecov.io/gh/postgrespro/ptrack), but you can also run tests locally via `Docker`:

```sh
export PG_VERSION=12
export PG_BRANCH=REL_12_STABLE
export TEST_CASE=all
export MODE=paranoia

docker-compose build
docker-compose run tests
```

Available test modes (`MODE`) are `basic` (default) and `paranoia` (per-block checksum comparison of `PGDATA` content before and after backup-restore process). Available test cases (`TEST_CASE`) are `tap` (minimalistic PostgreSQL [tap test](https://github.com/postgrespro/ptrack/blob/master/t/001_basic.pl)), `all` or any specific [pg_probackup test](https://github.com/postgrespro/pg_probackup/blob/master/tests/ptrack.py), e.g. `test_ptrack_simple`.

### TODO

* Use POSIX `shm_open()` instead of `open()` to do not create an additional working copy of `ptrack` map file.
* Should we introduce `ptrack.map_path` to allow `ptrack` service files storage outside of `PGDATA`? Doing that we will avoid patching PostgreSQL binary utilities to ignore `ptrack.map.*` files.
* Can we resize `ptrack` map on restart but keep the previously tracked changes?
* Can we resize `ptrack` map dynamicaly?
* Can we write a formal proof, that we never loose any modified page with `ptrack`? With TLA+?
