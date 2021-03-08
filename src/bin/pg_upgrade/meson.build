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
  c_args: ['-DDLSUFFIX=".so"'],
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code, libpq],
  install: true
)
