# Test NOWAIT with tuple locks.

setup
{
  CREATE TABLE nw3_foo (
	id int PRIMARY KEY,
	data text NOT NULL
  );
  INSERT INTO nw3_foo VALUES (1, 'x');
}

teardown
{
  DROP TABLE nw3_foo;
}

session s1
setup		{ BEGIN; }
step s1a	{ SELECT * FROM nw3_foo FOR UPDATE; }
step s1b	{ COMMIT; }

session s2
setup		{ BEGIN; }
step s2a	{ SELECT * FROM nw3_foo FOR UPDATE; }
step s2b	{ COMMIT; }

session s3
setup		{ BEGIN; }
step s3a	{ SELECT * FROM nw3_foo FOR UPDATE NOWAIT; }
step s3b	{ COMMIT; }

# s3 skips to second record due to tuple lock held by s2
permutation s1a s2a s3a s1b s2b s3b
