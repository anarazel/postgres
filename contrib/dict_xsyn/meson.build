dict_xsyn = shared_module('dict_xsyn',
  files('dict_xsyn.c'),
  dependencies: [backend_code],
  c_args: [],
  link_with: [postgres],
  name_prefix: '',
  install: true)

install_data(
  'dict_xsyn.control',
  'dict_xsyn--1.0.sql',
  install_dir: get_option('datadir') / 'extension'
)

install_data(
  'xsyn_sample.rules',
  install_dir: get_option('datadir') / 'tsearch_data'
)

regress_tests += {
  'name': 'dict_xsyn',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'dict_xsyn'
  ],
}
