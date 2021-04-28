# also used by isolationtester
pg_regress_c = files('pg_regress.c')
pg_regress_inc = include_directories('.')

regress_sources = pg_regress_c + files(
  'pg_regress_main.c'
)

pg_regress_cflags = ['-DHOST_TUPLE="frak"', '-DDLSUFFIX=".so"', '-DSHELLPROG="/bin/sh"']

pg_regress = executable('pg_regress',
  regress_sources,
  c_args: pg_regress_cflags,
  link_with: [pgport_static, common_static, fe_utils],
  dependencies: [frontend_code, libpq, rt_dep],
  install: false)

regress_module = shared_module('regress',
  ['regress.c'],
  dependencies: [backend_code],
  name_prefix: '',
  c_args: [],
  install: false)

# Get some extra C modules from contrib/spi
# but mark them as not to be installed
shared_module('autoinc',
  ['../../../contrib/spi/autoinc.c'],
  dependencies: [backend_code, libpq],
  c_args: [],
  name_prefix: '',
  install: false)

shared_module('refint',
  ['../../../contrib/spi/refint.c'],
  dependencies: [backend_code, libpq],
  c_args: refint_cflags,
  name_prefix: '',
  install: false)


regress_tests += {
  'name': 'main',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'schedule': files('parallel_schedule'),
  'test_kwargs': {
    'priority': 50,
    'timeout': 1000,
  },
}
