use strict;
use warnings;

my $format = $ARGV[0] or die "$0: missing required argument: format\n";
my $input = $ARGV[1] or die "$0: missing required argument: input\n";
my $output = $ARGV[2] or die "$0: missing required argument: output\n";

#FIXME: handle format argument, so we can reuse the one script for several platforms
$format eq 'gnu' or die "$0: $format is not yet handled (only gnu is)\n";

open(my $input_handle, '<', $input)
  or die "$0: could not open input file '$input': $!\n";

open(my $output_handle, '>', $output)
  or die "$0: could not open output file '$output': $!\n";

print $output_handle "{
  global:
";

while (<$input_handle>)
{
	if (/^#/)
	{
		# don't do anything with a comment
	}
	elsif (/^([^\s]+)\s+([^\s]+)/)
	{
		print $output_handle "    $1;\n";
	}
	else
	{
		die "$0: unexpected line $_\n";
	}
}

print $output_handle "  local: *;
};
";

exit(0);
