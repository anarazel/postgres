pg_buffercache = shared_module('pg_buffercache',
  files(
    'pg_buffercache_pages.c',
  ),
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)

install_data(
  'pg_buffercache--1.0--1.1.sql',
  'pg_buffercache--1.1--1.2.sql',
  'pg_buffercache--1.2--1.3.sql',
  'pg_buffercache--1.2.sql',
  'pg_buffercache.control',
  kwargs: contrib_data_args,
)
