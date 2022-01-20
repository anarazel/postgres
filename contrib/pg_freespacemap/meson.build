pg_freespacemap = shared_module('pg_freespacemap',
  files(
    'pg_freespacemap.c',
  ),
  c_pch: '../../src/include/pch/postgres_pch.h',
  kwargs: contrib_mod_args,
)

install_data(
  'pg_freespacemap--1.0--1.1.sql',
  'pg_freespacemap--1.1--1.2.sql',
  'pg_freespacemap--1.1.sql',
  'pg_freespacemap.control',
  kwargs: contrib_data_args,
)
