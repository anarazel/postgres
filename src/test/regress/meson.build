# also used by isolationtester and ecpg tests
pg_regress_c = files('pg_regress.c')
pg_regress_inc = include_directories('.')

regress_sources = pg_regress_c + files(
  'pg_regress_main.c'
)

pg_regress_cflags = ['-DHOST_TUPLE="frak"', '-DSHELLPROG="/bin/sh"']

pg_regress = executable('pg_regress',
  regress_sources,
  c_args: pg_regress_cflags,
  dependencies: [frontend_code],
  kwargs: default_bin_args + {
    'install_dir': dir_pgxs / 'src/test/regress',
  },
)

regress_module = shared_module('regress',
  ['regress.c'],
  kwargs: pg_mod_args + {
    'install': false,
  },
)

# Get some extra C modules from contrib/spi but mark them as not to be
# installed.
# FIXME: avoid the duplication.

shared_module('autoinc',
  ['../../../contrib/spi/autoinc.c'],
  kwargs: pg_mod_args + {
    'install': false,
  },
)

shared_module('refint',
  ['../../../contrib/spi/refint.c'],
  kwargs: pg_mod_args + {
    'c_args': refint_cflags + contrib_mod_args['c_args'],
    'install': false,
  },
)


tests += {
  'name': 'main',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'schedule': files('parallel_schedule'),
    'test_kwargs': {
      'priority': 50,
      'timeout': 1000,
    },
  },
}
