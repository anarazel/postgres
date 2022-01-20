# Copyright (c) 2021, PostgreSQL Global Development Group

use strict;
use warnings;
use 5.8.0;
use List::Util qw(max);

my ($deffile, $txtfile, $libname) = @ARGV;

print STDERR "Generating $deffile...\n";
open(my $if, '<', $txtfile) || die("Could not open $txtfile\n");
open(my $of, '>', $deffile) || die("Could not open $deffile for writing\n");
print $of "LIBRARY $libname\nEXPORTS\n";
while (<$if>)
{
	next if (/^#/);
	next if (/^\s*$/);
	my ($f, $o) = split;
	print $of " $f @ $o\n";
}
close($of);
close($if);
