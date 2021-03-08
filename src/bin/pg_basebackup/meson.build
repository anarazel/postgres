common_sources = files(
  'receivelog.c',
  'streamutil.c',
  'walmethods.c',
)

pg_basebackup_common = static_library('pg_basebackup_common',
  common_sources,
  dependencies: [frontend_code, libpq, zlib],
  install: false)

executable('pg_basebackup',
  'pg_basebackup.c',
  link_with: [pgport_static, common_static, fe_utils, pg_basebackup_common],
  dependencies: [frontend_code, libpq, rt_dep],
  install: true)

executable('pg_receivewal',
  'pg_receivewal.c',
  link_with: [pgport_static, common_static, fe_utils, pg_basebackup_common],
  dependencies: [frontend_code, libpq, rt_dep],
  install: true)

executable('pg_recvlogical',
  'pg_recvlogical.c',
  link_with: [pgport_static, common_static, fe_utils, pg_basebackup_common],
  dependencies: [frontend_code, libpq, rt_dep],
  install: true)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_basebackup',
  'tests' :['t/010_pg_basebackup.pl', 't/020_pg_receivewal.pl', 't/030_pg_recvlogical.pl']}
