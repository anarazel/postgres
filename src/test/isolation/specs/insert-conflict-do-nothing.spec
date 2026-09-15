# INSERT...ON CONFLICT DO NOTHING test
#
# This test tries to expose problems with the interaction between concurrent
# sessions during INSERT...ON CONFLICT DO NOTHING.
#
# The convention here is that session 1 always ends up inserting, and session 2
# always ends up doing nothing.

setup
{
  CREATE TABLE icdn_ints (key int primary key, val text);
}

teardown
{
  DROP TABLE icdn_ints;
}

session s1
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step donothing1 { INSERT INTO icdn_ints(key, val) VALUES(1, 'donothing1') ON CONFLICT DO NOTHING; }
step c1 { COMMIT; }
step a1 { ABORT; }

session s2
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step donothing2 { INSERT INTO icdn_ints(key, val) VALUES(1, 'donothing2') ON CONFLICT DO NOTHING; }
step select2 { SELECT * FROM icdn_ints; }
step c2 { COMMIT; }

# Regular case where one session block-waits on another to determine if it
# should proceed with an insert or do nothing.
permutation donothing1 donothing2 c1 select2 c2
permutation donothing1 donothing2 a1 select2 c2
