pg_archivecleanup = executable('pg_archivecleanup',
  ['pg_archivecleanup.c'],
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code],
  install: true
)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_archivecleanup',
  'tests' :['t/010_pg_archivecleanup.pl']
}
