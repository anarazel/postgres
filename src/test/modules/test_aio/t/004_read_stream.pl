# Copyright (c) 2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use TestAio;


my $node = PostgreSQL::Test::Cluster->new('test');
$node->init();

$node->append_conf(
	'postgresql.conf', qq(
shared_preload_libraries=test_aio
log_min_messages = 'DEBUG3'
log_statement=all
log_error_verbosity=default
restart_after_crash=false
temp_buffers=100
max_connections=8
io_method=worker
));

$node->start();
test_setup($node);
$node->stop();


foreach my $method (TestAio::supported_io_methods())
{
	$node->adjust_conf('postgresql.conf', 'io_method', 'worker');
	$node->start();
	test_io_method($method, $node);
	$node->stop();
}

done_testing();


sub test_setup
{
	my $node = shift;

	$node->safe_psql(
		'postgres', qq(
CREATE EXTENSION test_aio;

CREATE TABLE largeish(k int not null) WITH (FILLFACTOR=10);
INSERT INTO largeish(k) SELECT generate_series(1, 10000);
));
	ok(1, "setup");
}


sub test_repeated_blocks
{
	my $io_method = shift;
	my $node = shift;

	my $psql = $node->background_psql('postgres', on_error_stop => 0);

	# Preventing larger reads makes testing easier
	$psql->query_safe(
		qq/
SET io_combine_limit = 1;
/);

	# test miss of the same block twice in a row
	$psql->query_safe(
		qq/
SELECT evict_rel('largeish');
/);
	$psql->query_safe(
		qq/
SELECT * FROM read_stream_for_blocks('largeish', ARRAY[0, 2, 2, 4, 4]);
/);
	ok(1, "$io_method: stream missing the same block repeatedly");

	$psql->query_safe(
		qq/
SELECT * FROM read_stream_for_blocks('largeish', ARRAY[0, 2, 2, 4, 4]);
/);
	ok(1, "$io_method: stream hitting the same block repeatedly");

	# test hit of the same block twice in a row
	$psql->query_safe(
		qq/
SELECT evict_rel('largeish');
/);
	$psql->query_safe(
		qq/
SELECT * FROM read_stream_for_blocks('largeish', ARRAY[0, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1, 0]);
/);
	ok(1, "$io_method: stream accessing same block");

	$psql->quit();
}


