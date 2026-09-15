# A funkier version of delete-abort-savept
setup
{
  CREATE TABLE das2_foo (
     key INT PRIMARY KEY,
     value INT
  );

  INSERT INTO das2_foo VALUES (1, 1);
}

teardown
{
  DROP TABLE das2_foo;
}

session s1
setup			{ BEGIN; }
step s1l		{ SELECT * FROM das2_foo FOR KEY SHARE; }
step s1svp		{ SAVEPOINT f; }
step s1d		{ SELECT * FROM das2_foo FOR NO KEY UPDATE; }
step s1r		{ ROLLBACK TO f; }
step s1c		{ COMMIT; }

session s2
setup			{ BEGIN; }
step s2l		{ SELECT * FROM das2_foo FOR UPDATE; }
step s2l2		{ SELECT * FROM das2_foo FOR NO KEY UPDATE; }
step s2c		{ COMMIT; }

permutation s1l s1svp s1d s1r s2l s1c s2c
permutation s1l s1svp s1d s2l s1r s1c s2c
permutation s1l s1svp s1d s1r s2l2 s1c s2c
permutation s1l s1svp s1d s2l2 s1r s1c s2c
