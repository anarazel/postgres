# Test for log messages emitted by VACUUM and ANALYZE when a specified
# relation is concurrently dropped.
#
# This also verifies that log messages are not emitted for concurrently
# dropped relations that were not specified in the VACUUM or ANALYZE
# command.

setup
{
	CREATE TABLE vcd_parted (a INT) PARTITION BY LIST (a);
	CREATE TABLE vcd_part1 PARTITION OF vcd_parted FOR VALUES IN (1);
	CREATE TABLE vcd_part2 PARTITION OF vcd_parted FOR VALUES IN (2);
}

teardown
{
	DROP TABLE IF EXISTS vcd_parted;
}

session s1
step lock
{
	BEGIN;
	LOCK vcd_part1 IN SHARE MODE;
}
step drop_and_commit
{
	DROP TABLE vcd_part2;
	COMMIT;
}

session s2
step vac_specified		{ VACUUM vcd_part1, vcd_part2; }
step vac_all_parts		{ VACUUM vcd_parted; }
step analyze_specified	{ ANALYZE vcd_part1, vcd_part2; }
step analyze_all_parts	{ ANALYZE vcd_parted; }
step vac_analyze_specified	{ VACUUM ANALYZE vcd_part1, vcd_part2; }
step vac_analyze_all_parts	{ VACUUM ANALYZE vcd_parted; }

permutation lock vac_specified drop_and_commit
permutation lock vac_all_parts drop_and_commit
permutation lock analyze_specified drop_and_commit
permutation lock analyze_all_parts drop_and_commit
permutation lock vac_analyze_specified drop_and_commit
permutation lock vac_analyze_all_parts drop_and_commit
