# pg_regress_c helpfully provided by regress/meson.build

isolation_sources = pg_regress_c + files(
  'isolation_main.c',
)

# see src/backend/replication/meson.build for depend logic
spec_scanner = custom_target('specscanner',
  input : files('specscanner.l'),
  output : ['specscanner.c'],
  command : [flex, '-CF', '-p', '-p', '-o', '@OUTPUT0@', '@INPUT@']
)

isolationtester_sources = files('isolationtester.c')
isolationtester_sources += custom_target('specparse',
  input: 'specparse.y',
  output: 'specparse.c',
  depends: spec_scanner,
  command: [bison, '-Wno-deprecated', '-o', '@OUTPUT@', '@INPUT0@'])

pg_isolation_regress = executable('pg_isolation_regress',
  isolation_sources,
  c_args: pg_regress_cflags,
  include_directories: [pg_regress_inc],
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code],
  install: false
)

isolationtester = executable('isolationtester',
  isolationtester_sources,
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code, libpq],
  install: false
)

test('pg_isolation_regress',
  pg_isolation_regress,
  args: [
    '--temp-instance=./tmp_check_iso',
    '--inputdir', meson.current_source_dir(),
    '--bindir', '',
    '--schedule', files('isolation_schedule'),
    '--dlpath', meson.current_build_dir(),
  ],
  suite: ['isolation'],
  depends: test_deps,
  priority: 99,
  timeout: 1000,
)
