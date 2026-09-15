# Make sure that FKs referencing partitioned tables actually work.
setup {
  drop table if exists fp2_ppk, fp2_pfk, fp2_pfk1;
  create table fp2_ppk (a int primary key) partition by list (a);
  create table fp2_ppk1 partition of fp2_ppk for values in (1);
  insert into fp2_ppk values (1);
  create table fp2_pfk (a int references fp2_ppk) partition by list (a);
  create table fp2_pfk1 partition of fp2_pfk for values in (1);
}

session s1
step s1b	{	begin; }
step s1d	{	delete from fp2_ppk where a = 1; }
step s1c	{	commit; }

session s2
step s2b	{	begin; }
step s2bs	{	begin isolation level serializable; select 1; }
step s2i	{	insert into fp2_pfk values (1); }
step s2c	{	commit; }

teardown	{	drop table fp2_ppk, fp2_pfk, fp2_pfk1; }

permutation s1b s1d  s2b  s2i s1c s2c
permutation s1b s1d  s2bs s2i s1c s2c
permutation s1b s2b  s1d  s2i s1c s2c
permutation s1b s2bs s1d  s2i s1c s2c
permutation s1b s2b  s2i  s1d s2c s1c
permutation s1b s2bs s2i  s1d s2c s1c
