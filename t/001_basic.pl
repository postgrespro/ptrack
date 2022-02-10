#
# Here we mostly do sanity checks and verify, that ptrack public API works
# as expected.  Data integrity after incremental backups taken via ptrack
# is tested on the pg_probackup side.
#

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More;

plan tests => 24;

my $node;
my $res;
my $res_stdout;
my $res_stderr;

# Initialize node
# Older version of PostgresNode.pm use get_new_node function.
# Newer use standard perl object constructor syntax
if (PostgresNode->can('get_new_node')) {
	$node = get_new_node('node');
} else {
	$node = PostgresNode->new("node");
}
$node->init;
$node->start;

# Could not load ptrack module after postmaster start
($res, $res_stdout, $res_stderr) = $node->psql("postgres", "CREATE EXTENSION ptrack");
is($res, 3, 'errors out without shared_preload_libraries = \'ptrack\'');
like(
	$res_stderr,
	qr/ptrack module must be initialized by Postmaster/,
	'errors out without shared_preload_libraries = \'ptrack\'');

# Load ptrack library
$node->append_conf(
	'postgresql.conf', q{
wal_level = 'minimal'
shared_preload_libraries = 'ptrack'
log_min_messages = debug1
});
$node->restart;

$node->safe_psql("postgres", "CREATE EXTENSION ptrack");

# Check some static functions
$node->safe_psql("postgres", "SELECT ptrack_version()");

# Could not use ptrack if disabled
($res, $res_stdout, $res_stderr) = $node->psql("postgres", "SELECT ptrack_get_pagemapset('0/0')");
is($res, 3, 'errors out if ptrack is disabled');
like(
	$res_stderr,
	qr/ptrack is disabled/,
	'errors out if ptrack is disabled');
($res, $res_stdout, $res_stderr) = $node->psql("postgres", "SELECT ptrack_init_lsn()");
is($res, 0, 'only warning if ptrack is disabled');
like(
	$res_stdout,
	qr/0\/0/,
	'should print init LSN 0/0 if disabled');
like(
	$res_stderr,
	qr/ptrack is disabled/,
	'warning if ptrack is disabled');

# Actually enable ptrack
$node->append_conf(
	'postgresql.conf', q{
ptrack.map_size = 13
});
$node->stop;
$res = $node->start(fail_ok => 1);
is($res, 0, 'could not start with wal_level = \'minimal\'');
$node->append_conf(
	'postgresql.conf', q{
wal_level = 'replica'
});
$node->start;

# Do checkpoint (test ptrack hook)
$node->safe_psql("postgres", "CHECKPOINT");

# Remember pg_current_wal_flush_lsn() value
my $flush_lsn = $node->safe_psql("postgres", "SELECT pg_current_wal_flush_lsn()");

# Remember ptrack init_lsn
my $init_lsn = $node->safe_psql("postgres", "SELECT ptrack_init_lsn()");
unlike(
	$init_lsn,
	qr/0\/0/,
	'ptrack init LSN should not be 0/0 after CHECKPOINT');

# Ptrack map should survive crash
$node->stop('immediate');
$node->start;
$res_stdout = $node->safe_psql("postgres", "SELECT ptrack_init_lsn()");
is($res_stdout, $init_lsn, 'ptrack init_lsn should be the same after crash recovery');

# Do some stuff, which hits ptrack
$node->safe_psql("postgres", "CREATE DATABASE ptrack_test");
$node->safe_psql("postgres", "CREATE TABLE ptrack_test AS SELECT i AS id FROM generate_series(0, 1000) i");

# Remember DB and relation oids
my $db_oid = $node->safe_psql("postgres", "SELECT oid FROM pg_database WHERE datname = 'ptrack_test'");
my $rel_oid = $node->safe_psql("postgres", "SELECT relfilenode FROM pg_class WHERE relname = 'ptrack_test'");

# Data should survive clean restart
$node->restart;
$res_stdout = $node->safe_psql("postgres", "SELECT ptrack_get_pagemapset('$flush_lsn')");
like(
	$res_stdout,
	qr/base\/$db_oid/,
	'ptrack pagemapset should contain new database oid');
like(
	$res_stdout,
	qr/$rel_oid/,
	'ptrack pagemapset should contain new relation oid');

# Check change stats
$res_stdout = $node->safe_psql("postgres", "SELECT pages FROM ptrack_get_change_stat('$flush_lsn')");
is($res_stdout > 0, 1, 'should be able to get aggregated stats of changes');

# We should be able to change ptrack map size (but loose all changes)
$node->append_conf(
	'postgresql.conf', q{
ptrack.map_size = 14
});
$node->restart;

$node->safe_psql("postgres", "CHECKPOINT");
$res_stdout = $node->safe_psql("postgres", "SELECT ptrack_init_lsn()");
unlike(
	$res_stdout,
	qr/0\/0/,
	'ptrack init LSN should not be 0/0 after CHECKPOINT');
ok($res_stdout ne $init_lsn, 'ptrack init_lsn should not be the same after map resize');
$res_stdout = $node->safe_psql("postgres", "SELECT ptrack_get_pagemapset('$flush_lsn')");
unlike(
	$res_stdout,
	qr/base\/$db_oid/,
	'we should loose changes after ptrack map resize');

# We should be able to turn off ptrack and clean up all files by stting ptrack.map_size = 0
$node->append_conf(
	'postgresql.conf', q{
ptrack.map_size = 0
});
$node->restart;

# Check that we have lost everything
ok(! -f $node->data_dir . "/global/ptrack.map", "ptrack.map should be cleaned up");
ok(! -f $node->data_dir . "/global/ptrack.map.tmp", "ptrack.map.tmp should be cleaned up");

($res, $res_stdout, $res_stderr) = $node->psql("postgres", "SELECT ptrack_get_pagemapset('0/0')");
is($res, 3, 'errors out if ptrack is disabled');
like(
	$res_stderr,
	qr/ptrack is disabled/,
	'errors out if ptrack is disabled');
($res, $res_stdout, $res_stderr) = $node->psql("postgres", "SELECT ptrack_init_lsn()");
is($res, 0, 'only warning if ptrack is disabled');
like(
	$res_stdout,
	qr/0\/0/,
	'should print init LSN 0/0 if disabled');
like(
	$res_stderr,
	qr/ptrack is disabled/,
	'warning if ptrack is disabled');

$node->stop;

done_testing;
