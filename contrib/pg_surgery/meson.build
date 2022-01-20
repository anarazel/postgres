pg_surgery = shared_module('pg_surgery',
  files(
    'heap_surgery.c',
  ),
  c_pch: '../../src/include/pch/postgres_pch.h',
  kwargs: contrib_mod_args,
)

install_data(
  'pg_surgery--1.0.sql',
  'pg_surgery.control',
  kwargs: contrib_data_args,
)


regress_tests += {
  'name': 'pg_surgery',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'heap_surgery'
  ],
}
