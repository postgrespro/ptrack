# Ptrack benchmarks

## Runtime overhead

First target was to measure `ptrack` overhead on TPS due to marking modified pages in the map in memory. We used PostgreSQL 12 cluster of approximately 1 GB size, initialized with `pgbench` on a `tmpfs` partition:

```sh
pgbench -i -s 133
```

Default `pgbench` transaction script [were modified](pgb.sql) to exclude `pgbench_tellers` and `pgbench_branches` updates in order to lower lock contention and make `ptrack` overhead more visible. So `pgbench` was invoked as following:

```sh
pgbench -s133 -c40 -j1 -n -P15 -T300 -f pgb.sql
```

Results:

| ptrack.map_size, MB | 0 (turned off) | 32 | 64 | 256 | 512 | 1024 |
|---------------------|----------------|----|----|-----|-----|------|
| TPS | 16900 | 16890 | 16855 | 16468 | 16490 | 16220 |

TPS fluctuates in a several percent range around 16500 on the used machine, but in average `ptrack` overhead does not exceed 1-3% for any reasonable `ptrack.map_size`. It only becomes noticeable closer to 1 GB `ptrack.map_size` (~3-4%), which is enough to track changes in the database of up to 1 TB size without false positives.


<!-- ## Checkpoint overhead

Since `ptrack` map is completely flushed to disk during checkpoints, the same test were performed on HDD, but with slightly different configuration:
```conf
synchronous_commit = off
shared_buffers = 1GB
```
and `pg_prewarm` run prior to the test. -->

## Backups speedup

To test incremental backups speed a fresh cluster were initialized with following DDL:

```sql
CREATE TABLE large_test (num1 bigint, num2 double precision, num3 double precision);
CREATE TABLE large_test2 (num1 bigint, num2 double precision, num3 double precision);
```

These relations were populated with approximately 2 GB of data that way:

```sql
INSERT INTO large_test (num1, num2, num3)
SELECT s, random(), random()*142
FROM generate_series(1, 20000000) s;
```

Then a part of one relation was touched with a following query:

```sql
UPDATE large_test2 SET num3 = num3 + 1 WHERE num1 < 20000000 / 5;
```

After that, incremental `ptrack` backups were taken with `pg_probackup` followed by full backups. Tests show that `ptrack_backup_time / full_backup_time ~= ptrack_backup_size / full_backup_size`, i.e. if the only 20% of data were modified, then `ptrack` backup will be 5 times faster than full backup. Thus, the overhead of building `ptrack` map during backup is minimal. Example:

```log
21:02:43 postgres:~/dev/ptrack_test$ time pg_probackup backup -B $(pwd)/backup --instance=node -p5432 -b ptrack --no-sync --stream
INFO: Backup start, pg_probackup version: 2.3.1, instance: node, backup ID: QAA89O, backup mode: PTRACK, wal mode: STREAM, remote: false, compress-algorithm: none, compress-level: 1
INFO: Parent backup: QAA7FL
INFO: PGDATA size: 2619MB
INFO: Extracting pagemap of changed blocks
INFO: Pagemap successfully extracted, time elapsed: 0 sec
INFO: Start transferring data files
INFO: Data files are transferred, time elapsed: 3s
INFO: wait for pg_stop_backup()
INFO: pg_stop backup() successfully executed
WARNING: Backup files are not synced to disk
INFO: Validating backup QAA89O
INFO: Backup QAA89O data files are valid
INFO: Backup QAA89O resident size: 632MB
INFO: Backup QAA89O completed

real	0m11.574s
user	0m1.924s
sys	0m1.100s

21:20:23 postgres:~/dev/ptrack_test$ time pg_probackup backup -B $(pwd)/backup --instance=node -p5432 -b full --no-sync --stream
INFO: Backup start, pg_probackup version: 2.3.1, instance: node, backup ID: QAA8A6, backup mode: FULL, wal mode: STREAM, remote: false, compress-algorithm: none, compress-level: 1
INFO: PGDATA size: 2619MB
INFO: Start transferring data files
INFO: Data files are transferred, time elapsed: 32s
INFO: wait for pg_stop_backup()
INFO: pg_stop backup() successfully executed
WARNING: Backup files are not synced to disk
INFO: Validating backup QAA8A6
INFO: Backup QAA8A6 data files are valid
INFO: Backup QAA8A6 resident size: 2653MB
INFO: Backup QAA8A6 completed

real	0m42.629s
user	0m8.904s
sys	0m11.960s
```