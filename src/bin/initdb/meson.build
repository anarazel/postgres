initdb_sources = files(
  'findtimezone.c',
  'initdb.c'
)

initdb_sources += timezone_localtime_source

#fixme: reimplement libpq_pgport logic

executable('initdb',
  initdb_sources,
  include_directories: [timezone_inc],
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code, libpq, rt_dep],
  install: true)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'initdb',
  'tests' :['t/001_initdb.pl']}
