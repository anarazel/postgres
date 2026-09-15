# Tests that an INSERT on referencing table correctly fails when
# the referenced value disappears due to a concurrent update
setup
{
  CREATE TABLE fcpu_parent (
    parent_key int PRIMARY KEY,
    aux   text NOT NULL
  );

  CREATE TABLE fcpu_child (
    child_key int PRIMARY KEY,
    parent_key int8 NOT NULL REFERENCES fcpu_parent
  );

  INSERT INTO fcpu_parent VALUES (1, 'foo');
}

teardown
{
  DROP TABLE fcpu_parent, fcpu_child;
}

session s1
step s1b  { BEGIN; }
step s1i { INSERT INTO fcpu_child VALUES (1, 1); }
step s1c { COMMIT; }
step s1s { SELECT * FROM fcpu_child; }

session s2
step s2b  { BEGIN; }
step s2ukey { UPDATE fcpu_parent SET parent_key = 2 WHERE parent_key = 1; }
step s2uaux { UPDATE fcpu_parent SET aux = 'bar' WHERE parent_key = 1; }
step s2ukey2 { UPDATE fcpu_parent SET parent_key = 1 WHERE parent_key = 2; }
step s2dkey { DELETE FROM fcpu_parent WHERE parent_key = 1; }
step s2c { COMMIT; }
step s2s { SELECT * FROM fcpu_parent; }

session s3
step s3b { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s3i { INSERT INTO fcpu_child VALUES (2, 1); }
step s3c { COMMIT; }
step s3s { SELECT * FROM fcpu_child; }

# fail
permutation s2b s2ukey s1b s1i s2c s1c s2s s1s
# ok
permutation s2b s2uaux s1b s1i s2c s1c s2s s1s
# ok
permutation s2b s2ukey s1b s1i s2ukey2 s2c s1c s2s s1s

# RR: key update -> serialization failure
permutation s2b s2ukey s3b s3i s2c s3c s2s s3s
# RR: key delete -> serialization failure
permutation s2b s2dkey s3b s3i s2c s3c s2s s3s
# RR: non-key update -> old version visible via transaction snapshot
permutation s2b s2uaux s3b s3i s2c s3c s2s s3s
