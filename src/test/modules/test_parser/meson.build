# FIXME: prevent install during main install, but not during test :/
test_parser = shared_module('test_parser',
  ['test_parser.c'],
  kwargs: pg_mod_args,
)

install_data(
  'test_parser.control',
  'test_parser--1.0.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'test_parser',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'test_parser',
  ]
}
