executable('pg_controldata',
  ['pg_controldata.c'],
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code],
  install: true)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_controldata',
  'tests' :['t/001_pg_controldata.pl']}
