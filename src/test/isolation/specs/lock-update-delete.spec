# This test verifies behavior when traversing an update chain during
# locking an old version of the tuple.  There are three tests here:
# 1. update the tuple, then delete it; a second transaction locks the
# first version.  This should raise an error if the DELETE succeeds,
# but be allowed to continue if it aborts.
# 2. Same as (1), except that instead of deleting the tuple, we merely
# update its key.  The behavior should be the same as for (1).
# 3. Same as (2), except that we update the tuple without modifying its
# key. In this case, no error should be raised.
# When run in REPEATABLE READ or SERIALIZABLE transaction isolation levels, all
# permutations that commit s2 cause a serializability error; all permutations
# that rollback s2 can get through.
#
# We use an advisory lock (which is locked during s1's setup) to let s2 obtain
# its snapshot early and only allow it to actually traverse the update chain
# when s1 is done creating it.

setup
{
  DROP TABLE IF EXISTS lud_foo;
  CREATE TABLE lud_foo (
	key		int PRIMARY KEY,
	value	int
  );

  INSERT INTO lud_foo VALUES (1, 1);
}

teardown
{
  DROP TABLE lud_foo;
}

session s1
# obtain lock on the tuple, traversing its update chain
step s1l	{ SELECT * FROM lud_foo WHERE pg_advisory_xact_lock(0) IS NOT NULL AND key = 1 FOR KEY SHARE; }

session s2
setup		{ SELECT pg_advisory_lock(0); }
step s2b	{ BEGIN; }
step s2u	{ UPDATE lud_foo SET value = 2 WHERE key = 1; }
step s2_blocker1	{ DELETE FROM lud_foo; }
step s2_blocker2	{ UPDATE lud_foo SET key = 2 WHERE key = 1; }
step s2_blocker3	{ UPDATE lud_foo SET value = 2 WHERE key = 1; }
step s2_unlock		{ SELECT pg_advisory_unlock(0); }
step s2c	{ COMMIT; }
step s2r	{ ROLLBACK; }

permutation s2b s1l s2u s2_blocker1 s2_unlock s2c
permutation s2b s1l s2u s2_blocker2 s2_unlock s2c
permutation s2b s1l s2u s2_blocker3 s2_unlock s2c
permutation s2b s1l s2u s2_blocker1 s2_unlock s2r
permutation s2b s1l s2u s2_blocker2 s2_unlock s2r
permutation s2b s1l s2u s2_blocker3 s2_unlock s2r

permutation s2b s1l s2u s2_blocker1 s2c s2_unlock
permutation s2b s1l s2u s2_blocker2 s2c s2_unlock
permutation s2b s1l s2u s2_blocker3 s2c s2_unlock
permutation s2b s1l s2u s2_blocker1 s2r s2_unlock
permutation s2b s1l s2u s2_blocker2 s2r s2_unlock
permutation s2b s1l s2u s2_blocker3 s2r s2_unlock
