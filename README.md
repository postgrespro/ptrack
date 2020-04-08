# ptrack

## Overview

Ptrack is a fast block-level incremental backup engine for PostgreSQL. Currently `ptrack` codebase is split approximately 50/50% between PostgreSQL core patch and extension. All public SQL API methods are placed in the `ptrack` extension, while the main engine is still in core.

## Installation

1) Get latest PostgreSQL sources:

```shell
git clone https://github.com/postgres/postgres.git -b REL_12_STABLE && cd postgres
```

2) Apply PostgreSQL core patch:

```shell
git apply ptrack/patches/ptrack-2.0-core.diff
```

3) Compile and install PostgreSQL

4) Set `ptrack_map_size` (in MB)

```shell
echo 'ptrack_map_size = 64' >> postgres_data/postgresql.conf
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

 * ptrack_version() --- returns ptrack version string (2.0 currently).
 * pg_ptrack_get_pagemapset('LSN') --- returns a set of changed data files with bitmaps of changed blocks since specified LSN.
 * pg_ptrack_control_lsn() --- returns LSN of the last ptrack map initialization.
 * pg_ptrack_get_block --- returns a requested block of relation.

## Architecture

TBA

## Roadmap

The main goal currently is to move as much `ptrack` functionality into the extension as possible and leave only certain requred hooks as core patch.
