# pg_regress_c helpfully provided by regress/meson.build

isolation_sources = pg_regress_c + files(
  'isolation_main.c',
)

# see src/backend/replication/meson.build for depend logic
spec_scanner = custom_target('specscanner',
  input : files('specscanner.l'),
  output : ['specscanner.c'],
  command : [flex, '-o', '@OUTPUT0@', '@INPUT@']
)

isolationtester_sources = files('isolationtester.c')
isolationtester_sources += custom_target('specparse',
  input: 'specparse.y',
  output: 'specparse.c',
  depends: spec_scanner,
  command: [bison, bisonflags, '-o', '@OUTPUT@', '@INPUT0@'])

pg_isolation_regress = executable('pg_isolation_regress',
  isolation_sources,
  c_args: pg_regress_cflags,
  include_directories: [pg_regress_inc],
  dependencies: [frontend_code],
  kwargs: default_bin_args + {
    'install': false
  },
)

isolationtester = executable('isolationtester',
  isolationtester_sources,
  include_directories: include_directories('.'),
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false
  },
)

isolation_tests += {
  'name': 'main',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'schedule': files('isolation_schedule'),
  'test_kwargs': {
    'priority': 40,
    'timeout': 1000,
  },
}
