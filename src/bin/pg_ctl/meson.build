executable('pg_ctl',
  ['pg_ctl.c'],
  link_with: [pgport_static, common_static],
  dependencies: [frontend_code, libpq, rt_dep],
  install: true)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_ctl',
  'tests' :['t/001_start_stop.pl', 't/002_status.pl', 't/003_promote.pl', 't/004_logrotate.pl']}
