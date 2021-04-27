pg_amcheck_sources = files(
  'pg_amcheck.c'
)

pg_amcheck = executable('pg_amcheck',
  pg_amcheck_sources,
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code, libpq],
  install: true
)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_amcheck',
  'tests' :[
    't/001_basic.pl',
    't/003_check.pl',
    't/005_opclass_damage.pl',
    't/002_nonesuch.pl',
    't/004_verify_heapam.pl',
  ]
}
