# Copyright (c) 2023, PostgreSQL Global Development Group

# Check that vacuum_defer_cleanup_age works.

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

# to make things more predictable, we schedule all vacuums ourselves
$primary->append_conf('postgresql.conf', qq[
autovacuum = 0
log_line_prefix='%m [%p][%b][%v:%x][%a] '
]);

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup',
  has_streaming => 1);

# vacuum_defer_cleanup_age only makes sense without feedback
# We don't want to ever wait for the standby to apply, otherwise the test will
# take forever.
$standby->append_conf('postgresql.conf', qq[
hot_standby_feedback = 0
max_standby_streaming_delay = 0
]);
$standby->start;

# to avoid constantly needing to wait manually, use syncrep with remote_apply
$primary->append_conf('postgresql.conf', qq[
synchronous_standby_names = '*'
synchronous_commit = remote_apply
]);

$standby->reload;


# Find original txid at start, we want to use a cleanup age bigger than that,
# because we had bugs with horizons wrapping around to before the big bang
my $xid_at_start = $primary->safe_psql('postgres', 'SELECT txid_current()');
my $defer_age = $xid_at_start + 100;

note "Initial xid is $xid_at_start, will set defer to $defer_age";

$primary->safe_psql('postgres', qq[
  ALTER SYSTEM SET vacuum_defer_cleanup_age = $defer_age;
  SELECT pg_reload_conf();
]);

$primary->safe_psql('postgres', qq[
  CREATE TABLE testdata(id int not null unique, data text);
  INSERT INTO testdata(id, data) VALUES(1, '1');
  INSERT INTO testdata(id, data) VALUES(2, '2');
  INSERT INTO testdata(id, data) VALUES(3, '3');
  INSERT INTO testdata(id, data) VALUES(4, '4');
  INSERT INTO testdata(id, data) VALUES(5, '5');
]);


# start a background psql on both nodes, to test effects on running sessions
my $psql_timeout = IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default);
my %psql_primary = ('stdin' => '', 'stdout' => '');
$psql_primary{run} =
  $primary->background_psql('postgres', \$psql_primary{stdin},
	\$psql_primary{stdout},
	$psql_timeout);

my %psql_standby = ('stdin' => '', 'stdout' => '');
$psql_standby{run} =
  $standby->background_psql('postgres', \$psql_standby{stdin},
	\$psql_standby{stdout},
	$psql_timeout);

$psql_primary{stdin} .= '\t\set QUIET off';
$psql_standby{stdin} .= '\t\set QUIET off';

# start transaction on the standby
my $q = qq[
SET application_name = 'background_psql';
BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;
SELECT 1;
];
$psql_primary{stdin} .= $q;
$psql_standby{stdin} .= $q;
ok( pump_until_primary(qr/\n\(1 row\)\n$/s), 'started transaction');
ok( pump_until_standby(qr/\n\(1 row\)\n$/s), 'started transaction');

# update a row on the primary, select from the table to remove old row version
my $res = $primary->safe_psql('postgres', qq[
  SELECT txid_current();
  UPDATE testdata SET data = data || '-updated' WHERE id = 1;
  SELECT txid_current();
  VACUUM testdata;
  SELECT count(*) FROM testdata;
  SELECT count(*) FROM testdata;
  SELECT txid_current();
  SET enable_seqscan = 0;
  EXPLAIN SELECT * FROM testdata WHERE id = 1;
  SELECT * FROM testdata WHERE id = 1;
]);


# check query result is as expected on primary and on a new snapshot on the standby
$q = "SELECT data FROM testdata WHERE id = 1";
is($primary->safe_psql('postgres', $q), '1-updated', "query result on primary as expected");
is($standby->safe_psql('postgres', $q), '1-updated', "query result on standby as expected");

$q = "SELECT data FROM testdata WHERE id = 1;\n";
$psql_primary{stdin} .= $q;
$psql_standby{stdin} .= $q;

$res = pump_until_primary(qr/^.*\n\(1 row\)\n$/s);
my $expect = "data\n1\n(1 row)\n";
is( $res, $expect, 'version 1 still visible on primary after update on primary');
$res = pump_until_standby(qr/.*\n\(1 row\)\n$/s);
is( $res, $expect, 'row 1, version 1 still visible on standby after update on primary');

$psql_primary{stdin} .= "COMMIT;\n";
is(pump_until_primary(qr/^COMMIT\n/m), "COMMIT\n", "release on primary");


done_testing();

sub pump_until_node
{
	my $psql = shift;
	my $match = shift;
	my $ret;

	pump_until($$psql{run}, $psql_timeout,
		\$$psql{stdout}, $match);
	$ret = $$psql{stdout};
	$$psql{stdout} = '';
	return $ret;
}

sub pump_until_primary
{
	my $match = shift;

	return pump_until_node(\%psql_primary, $match);
}

sub pump_until_standby
{
	my $match = shift;

	return pump_until_node(\%psql_standby, $match);
}

sub burn_xids
{
	my $cnt = shift;

	my $out = $primary->safe_psql('postgres', qq[
SELECT txid_current();
DO $$
  BEGIN FOR i IN 1..100 LOOP
    PERFORM txid_current();
    COMMIT AND CHAIN;
  END LOOP;
END; $$;
SELECT txid_current();
]);
	note $out;
}
