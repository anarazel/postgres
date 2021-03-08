executable('pg_config',
  ['pg_config.c'],
  link_with: [pgport_static, common_static],
  dependencies: [frontend_code],
  install: true)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_config',
  'tests' :['t/001_pg_config.pl']}
