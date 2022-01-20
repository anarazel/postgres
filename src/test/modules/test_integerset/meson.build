# FIXME: prevent install during main install, but not during test :/
test_integerset = shared_module('test_integerset',
  ['test_integerset.c'],
  kwargs: pg_mod_args,
)

install_data(
  'test_integerset.control',
  'test_integerset--1.0.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'test_integerset',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'test_integerset',
  ]
}
