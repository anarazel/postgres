setup
{
  CREATE TABLE fd2_a (
	AID integer not null,
	Col1 integer,
	PRIMARY KEY (AID)
  );

  CREATE TABLE fd2_b (
	BID integer not null,
	AID integer not null,
	Col2 integer,
	PRIMARY KEY (BID),
	FOREIGN KEY (AID) REFERENCES fd2_a(AID)
  );

  INSERT INTO fd2_a (AID) VALUES (1);
  INSERT INTO fd2_b (BID,AID) VALUES (2,1);
}

teardown
{
  DROP TABLE fd2_a, fd2_b;
}

session s1
setup		{ BEGIN; SET deadlock_timeout = '100ms'; }
step s1u1	{ UPDATE fd2_a SET Col1 = 1 WHERE AID = 1; }
step s1u2	{ UPDATE fd2_b SET Col2 = 1 WHERE BID = 2; }
step s1c	{ COMMIT; }

session s2
setup		{ BEGIN; SET deadlock_timeout = '10s'; }
step s2u1	{ UPDATE fd2_b SET Col2 = 1 WHERE BID = 2; }
step s2u2	{ UPDATE fd2_b SET Col2 = 1 WHERE BID = 2; }
step s2c	{ COMMIT; }

permutation s1u1 s1u2 s1c s2u1 s2u2 s2c
permutation s1u1 s1u2 s2u1 s1c s2u2 s2c
permutation s1u1 s2u1 s1u2 s2u2 s2c s1c
permutation s1u1 s2u1 s2u2 s1u2 s2c s1c
permutation s1u1 s2u1 s2u2 s2c s1u2 s1c
permutation s2u1 s1u1 s1u2 s2u2 s2c s1c
permutation s2u1 s1u1 s2u2 s1u2 s2c s1c
permutation s2u1 s1u1 s2u2 s2c s1u2 s1c
permutation s2u1 s2u2 s1u1 s1u2 s2c s1c
permutation s2u1 s2u2 s1u1 s2c s1u2 s1c
permutation s2u1 s2u2 s2c s1u1 s1u2 s1c
