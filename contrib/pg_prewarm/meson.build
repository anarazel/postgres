pg_prewarm = shared_module('pg_prewarm',
  files(
    'autoprewarm.c',
    'pg_prewarm.c',
  ),
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)

install_data(
  'pg_prewarm--1.0--1.1.sql',
  'pg_prewarm--1.1--1.2.sql',
  'pg_prewarm--1.1.sql',
  'pg_prewarm.control',
  kwargs: contrib_data_args,
)
