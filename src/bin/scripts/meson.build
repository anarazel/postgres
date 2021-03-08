
scripts_common = static_library('scripts_common',
  files('common.c'),
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code, libpq],
  install: false,
)

binaries = [
  'createdb',
  'dropdb',
  'createuser',
  'dropuser',
  'clusterdb',
  'vacuumdb',
  'reindexdb',
  'pg_isready',
]

foreach binary : binaries
  executable(binary,
    files(binary + '.c'),
    link_with: [pgport_static, common_static, fe_utils, scripts_common],
    dependencies: [frontend_code, libpq],
    install: true,
  )
endforeach

tap_tests += {
  'sd': meson.current_source_dir(),
  'name': 'scripts',
  'tests': [
    't/010_clusterdb.pl',
    't/011_clusterdb_all.pl',
    't/020_createdb.pl',
    't/040_createuser.pl',
    't/050_dropdb.pl',
    't/070_dropuser.pl',
    't/080_pg_isready.pl',
    't/090_reindexdb.pl',
    't/091_reindexdb_all.pl',
    't/100_vacuumdb.pl',
    't/101_vacuumdb_all.pl',
    't/102_vacuumdb_stages.pl',
    't/200_connstr.pl',
  ]
}
