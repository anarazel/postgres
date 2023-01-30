
# Copyright (c) 2021-2023, PostgreSQL Global Development Group

package PostgreSQL::Test::BackgroundPsql;

use strict;
use warnings;

use Carp;
use Config;
use IPC::Run;
use PostgreSQL::Test::Utils qw(pump_until);
use Test::More;

# Start a new psql background session
#
# Parameters:
# - "interactive" - should a PTY be used
# - "psql" - psql command, parameters, including connection string
sub new
{
	my $class = shift;
	my ($interactive, $psql_params) = @_;
	my $psql = {'stdin' => '', 'stdout' => '', 'stderr' => ''};
	my $run;

	$psql->{timeout} = IPC::Run::timeout($PostgreSQL::Test::Utils::timeout_default);

	if ($interactive)
	{
		$run = IPC::Run::start $psql_params,
		  '<pty<', \$psql->{stdin}, '>pty>', \$psql->{stdout}, '2>', \$psql->{stderr},
		  $psql->{timeout};
	}
	else
	{
		$run = IPC::Run::start $psql_params,
		  '<', \$psql->{stdin}, '>', \$psql->{stdout}, '2>', \$psql->{stderr},
		  $psql->{timeout};
	}

	$psql->{run} = $run;

	my $self = bless $psql, $class;

	$self->wait_connect();

	return $self;
}

sub wait_connect
{
	my ($self) = @_;

	# Request some output, and pump until we see it.  This means that psql
	# connection failures are caught here, relieving callers of the need to
	# handle those.  (Right now, we have no particularly good handling for
	# errors anyway, but that might be added later.)
	my $banner = "background_psql: ready";
	$self->{stdin} .= "\\echo $banner\n";
	$self->{run}->pump() until $self->{stdout} =~ /$banner/ || $self->{timeout}->is_expired;
	$self->{stdout} = ''; # clear out banner

	die "psql startup timed out" if $self->{timeout}->is_expired;
}

sub quit
{
	my ($self) = @_;

	$self->{stdin} .= "\\q\n";

	return $self->{run}->finish;
}

sub reconnect_and_clear
{
	my ($self) = @_;

	# If psql isn't dead already, tell it to quit as \q, when already dead,
	# causes IPC::Run to unhelpfully error out with "ack Broken pipe:".
	$self->{run}->pump_nb();
	if ($self->{run}->pumpable())
	{
		$self->{stdin} .= "\\q\n";
	}
	$self->{run}->finish;

	# restart
	$self->{run}->run();
	$self->{stdin}  = '';
	$self->{stdout} = '';

	$self->wait_connect()
}

sub query
{
	my ($self, $query) = @_;
	my $ret;
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	note "issuing query via background psql: $query";

	# feed the query to psql's stdin, follwed by \n (so psql processes the
	# line), by a ; (so that psql issues the query, if it doesnt't include a ;
	# itself), and a separator echoed with \echo, that we can wait on.
	my $banner = "background_psql: QUERY_SEPARATOR";
	$self->{stdin} .= "$query\n;\n\\echo $banner\n";

	pump_until($self->{run}, $self->{timeout}, \$self->{stdout}, qr/$banner/);

	die "psql query timed out" if $self->{timeout}->is_expired;
	$ret = $self->{stdout};

	# remove banner again, our caller doesn't care
	$ret =~ s/\n$banner$//s;

	# clear out output for the next query
	$self->{stdout} = '';

	return $ret;
}

# Like query(), but errors out if the query failed.
#
# Query failure is determined by producing output on stderr.
sub query_safe
{
	my ($self, $query) = @_;

	my $ret = $self->query($query);

	if ($self->{stderr} ne "")
	{
		die "query failed: $self->{stderr}";
	}

	return $ret;
}

# issue query, but only wait for $until, not query completion
#
# Note: Query needs newline and semicolon for psql to process the input.
sub query_until
{
	my ($self, $until, $query) = @_;
	my $ret;
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	$self->{stdin} .= $query;

	pump_until($self->{run}, $self->{timeout}, \$self->{stdout}, $until);

	die "psql query timed out" if $self->{timeout}->is_expired;

	$ret = $self->{stdout};

	# clear out output for the next query
	$self->{stdout} = '';

	return $ret;
}

1;
