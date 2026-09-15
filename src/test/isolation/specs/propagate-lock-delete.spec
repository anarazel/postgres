# When an update propagates a preexisting lock on the updated tuple, make sure
# we don't ignore the lock in subsequent operations of the new version.  (The
# version with the aborted savepoint uses a slightly different code path).
setup
{
	create table pld_parent (i int, c char(3));
	create unique index pld_parent_idx on pld_parent (i);
	insert into pld_parent values (1, 'AAA');
	create table pld_child (i int references pld_parent(i));
}

teardown
{
	drop table pld_child, pld_parent;
}

session s1
step s1b	{ BEGIN; }
step s1l	{ INSERT INTO pld_child VALUES (1); }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN; }
step s2l	{ INSERT INTO pld_child VALUES (1); }
step s2c	{ COMMIT; }

session s3
step s3b	{ BEGIN; }
step s3u	{ UPDATE pld_parent SET c=lower(c); }	# no key update
step s3u2	{ UPDATE pld_parent SET i = i; }		# key update
step s3svu	{ SAVEPOINT f; UPDATE pld_parent SET c = 'bbb'; ROLLBACK TO f; }
step s3d	{ DELETE FROM pld_parent; }
step s3c	{ COMMIT; }

permutation s1b s1l s2b s2l s3b s3u          s3d s1c s2c s3c
permutation s1b s1l s2b s2l s3b s3u  s3svu s3d s1c s2c s3c
permutation s1b s1l s2b s2l s3b s3u2         s3d s1c s2c s3c
permutation s1b s1l s2b s2l s3b s3u2 s3svu s3d s1c s2c s3c
permutation s1b s1l             s3b s3u          s3d s1c       s3c
permutation s1b s1l             s3b s3u  s3svu s3d s1c       s3c
permutation s1b s1l             s3b s3u2         s3d s1c       s3c
permutation s1b s1l             s3b s3u2 s3svu s3d s1c       s3c
