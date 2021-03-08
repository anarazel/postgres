pg_verifybackup_sources = files(
  'parse_manifest.c',
  'pg_verifybackup.c'
)

pg_verifybackup = executable('pg_verifybackup',
  pg_verifybackup_sources,
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code, libpq],
  install: true
)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_verifybackup',
  'tests' :[
    't/001_basic.pl',
    't/002_algorithm.pl',
    't/003_corruption.pl',
    't/004_options.pl',
    't/005_bad_manifest.pl',
    't/006_encoding.pl',
    't/007_wal.pl',
  ]
}
