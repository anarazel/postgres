executable('pg_resetwal',
  files('pg_resetwal.c'),
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code],
  install: true)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_resetwal',
  'tests' : ['t/001_basic.pl', 't/002_corrupted.pl']}
