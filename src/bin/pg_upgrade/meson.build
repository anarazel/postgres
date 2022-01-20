pg_upgrade_sources = files(
  'check.c',
  'controldata.c',
  'dump.c',
  'exec.c',
  'file.c',
  'function.c',
  'info.c',
  'option.c',
  'parallel.c',
  'pg_upgrade.c',
  'relfilenode.c',
  'server.c',
  'tablespace.c',
  'util.c',
  'version.c',
)

pg_upgrade = executable('pg_upgrade',
  pg_upgrade_sources,
  c_pch: '../../include/pch/c_pch.h',
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args,
)

# FIXME: add test
