pg_test_timing = executable('pg_test_timing',
  ['pg_test_timing.c'],
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code],
  install: true
)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_test_timing',
  'tests' :['t/001_basic.pl']
}
