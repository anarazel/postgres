# FIXME: prevent install during main install, but not during test :/
test_worker_spi = shared_module('worker_spi',
  files(
    'worker_spi.c',
  ),
  kwargs: pg_mod_args,
)

install_data(
  'worker_spi.control',
  'worker_spi--1.0.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'worker_spi',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'worker_spi',
  ],
  'regress_args': ['--temp-config', files('dynamic.conf'), '--dbname=contrib_regression'],
}
