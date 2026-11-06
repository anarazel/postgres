use strict;
use warnings;

our $config;

$config->{"tap_tests"} = 1;
$config->{"asserts"}   = 1;

# Set up by the CI workflow, see "Setup OpenSSL" in pg-ci.yml
$config->{"openssl"} = $ENV{PG_CI_OPENSSL_DIR};
$config->{"perl"}    = "c:/strawberry/perl/";

# Use the python installation found in PATH
my $python_prefix = `python -c "import sys; print(sys.prefix)"`;
chomp $python_prefix;
$config->{"python"} = $python_prefix;

1;
