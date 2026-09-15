# ALTER TABLE - Add foreign keys with concurrent reads
#
# ADD CONSTRAINT uses ShareRowExclusiveLock so we mix writes with it
# to see what works or waits.

setup
{
 CREATE TABLE at2_a (i int PRIMARY KEY);
 CREATE TABLE at2_b (a_id int);
 INSERT INTO at2_a VALUES (0), (1), (2), (3);
 INSERT INTO at2_b SELECT generate_series(1,1000) % 4;
}

teardown
{
 DROP TABLE at2_a, at2_b;
}

session s1
step s1a { BEGIN; }
step s1b { ALTER TABLE at2_b ADD CONSTRAINT bfk FOREIGN KEY (a_id) REFERENCES at2_a (i) NOT VALID; }
step s1c { COMMIT; }

session s2
step s2a { BEGIN; }
step s2b { SELECT * FROM at2_a WHERE i = 1 LIMIT 1 FOR UPDATE; }
step s2c { SELECT * FROM at2_b WHERE a_id = 3 LIMIT 1 FOR UPDATE; }
step s2d { INSERT INTO at2_b VALUES (0); }
step s2e { INSERT INTO at2_a VALUES (4); }
step s2f { COMMIT; }

permutation s1a s1b s1c s2a s2b s2c s2d s2e s2f
permutation s1a s1b s2a s1c s2b s2c s2d s2e s2f
permutation s1a s1b s2a s2b s1c s2c s2d s2e s2f
permutation s1a s1b s2a s2b s2c s1c s2d s2e s2f
permutation s1a s1b s2a s2b s2c s2d s1c s2e s2f
permutation s1a s2a s1b s1c s2b s2c s2d s2e s2f
permutation s1a s2a s1b s2b s1c s2c s2d s2e s2f
permutation s1a s2a s1b s2b s2c s1c s2d s2e s2f
permutation s1a s2a s1b s2b s2c s2d s1c s2e s2f
permutation s1a s2a s2b s1b s1c s2c s2d s2e s2f
permutation s1a s2a s2b s1b s2c s1c s2d s2e s2f
permutation s1a s2a s2b s1b s2c s2d s1c s2e s2f
permutation s1a s2a s2b s2c s1b s1c s2d s2e s2f
permutation s1a s2a s2b s2c s1b s2d s1c s2e s2f
permutation s1a s2a s2b s2c s2d s1b s2e s2f s1c
permutation s1a s2a s2b s2c s2d s2e s1b s2f s1c
permutation s1a s2a s2b s2c s2d s2e s2f s1b s1c
permutation s2a s1a s1b s1c s2b s2c s2d s2e s2f
permutation s2a s1a s1b s2b s1c s2c s2d s2e s2f
permutation s2a s1a s1b s2b s2c s1c s2d s2e s2f
permutation s2a s1a s1b s2b s2c s2d s1c s2e s2f
permutation s2a s1a s2b s1b s1c s2c s2d s2e s2f
permutation s2a s1a s2b s1b s2c s1c s2d s2e s2f
permutation s2a s1a s2b s1b s2c s2d s1c s2e s2f
permutation s2a s1a s2b s2c s1b s1c s2d s2e s2f
permutation s2a s1a s2b s2c s1b s2d s1c s2e s2f
permutation s2a s1a s2b s2c s2d s1b s2e s2f s1c
permutation s2a s1a s2b s2c s2d s2e s1b s2f s1c
permutation s2a s1a s2b s2c s2d s2e s2f s1b s1c
permutation s2a s2b s1a s1b s1c s2c s2d s2e s2f
permutation s2a s2b s1a s1b s2c s1c s2d s2e s2f
permutation s2a s2b s1a s1b s2c s2d s1c s2e s2f
permutation s2a s2b s1a s2c s1b s1c s2d s2e s2f
permutation s2a s2b s1a s2c s1b s2d s1c s2e s2f
permutation s2a s2b s1a s2c s2d s1b s2e s2f s1c
permutation s2a s2b s1a s2c s2d s2e s1b s2f s1c
permutation s2a s2b s1a s2c s2d s2e s2f s1b s1c
permutation s2a s2b s2c s1a s1b s1c s2d s2e s2f
permutation s2a s2b s2c s1a s1b s2d s1c s2e s2f
permutation s2a s2b s2c s1a s2d s1b s2e s2f s1c
permutation s2a s2b s2c s1a s2d s2e s1b s2f s1c
permutation s2a s2b s2c s1a s2d s2e s2f s1b s1c
permutation s2a s2b s2c s2d s1a s1b s2e s2f s1c
permutation s2a s2b s2c s2d s1a s2e s1b s2f s1c
permutation s2a s2b s2c s2d s1a s2e s2f s1b s1c
permutation s2a s2b s2c s2d s2e s1a s1b s2f s1c
permutation s2a s2b s2c s2d s2e s1a s2f s1b s1c
permutation s2a s2b s2c s2d s2e s2f s1a s1b s1c
