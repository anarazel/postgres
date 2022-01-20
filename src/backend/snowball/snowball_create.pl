#!/usr/bin/perl

use strict;
use warnings;
use Getopt::Long;

my $output_path = '';
my $makefile_path = '';
my $input_path = '';

GetOptions(
	'output:s'   => \$output_path,
	'input:s'    => \$input_path) || usage();

# Make sure input_path ends in a slash if needed.
if ($input_path ne '' && substr($input_path, -1) ne '/')
{
	$output_path .= '/';
}

# Make sure output_path ends in a slash if needed.
if ($output_path ne '' && substr($output_path, -1) ne '/')
{
	$output_path .= '/';
}

GenerateTsearchFiles();

sub usage
{
	die <<EOM;
Usage: snowball_create.pl --input/-i <path> --input <path>
    --output        Output directory (default '.')
    --input         Input directory

snowball_create.pl creates snowball.sql from snowball.sql.in
EOM
}

sub GenerateTsearchFiles
{
	my $target = shift;
	my $output_file = "$output_path/snowball_create.sql";

	my $F;
	my $D;
	my $tmpl = read_file("$input_path/snowball.sql.in");
	my $mf   = read_file("$input_path/Makefile");

	open($D, '>', "$output_path/snowball_create.dep")
	  || die "Could not write snowball_create.dep";

	print $D "$output_file: $input_path/Makefile\n";
	print $D "$output_file: $input_path/snowball.sql.in\n";
	print $D "$output_file: $input_path/snowball_func.sql.in\n";

	$mf =~ s{\\\r?\n}{}g;
	$mf =~ /^LANGUAGES\s*=\s*(.*)$/m
	  || die "Could not find LANGUAGES line in snowball Makefile\n";
	my @pieces = split /\s+/, $1;
	open($F, '>', $output_file)
	  || die "Could not write snowball_create.sql";

	print $F "-- Language-specific snowball dictionaries\n";

	print $F read_file("$input_path/snowball_func.sql.in");

	while ($#pieces > 0)
	{
		my $lang    = shift @pieces || last;
		my $asclang = shift @pieces || last;
		my $txt     = $tmpl;
		my $stop    = '';
		my $stopword_path = "$input_path/stopwords/$lang.stop";

		if (-s "$stopword_path")
		{
			$stop = ", StopWords=$lang";

			print $D "$output_file: $stopword_path\n";
		}

		$txt =~ s#_LANGNAME_#${lang}#gs;
		$txt =~ s#_DICTNAME_#${lang}_stem#gs;
		$txt =~ s#_CFGNAME_#${lang}#gs;
		$txt =~ s#_ASCDICTNAME_#${asclang}_stem#gs;
		$txt =~ s#_NONASCDICTNAME_#${lang}_stem#gs;
		$txt =~ s#_STOPWORDS_#$stop#gs;
		print $F $txt;
	}
	close($F);
	close($D);
	return;
}


sub read_file
{
	my $filename = shift;
	my $F;
	local $/ = undef;
	open($F, '<', $filename) || die "Could not open file $filename\n";
	my $txt = <$F>;
	close($F);

	return $txt;
}
