# FIXME: prevent install during main install, but not during test :/
test_predtest = shared_module('test_predtest',
  ['test_predtest.c'],
  kwargs: pg_mod_args,
)

install_data(
  'test_predtest.control',
  'test_predtest--1.0.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'test_predtest',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'test_predtest',
  ]
}
