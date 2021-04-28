dblink_sources = files(
  'dblink.c',
)

dblink = shared_module('dblink',
  dblink_sources,
  dependencies: [backend_code, libpq],
  c_args: [],
  link_with: [postgres],
  name_prefix: '',
  install: true)

install_data(
  'dblink.control',
  'dblink--1.0--1.1.sql',
  'dblink--1.1--1.2.sql',
  'dblink--1.2.sql',
  install_dir: get_option('datadir') / 'extension'
)

regress_tests += {
  'name': 'dblink',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'paths',
    'dblink'
  ],
  'regress_args': ['--dlpath', meson.build_root() / 'src/test/regress'],
}
