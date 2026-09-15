# Verify that cloning a foreign key constraint to a partition ensures
# that referenced values exist, even if they're being concurrently
# deleted.
setup {
drop table if exists fp1_ppk, fp1_pfk, fp1_pfk1;
  create table fp1_ppk (a int primary key) partition by list (a);
  create table fp1_ppk1 partition of fp1_ppk for values in (1);
  insert into fp1_ppk values (1);
  create table fp1_pfk (a int references fp1_ppk) partition by list (a);
  create table fp1_pfk1 (a int not null);
  insert into fp1_pfk1 values (1);
}

session s1
step s1b	{	begin; }
step s1d	{	delete from fp1_ppk1 where a = 1; }
step s1c	{	commit; }

session s2
step s2b	{	begin; }
step s2a	{	alter table fp1_pfk attach partition fp1_pfk1 for values in (1); }
step s2c	{	commit; }

teardown	{	drop table fp1_ppk, fp1_pfk, fp1_pfk1; }

permutation s1b s1d s1c s2b s2a s2c
permutation s1b s1d s2b s1c s2a s2c
permutation s1b s1d s2b s2a s1c s2c
#permutation s1b s1d s2b s2a s2c s1c
permutation s1b s2b s1d s1c s2a s2c
permutation s1b s2b s1d s2a s1c s2c
#permutation s1b s2b s1d s2a s2c s1c
#permutation s1b s2b s2a s1d s1c s2c
permutation s1b s2b s2a s1d s2c s1c
permutation s1b s2b s2a s2c s1d s1c
permutation s2b s1b s1d s1c s2a s2c
permutation s2b s1b s1d s2a s1c s2c
#permutation s2b s1b s1d s2a s2c s1c
#permutation s2b s1b s2a s1d s1c s2c
permutation s2b s1b s2a s1d s2c s1c
permutation s2b s1b s2a s2c s1d s1c
#permutation s2b s2a s1b s1d s1c s2c
permutation s2b s2a s1b s1d s2c s1c
permutation s2b s2a s1b s2c s1d s1c
permutation s2b s2a s2c s1b s1d s1c
