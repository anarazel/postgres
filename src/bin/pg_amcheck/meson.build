pg_amcheck_sources = files(
  'pg_amcheck.c'
)

pg_amcheck = executable('pg_amcheck',
  pg_amcheck_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args,
)

tap_tests += {
  'name': 'pg_amcheck',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests': [
    't/001_basic.pl',
    't/002_nonesuch.pl',
    't/003_check.pl',
    't/004_verify_heapam.pl',
    't/005_opclass_damage.pl',
  ]
}
