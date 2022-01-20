pg_visibility = shared_module('pg_visibility',
  files(
    'pg_visibility.c',
  ),
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)

install_data(
  'pg_visibility--1.0--1.1.sql',
  'pg_visibility--1.1--1.2.sql',
  'pg_visibility--1.1.sql',
  'pg_visibility.control',
  kwargs: contrib_data_args,
)


tests += {
  'name': 'pg_visibility',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_visibility',
    ],
  },
}
