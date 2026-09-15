# Total Cash test
#
# Another famous test of snapshot isolation anomaly.
#
# Any overlap between the transactions must cause a serialization failure.

setup
{
 CREATE TABLE tc_accounts (accountid text NOT NULL PRIMARY KEY, balance numeric not null);
 INSERT INTO tc_accounts VALUES ('checking', 600),('savings',600);
}

teardown
{
 DROP TABLE tc_accounts;
}

session s1
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step wx1	{ UPDATE tc_accounts SET balance = balance - 200 WHERE accountid = 'checking'; }
step rxy1	{ SELECT SUM(balance) FROM tc_accounts; }
step c1		{ COMMIT; }

session s2
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step wy2	{ UPDATE tc_accounts SET balance = balance - 200 WHERE accountid = 'savings'; }
step rxy2	{ SELECT SUM(balance) FROM tc_accounts; }
step c2		{ COMMIT; }
