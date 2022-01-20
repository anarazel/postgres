if not ssl.found()
  subdir_done()
endif

sslinfo = shared_module('sslinfo',
  files(
    'sslinfo.c',
  ),
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args + {
    'dependencies': [ssl, pg_mod_args['dependencies']],
  }
)

install_data(
  'sslinfo--1.0--1.1.sql',
  'sslinfo--1.1--1.2.sql',
  'sslinfo--1.2.sql',
  'sslinfo.control',
  kwargs: contrib_data_args,
)
