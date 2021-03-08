executable('pg_checksums',
  ['pg_checksums.c'],
  include_directories: [timezone_inc],
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code, rt_dep],
  install: true)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_checksums',
  'tests' :['t/001_basic.pl', 't/002_actions.pl']}
