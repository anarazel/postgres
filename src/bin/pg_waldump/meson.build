pg_waldump_sources = files(
  'compat.c',
  'pg_waldump.c',
  'rmgrdesc.c',
)

pg_waldump_sources += rmgr_desc_sources
pg_waldump_sources += xlogreader_sources

pg_waldump = executable('pg_waldump',
  pg_waldump_sources,
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code],
  install: true
)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_waldump',
  'tests' :[
    't/001_basic.pl',
  ]
}
