earthdistance = shared_module('earthdistance',
  files('earthdistance.c'),
  dependencies: [backend_code],
  c_args: [],
  link_with: [postgres],
  name_prefix: '',
  install: true)

install_data(
  'earthdistance.control',
  'earthdistance--1.0--1.1.sql',
  'earthdistance--1.1.sql',
  install_dir: get_option('datadir') / 'extension'
)

regress_tests += {
  'name': 'earthdistance',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'earthdistance'
  ],
}
