executable('pg_config',
  ['pg_config.c'],
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)

tap_tests += {
  'name': 'pg_config',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests': [
    't/001_pg_config.pl',
  ]
}
