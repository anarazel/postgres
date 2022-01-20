pg_rewind_sources = files(
  'datapagemap.c',
  'file_ops.c',
  'filemap.c',
  'libpq_source.c',
  'local_source.c',
  'parsexlog.c',
  'pg_rewind.c',
  'timeline.c',
  '../../backend/access/transam/xlogreader.c',
)

pg_rewind = executable('pg_rewind',
  pg_rewind_sources,
  dependencies: [frontend_code, libpq, lz4, zstd],
  kwargs: default_bin_args,
)


tap_tests += {
  'name': 'pg_rewind',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests': [
    't/001_basic.pl',
    't/002_databases.pl',
    't/003_extrafiles.pl',
    't/004_pg_xlog_symlink.pl',
    't/005_same_timeline.pl',
    't/006_options.pl',
    't/007_standby_source.pl',
    't/008_min_recovery_point.pl',
  ]
}
