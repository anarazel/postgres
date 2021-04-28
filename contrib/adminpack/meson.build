autoinc = shared_module('adminpack',
  ['adminpack.c'],
  dependencies: [backend_code],
  c_args: [],
  name_prefix: '',
  install: true)

install_data(
  'adminpack.control',
  'adminpack--1.0.sql',
  'adminpack--1.0--1.1.sql',
  'adminpack--1.1--2.0.sql',
  'adminpack--2.0--2.1.sql',
  install_dir: get_option('datadir') / 'extension'
)

regress_tests += {
  'name': 'adminpack',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': ['adminpack'],
}