sub test_inject_foreign
{
	my $io_method = shift;
	my $node = shift;

	my $psql_a = $node->background_psql('postgres', on_error_stop => 0);
	my $psql_b = $node->background_psql('postgres', on_error_stop => 0);

	my $pid_a = $psql_a->query_safe(qq/SELECT pg_backend_pid();/);


	###
	# Test read stream encountering buffers undergoing IO in another backend,
	# with the other backend's reads succeeding.
	###
	$psql_a->query_safe(
		qq/
SELECT evict_rel('largeish');
/);

	$psql_b->query_safe(
		qq/
SELECT inj_io_completion_wait(pid=>pg_backend_pid(), relfilenode=>pg_relation_filenode('largeish'));
/);

	$psql_b->{stdin} .= qq/
SELECT read_rel_block_ll('largeish', blockno=>5, nblocks=>1);
/;
	$psql_b->{run}->pump_nb();

	$node->poll_query_until(
		'postgres', qq/
SELECT wait_event FROM pg_stat_activity WHERE wait_event = 'completion_wait';
/,
		'completion_wait');

	$psql_a->{stdin} .= qq/
SELECT array_agg(blocknum) FROM read_stream_for_blocks('largeish', ARRAY[0, 2, 5, 7]);
/;
	$psql_a->{run}->pump_nb();

	$node->poll_query_until('postgres',
		qq(SELECT wait_event FROM pg_stat_activity WHERE pid = $pid_a),
		'AioIoCompletion');

	$node->safe_psql('postgres', qq/SELECT inj_io_completion_continue()/);

	pump_until(
		$psql_a->{run}, $psql_a->{timeout},
		\$psql_a->{stdout}, qr/\{0,2,5,7\}/);

	ok(1,
		qq/$io_method: read stream encounters succeeding IO by another backend/
	);


	###
	# Test read stream encountering buffers undergoing IO in another backend,
	# with the other backend's reads failing.
	###
	$psql_a->query_safe(
		qq/
SELECT evict_rel('largeish');
/);

	$psql_b->query_safe(
		qq/
SELECT inj_io_completion_wait(pid=>pg_backend_pid(), relfilenode=>pg_relation_filenode('largeish'));
/);

	$psql_b->query_safe(
		qq/
SELECT inj_io_short_read_attach(-errno_from_string('EIO'), pid=>pg_backend_pid(), relfilenode=>pg_relation_filenode('largeish'));
/);

	$psql_b->{stdin} .= qq/
SELECT read_rel_block_ll('largeish', blockno=>5, nblocks=>1);
/;
	$psql_b->{run}->pump_nb();

	$node->poll_query_until(
		'postgres', qq/
SELECT wait_event FROM pg_stat_activity WHERE wait_event = 'completion_wait';
/,
		'completion_wait');

	$psql_a->{stdin} .= qq/
SELECT array_agg(blocknum) FROM read_stream_for_blocks('largeish', ARRAY[0, 2, 5, 7]);
/;
	$psql_a->{run}->pump_nb();

	$node->poll_query_until('postgres',
		qq(SELECT wait_event FROM pg_stat_activity WHERE pid = $pid_a),
		'AioIoCompletion');

	$node->safe_psql('postgres', qq/SELECT inj_io_completion_continue()/);

	pump_until(
		$psql_a->{run}, $psql_a->{timeout},
		\$psql_a->{stdout}, qr/\{0,2,5,7\}/);

	$psql_b->{run}->pump_nb();
	like(
		$psql_b->{stderr},
		qr/.*ERROR.*could not read blocks 5..5.*$/,
		"$io_method: injected error occurred");
	$psql_b->{stderr} = '';
	$psql_b->query_safe(qq/SELECT inj_io_short_read_detach();/);


	ok(1,
		qq/$io_method: read stream encounters failing IO by another backend/);


	###
	# Test read stream encountering two buffers that are undergoing the same
	# IO, started by another backend
	###
	$psql_a->query_safe(
		qq/
SELECT evict_rel('largeish');
/);

	$psql_b->query_safe(
		qq/
SELECT inj_io_completion_wait(pid=>pg_backend_pid(), relfilenode=>pg_relation_filenode('largeish'));
/);

	$psql_b->{stdin} .= qq/
SELECT read_rel_block_ll('largeish', blockno=>2, nblocks=>3);
/;
	$psql_b->{run}->pump_nb();

	$node->poll_query_until(
		'postgres', qq/
SELECT wait_event FROM pg_stat_activity WHERE wait_event = 'completion_wait';
/,
		'completion_wait');

	$psql_a->{stdin} .= qq/
SELECT array_agg(blocknum) FROM read_stream_for_blocks('largeish', ARRAY[0, 2, 4]);
/;
	$psql_a->{run}->pump_nb();

	$node->poll_query_until('postgres',
		qq(SELECT wait_event FROM pg_stat_activity WHERE pid = $pid_a),
		'AioIoCompletion');

	$node->safe_psql('postgres', qq/SELECT inj_io_completion_continue()/);

	pump_until(
		$psql_a->{run}, $psql_a->{timeout},
		\$psql_a->{stdout}, qr/\{0,2,4\}/);

	ok(1, qq/$io_method: read stream encounters two buffer read in one IO/);


	$psql_a->quit();
	$psql_b->quit();
}


sub test_io_method
{
	my $io_method = shift;
	my $node = shift;

	test_repeated_blocks($io_method, $node);

  SKIP:
	{
		skip 'Injection points not supported by this build', 1
		  unless $ENV{enable_injection_points} eq 'yes';
		test_inject_foreign($io_method, $node);
	}
}
