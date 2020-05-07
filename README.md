[![Build Status](https://travis-ci.com/postgrespro/ptrack.svg?branch=master)](https://travis-ci.com/postgrespro/ptrack)
![GitHub tag (latest SemVer)](https://img.shields.io/github/v/tag/postgrespro/ptrack?label=release)

# ptrack

## Overview

Ptrack is a fast block-level incremental backup engine for PostgreSQL. Currently `ptrack` codebase is split between small PostgreSQL core patch and extension. All public SQL API methods and main engine are placed in the `ptrack` extension, while the core patch contains only certain hooks and modifies binary utilities to ignore `ptrack.map.*` files.

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

## Public SQL API

 * ptrack_version() — returns ptrack version string (2.0 currently).
 * ptrack_get_pagemapset('LSN') — returns a set of changed data files with bitmaps of changed blocks since specified LSN.
 * ptrack_init_lsn() — returns LSN of the last ptrack map initialization.

## Architecture

TBA

## Roadmap

The main goal currently is to move as much `ptrack` functionality into the extension as possible and leave only certain requred hooks as core patch.
