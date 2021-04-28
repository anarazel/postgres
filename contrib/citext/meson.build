citext_sources = files(
  'citext.c',
)

citext = shared_module('citext',
  citext_sources,
  dependencies: [backend_code],
  c_args: [],
  link_with: [postgres],
  name_prefix: '',
  install: true)

install_data(
  'citext.control',
  'citext--1.0--1.1.sql',
  'citext--1.1--1.2.sql',
  'citext--1.2--1.3.sql',
  'citext--1.3--1.4.sql',
  'citext--1.4.sql',
  'citext--1.4--1.5.sql',
  'citext--1.5--1.6.sql',
  install_dir: get_option('datadir') / 'extension'
)

regress_tests += {
  'name': 'citext',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'citext'
  ],
}
