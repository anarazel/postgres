package PostgreSQL::Test::Session;

use strict;
use warnings FATAL => 'all';


use PostgreSQL::PqFFI;

my $setup_ok;

sub setup
{
	return if $setup_ok;
	my $libdir = shift;
	PostgreSQL::PqFFI::setup($libdir);
	$setup_ok = 1;
}

sub new
{
	my $class = shift;
	my $self = {};
	bless $self, $class;
	my %args = @_;
	my $node = $args{node};
	my $dbname = $args{dbname} || 'postgres';
	die "bad node" unless $node->isa("PostgreSQL::Test::Cluster");
	unless ($setup_ok)
	{
		my $libdir = $node->config_data('--libdir');
		setup($libdir);
	}
	$self->{connstr} = $node->connstr($dbname);
	$self->{conn} = PQconnectdb($self->{connstr});
	return $self;
}

sub close
{
	my $self = shift;
	PQfinish($self->{conn});
	delete $self->{conn};
}

sub DESTROY
{
	my $self = shift;
	$self->close if $self->{conn};
}

sub reconnect
{
	my $self = shift;
	$self->close if $self->{conn};
	$self->{conn} = PQconnectdb($self->{connstr});
}

# run some sql which doesn't return tuples

sub do
{
	my $self = shift;
	my $conn = $self->{conn};
	foreach my $sql (@_)
	{
		my $result = PQexec($conn, $sql);
		my $ok = $result && (PQresultStatus($result) == PGRES_COMMAND_OK);
		PQclear($result);
		return undef unless $ok;
	}
	return 1;
}

# run some sql that does return tuples

sub query
{
	my $self = shift;
	my $sql = shift;
	my $conn = $self->{conn};
	my $result = PQexec($conn, $sql);
	my $ok = $result && (PQresultStatus($result) == PGRES_TUPLES_OK);
	unless  ($ok)
	{
		PQclear($result) if $result;
		return undef;
	}
	my $ntuples = PQntuples($result);
	my $nfields = PQnfields($result);
	my $res = {	names => [], types => [], rows => [], };
	# assuming here that the strings returned by PQfname and PQgetvalue
	# are mapped into perl space using setsvpv or similar and thus won't
	# be affect by us calling PQclear on the result object.
	foreach my $field (0 .. $nfields-1)
	{
		push(@{$res->{names}}, PQfname($result, $field));
		push(@{$res->{types}}, PQftype($result, $field));
	}
	foreach my $nrow (0.. $ntuples - 1)
	{
		my $row = [];
		foreach my $field ( 0 .. $nfields - 1)
		{
			my $val = PQgetvalue($result, $nrow, $field);
			if (($val // "") eq "")
			{
				$val = undef if PQgetisnull($result, $nrow, $field);
			}
			push(@$row, $val);
		}
		push(@{$res->{rows}}, $row);
	}
	PQclear($result);
	return $res;
}

sub query_oneval
{
	my $self = shift;
	my $sql = shift;
	my $missing_ok = shift; # default is not ok
	my $conn = $self->{conn};
	my $result = PQexec($conn, $sql);
	my $ok = $result && (PQresultStatus($result) == PGRES_TUPLES_OK);
	unless  ($ok)
	{
		PQclear($result) if $result;
		return undef;
	}
	my $ntuples = PQntuples($result);
	return undef if ($missing_ok && !$ntuples);
	my $nfields = PQnfields($result);
	die "$ntuples tuples != 1 or $nfields fields != 1"
	  if $ntuples != 1 || $nfields != 1;
	my $val = PQgetvalue($result, 0, 0);
	if ($val eq "")
	{
		$val = undef if PGgetisnull($result, 0, 0);
	}
	PQclear($result);
	return $val;
}

# return tuples like psql's -A -t mode.

sub query_tuples
{
	my $self = shift;
	my @results;
	foreach my $sql (@_)
	{
		my $res = $self->query($sql);
		# join will render undef as an empty string here
		no warnings qw(uninitialized);
		my @tuples = map { join('|', @$_); } @{$res->{rows}};
		push(@results, join("\n",@tuples));
	}
	return join("\n",@results);
}


1;
