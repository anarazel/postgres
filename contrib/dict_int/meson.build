dict_int = shared_module('dict_int',
  files('dict_int.c'),
  dependencies: [backend_code],
  c_args: [],
  link_with: [postgres],
  name_prefix: '',
  install: true)

install_data(
  'dict_int.control',
  'dict_int--1.0.sql',
  install_dir: get_option('datadir') / 'extension'
)

regress_tests += {
  'name': 'dict_int',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'dict_int'
  ],
}
