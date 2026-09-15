# Test that an error if thrown if the target row has been moved to a
# different partition by a concurrent session.

setup
{
  --
  -- Setup to test an error from ExecUpdate and ExecDelete.
  --
  CREATE TABLE pku1_foo (a int, b text) PARTITION BY LIST(a);
  CREATE TABLE pku1_foo1 PARTITION OF pku1_foo FOR VALUES IN (1);
  CREATE TABLE pku1_foo2 PARTITION OF pku1_foo FOR VALUES IN (2);
  INSERT INTO pku1_foo VALUES (1, 'ABC');

  --
  -- Setup to test an error from GetTupleForTrigger
  --
  CREATE TABLE pku1_footrg (a int, b text) PARTITION BY LIST(a);
  CREATE TABLE pku1_footrg1 PARTITION OF pku1_footrg FOR VALUES IN (1);
  CREATE TABLE pku1_footrg2 PARTITION OF pku1_footrg FOR VALUES IN (2);
  INSERT INTO pku1_footrg VALUES (1, 'ABC');
  CREATE FUNCTION func_footrg_mod_a() RETURNS TRIGGER AS $$
    BEGIN
	  NEW.a = 2; -- This is changing partition key column.
   RETURN NEW;
  END $$ LANGUAGE PLPGSQL;
  CREATE TRIGGER pku1_footrg_mod_a BEFORE UPDATE ON pku1_footrg1
   FOR EACH ROW EXECUTE PROCEDURE func_footrg_mod_a();

  --
  -- Setup to test an error from ExecLockRows
  --
  CREATE TABLE pku1_foo_range_parted (a int, b text) PARTITION BY RANGE(a);
  CREATE TABLE pku1_foo_range_parted1 PARTITION OF pku1_foo_range_parted FOR VALUES FROM (1) TO (10);
  CREATE TABLE pku1_foo_range_parted2 PARTITION OF pku1_foo_range_parted FOR VALUES FROM (10) TO (20);
  INSERT INTO pku1_foo_range_parted VALUES(7, 'ABC');
  CREATE UNIQUE INDEX pku1_foo_range_parted1_a_unique ON pku1_foo_range_parted1 (a);
  CREATE TABLE pku1_bar (a int REFERENCES pku1_foo_range_parted1(a));
}

teardown
{
  DROP TABLE pku1_foo;
  DROP TRIGGER pku1_footrg_mod_a ON pku1_footrg1;
  DROP FUNCTION func_footrg_mod_a();
  DROP TABLE pku1_footrg;
  DROP TABLE pku1_bar, pku1_foo_range_parted;
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1u	{ UPDATE pku1_foo SET a=2 WHERE a=1; }
step s1u2	{ UPDATE pku1_footrg SET b='EFG' WHERE a=1; }
step s1u3pc	{ UPDATE pku1_foo_range_parted SET a=11 WHERE a=7; }
step s1u3npc	{ UPDATE pku1_foo_range_parted SET b='XYZ' WHERE a=7; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2u	{ UPDATE pku1_foo SET b='EFG' WHERE a=1; }
step s2u2	{ UPDATE pku1_footrg SET b='XYZ' WHERE a=1; }
step s2i	{ INSERT INTO pku1_bar VALUES(7); }
step s2d	{ DELETE FROM pku1_foo WHERE a=1; }
step s2c	{ COMMIT; }

# Concurrency error from ExecUpdate and ExecDelete.
permutation s1b s2b s1u s1c s2d s2c
permutation s1b s2b s1u s2d s1c s2c
permutation s1b s2b s1u s2u s1c s2c
permutation s1b s2b s2d s1u s2c s1c

# Concurrency error from GetTupleForTrigger
permutation s1b s2b s1u2 s1c s2u2 s2c
permutation s1b s2b s1u2 s2u2 s1c s2c
permutation s1b s2b s2u2 s1u2 s2c s1c

# Concurrency error from ExecLockRows
# test waiting for moved row itself
permutation s1b s2b s1u3pc s2i s1c s2c
permutation s1b s2b s1u3pc s2i s1r s2c
# test waiting for in-partition update, followed by cross-partition move
permutation s1b s2b s1u3npc s1u3pc s2i s1c s2c
permutation s1b s2b s1u3npc s1u3pc s2i s1r s2c
# test waiting for in-partition update, followed by cross-partition move
permutation s1b s2b s1u3npc s1u3pc s1u3pc s2i s1c s2c
permutation s1b s2b s1u3npc s1u3pc s1u3pc s2i s1r s2c
