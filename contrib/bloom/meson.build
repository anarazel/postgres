bloom_sources = files(
  'blcost.c',
  'blinsert.c',
  'blscan.c',
  'blutils.c',
  'blvacuum.c',
  'blvalidate.c',
)

bloom = shared_module('bloom',
  bloom_sources,
  dependencies: [backend_code],
  c_args: [],
  link_with: [postgres],
  name_prefix: '',
  install: true)

install_data(
  'bloom.control',
  'bloom--1.0.sql',
  install_dir: get_option('datadir') / 'extension'
)

regress_tests += {
  'name': 'bloom',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'bloom'
  ],
}
