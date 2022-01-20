pg_trgm = shared_module('pg_trgm',
  files(
    'trgm_gin.c',
    'trgm_gist.c',
    'trgm_op.c',
    'trgm_regexp.c',
  ),
  c_pch: '../../src/include/pch/postgres_pch.h',
  kwargs: contrib_mod_args,
)

install_data(
  'pg_trgm--1.0--1.1.sql',
  'pg_trgm--1.1--1.2.sql',
  'pg_trgm--1.2--1.3.sql',
  'pg_trgm--1.3--1.4.sql',
  'pg_trgm--1.3.sql',
  'pg_trgm--1.4--1.5.sql',
  'pg_trgm--1.5--1.6.sql',
  'pg_trgm.control',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'pg_trgm',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'pg_trgm',
    'pg_word_trgm',
    'pg_strict_word_trgm',
  ],
}
