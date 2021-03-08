pg_dump_common_sources = files(
  'compress_io.c',
  'dumputils.c',
  'parallel.c',
  'pg_backup_archiver.c',
  'pg_backup_custom.c',
  'pg_backup_db.c',
  'pg_backup_directory.c',
  'pg_backup_null.c',
  'pg_backup_tar.c',
  'pg_backup_utils.c',
)

pg_dump_common = static_library('pg_dump_common',
  pg_dump_common_sources,
  dependencies: [frontend_code, libpq, zlib],
  install: false)


pg_dump_sources = files(
  'pg_dump.c',
  'common.c',
  'pg_dump_sort.c',
)

executable('pg_dump',
  pg_dump_sources,
  link_with: [pgport_static, common_static, fe_utils, pg_dump_common],
  dependencies: [frontend_code, libpq, rt_dep, zlib],
  install: true)


pg_dumpall_sources = files(
  'pg_dumpall.c',
)

executable('pg_dumpall',
  pg_dumpall_sources,
  link_with: [pgport_static, common_static, fe_utils, pg_dump_common],
  dependencies: [frontend_code, libpq, rt_dep, zlib],
  install: true)


pg_restore_sources = files(
  'pg_restore.c',
)

executable('pg_restore',
  pg_restore_sources,
  link_with: [pgport_static, common_static, fe_utils, pg_dump_common],
  dependencies: [frontend_code, libpq, rt_dep, zlib],
  install: true)


tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'pg_basebackup',
  'tests' :['t/001_basic.pl', 't/002_pg_dump.pl', 't/003_pg_dump_with_server.pl', 't/010_dump_connstr.pl']}
