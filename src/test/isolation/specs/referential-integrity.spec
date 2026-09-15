# Referential Integrity test
#
# The assumption here is that the application code issuing the SELECT
# to test for the presence or absence of ri_a related record would do the
# right thing -- this script doesn't include that logic.
#
# Any overlap between the transactions must cause ri_a serialization failure.

setup
{
 CREATE TABLE ri_a (i int PRIMARY KEY);
 CREATE TABLE ri_b (a_id int);
 INSERT INTO ri_a VALUES (1);
}

teardown
{
 DROP TABLE ri_a, ri_b;
}

session s1
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step rx1	{ SELECT i FROM ri_a WHERE i = 1; }
step wy1	{ INSERT INTO ri_b VALUES (1); }
step c1		{ COMMIT; }

session s2
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step rx2	{ SELECT i FROM ri_a WHERE i = 1; }
step ry2	{ SELECT a_id FROM ri_b WHERE a_id = 1; }
step wx2	{ DELETE FROM ri_a WHERE i = 1; }
step c2		{ COMMIT; }
