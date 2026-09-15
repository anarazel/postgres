# ALTER TABLE - Enable and disable triggers with concurrent reads
#
# ENABLE/DISABLE TRIGGER uses ShareRowExclusiveLock so we mix writes with
# it to see what works or waits.

setup
{
 CREATE TABLE at3_a (i int PRIMARY KEY);
 INSERT INTO at3_a VALUES (0), (1), (2), (3);
 CREATE FUNCTION at3_f() RETURNS TRIGGER LANGUAGE plpgsql AS 'BEGIN RETURN NULL; END;';
 CREATE TRIGGER at3_t AFTER UPDATE ON at3_a EXECUTE PROCEDURE at3_f();
}

teardown
{
 DROP TABLE at3_a;
 DROP FUNCTION at3_f();
}

session s1
step s1a { BEGIN; }
step s1b { ALTER TABLE at3_a DISABLE TRIGGER at3_t; }
step s1c { ALTER TABLE at3_a ENABLE TRIGGER at3_t; }
step s1d { COMMIT; }

session s2
step s2a { BEGIN; }
step s2b { SELECT * FROM at3_a WHERE i = 1 LIMIT 1 FOR UPDATE; }
step s2c { INSERT INTO at3_a VALUES (0); }
step s2d { COMMIT; }

permutation s1a s1b s1c s1d s2a s2b s2c s2d
permutation s1a s1b s1c s2a s1d s2b s2c s2d
permutation s1a s1b s1c s2a s2b s1d s2c s2d
permutation s1a s1b s1c s2a s2b s2c s1d s2d
permutation s1a s1b s2a s1c s1d s2b s2c s2d
permutation s1a s1b s2a s1c s2b s1d s2c s2d
permutation s1a s1b s2a s1c s2b s2c s1d s2d
permutation s1a s1b s2a s2b s1c s1d s2c s2d
permutation s1a s1b s2a s2b s1c s2c s1d s2d
permutation s1a s1b s2a s2b s2c s1c s1d s2d
permutation s1a s2a s1b s1c s1d s2b s2c s2d
permutation s1a s2a s1b s1c s2b s1d s2c s2d
permutation s1a s2a s1b s1c s2b s2c s1d s2d
permutation s1a s2a s1b s2b s1c s1d s2c s2d
permutation s1a s2a s1b s2b s1c s2c s1d s2d
permutation s1a s2a s1b s2b s2c s1c s1d s2d
permutation s1a s2a s2b s1b s1c s1d s2c s2d
permutation s1a s2a s2b s1b s1c s2c s1d s2d
permutation s1a s2a s2b s1b s2c s1c s1d s2d
permutation s1a s2a s2b s2c s1b s1c s1d s2d
permutation s1a s2a s2b s2c s1b s1c s2d s1d
permutation s1a s2a s2b s2c s1b s2d s1c s1d
permutation s1a s2a s2b s2c s2d s1b s1c s1d
permutation s2a s1a s1b s1c s1d s2b s2c s2d
permutation s2a s1a s1b s1c s2b s1d s2c s2d
permutation s2a s1a s1b s1c s2b s2c s1d s2d
permutation s2a s1a s1b s2b s1c s1d s2c s2d
permutation s2a s1a s1b s2b s1c s2c s1d s2d
permutation s2a s1a s1b s2b s2c s1c s1d s2d
permutation s2a s1a s2b s1b s1c s1d s2c s2d
permutation s2a s1a s2b s1b s1c s2c s1d s2d
permutation s2a s1a s2b s1b s2c s1c s1d s2d
permutation s2a s1a s2b s2c s1b s1c s1d s2d
permutation s2a s1a s2b s2c s1b s1c s2d s1d
permutation s2a s1a s2b s2c s1b s2d s1c s1d
permutation s2a s1a s2b s2c s2d s1b s1c s1d
permutation s2a s2b s1a s1b s1c s1d s2c s2d
permutation s2a s2b s1a s1b s1c s2c s1d s2d
permutation s2a s2b s1a s1b s2c s1c s1d s2d
permutation s2a s2b s1a s2c s1b s1c s1d s2d
permutation s2a s2b s1a s2c s1b s1c s2d s1d
permutation s2a s2b s1a s2c s1b s2d s1c s1d
permutation s2a s2b s1a s2c s2d s1b s1c s1d
permutation s2a s2b s2c s1a s1b s1c s1d s2d
permutation s2a s2b s2c s1a s1b s1c s2d s1d
permutation s2a s2b s2c s1a s1b s2d s1c s1d
permutation s2a s2b s2c s1a s2d s1b s1c s1d
permutation s2a s2b s2c s2d s1a s1b s1c s1d
