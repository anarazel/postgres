setup
{
  CREATE TABLE fc_foo (a int PRIMARY KEY, b text);
  CREATE TABLE fc_bar (a int NOT NULL REFERENCES fc_foo);
  INSERT INTO fc_foo VALUES (42);
}

teardown
{
  DROP TABLE fc_foo, fc_bar;
}

session s1
setup		{ BEGIN; }
step ins	{ INSERT INTO fc_bar VALUES (42); }
step com	{ COMMIT; }

session s2
step upd	{ UPDATE fc_foo SET b = 'Hello World'; }
