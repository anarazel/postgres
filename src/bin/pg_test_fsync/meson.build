executable('pg_test_fsync',
  ['pg_test_fsync.c'],
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)

tap_tests += {
  'name' : 'pg_test_fsync',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests' :[
    't/001_basic.pl',
  ]
}
