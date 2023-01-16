use strict;
use warnings;
use Test::More;

my $pg_15_modules;

BEGIN
{
	$pg_15_modules = eval
	{
		require PostgreSQL::Test::Cluster;
		require PostgreSQL::Test::Utils;
		return 1;
	};

	unless (defined $pg_15_modules)
	{
		$pg_15_modules = 0;

		require PostgresNode;
		require TestLib;
	}
}

note('PostgreSQL 15 modules are used: ' . ($pg_15_modules ? 'yes' : 'no'));

my $node;
my $res_stdout;
my $res_stderr;

# Create node.
# Older versions of PostgreSQL modules use get_new_node function.
# Newer use standard perl object constructor syntax.
eval
{
	if ($pg_15_modules)
	{
		$node = PostgreSQL::Test::Cluster->new("node");
	}
	else
	{
		$node = PostgresNode::get_new_node("node");
	}
};

note "Test for handling a ptrack map in compressed relations";

my $psql_stdout;

# Starting the node
$node->init;

# Could not load ptrack module after postmaster start

my $cfs_tblspc1 = $node->basedir."/cfs_tblspc1";
my $cfs_tblspc2 = $node->basedir."/cfs_tblspc2";
mkdir $cfs_tblspc1 or die;
mkdir $cfs_tblspc2 or die;
my $no_cfs_tblspc1 = $node->basedir."/no_cfs_tblspc1";
my $no_cfs_tblspc2 = $node->basedir."/no_cfs_tblspc2";
mkdir $no_cfs_tblspc1 or die;
mkdir $no_cfs_tblspc2 or die;

$node->append_conf('postgresql.conf', qq{	
	shared_preload_libraries = 'ptrack'
	ptrack.map_size = 16
	wal_level = 'replica'
	autovacuum = 'off'
});

$node->start;

# check cfs availability first
my $cfs_available = $node->safe_psql('postgres',
			"select count(oid) from pg_proc where proname = 'cfs_version'");

if($cfs_available eq "0") {
	$node->stop;
	plan skip_all => "CFS is not supported by this PostgreSQL build";
} else {
	plan tests => 2;
}

# Creating content
$node->safe_psql('postgres', qq|
	create tablespace cfs_tblspc1 location '$cfs_tblspc1' with (compression=true);
	create tablespace cfs_tblspc2 location '$cfs_tblspc2' with (compression=true);
	create tablespace no_cfs_tblspc1 location '$no_cfs_tblspc1';
	create tablespace no_cfs_tblspc2 location '$no_cfs_tblspc2';

	create database testing_cfs tablespace cfs_tblspc1;
	create database testing_no_cfs tablespace no_cfs_tblspc1;
|);

$node->safe_psql('testing_cfs', qq{
	create table testing(i int, text varchar);
	insert into testing select 1, '1111111111111111111111111' from generate_series(1,10000000);
});

$node->safe_psql('testing_no_cfs', qq{
	create table testing_no(i int, text varchar);
	insert into testing_no select 1, '1111111111111111111111111' from generate_series(1,10000000);
});

# creating ptrack
$node->safe_psql('postgres', "create extension ptrack");

# obtaining init lsn for further usage in ptrack_get_pagemapset
my $init_lsn = $node->safe_psql('postgres', 'select ptrack_init_lsn()');

# forcing copydir() hook by altering dbs tablespaces
$node->safe_psql('postgres', "alter database testing_cfs set tablespace cfs_tblspc2;");
$node->safe_psql('postgres', "alter database testing_no_cfs set tablespace no_cfs_tblspc2;");

# obtaining relpath for cfs table
my $cfs_relpath = $node->safe_psql('testing_cfs', "select pg_relation_filepath('testing');");

# obtaining relpath for no-cfs table
my $no_cfs_relpath = $node->safe_psql('testing_no_cfs', "select pg_relation_filepath('testing_no');");

# select the pagecount sums and compare them (should be equal)
my $pagecount_sum_cfs = $node->safe_psql('postgres',
			"select sum(pagecount) from ptrack_get_pagemapset('$init_lsn'::pg_lsn) where path like '%$cfs_relpath';");
my $pagecount_sum_no_cfs = $node->safe_psql('postgres',
			"select sum(pagecount) from ptrack_get_pagemapset('$init_lsn'::pg_lsn) where path like '%$no_cfs_relpath';");

is($pagecount_sum_cfs, $pagecount_sum_no_cfs, "pagecount sums don't match");

# forcing copydir() hook by altering dbs tablespaces back
$node->safe_psql('postgres', "alter database testing_cfs set tablespace cfs_tblspc1;");
$node->safe_psql('postgres', "alter database testing_no_cfs set tablespace no_cfs_tblspc1;");

# obtaining new relpath for cfs table
$cfs_relpath = $node->safe_psql('testing_cfs', "select pg_relation_filepath('testing');");

# obtaining new relpath for no-cfs table
$no_cfs_relpath = $node->safe_psql('testing_no_cfs', "select pg_relation_filepath('testing_no');");

# select the pagecount sums and compare them (again, they should be equal)
$pagecount_sum_cfs = $node->safe_psql('postgres',
			"select sum(pagecount) from ptrack_get_pagemapset('$init_lsn'::pg_lsn) where path like '%$cfs_relpath';");
$pagecount_sum_no_cfs = $node->safe_psql('postgres',
			"select sum(pagecount) from ptrack_get_pagemapset('$init_lsn'::pg_lsn) where path like '%$no_cfs_relpath';");

is($pagecount_sum_cfs, $pagecount_sum_no_cfs, "pagecount sums don't match");


$node->stop;

