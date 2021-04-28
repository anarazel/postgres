amcheck = shared_module('amcheck',
  ['verify_heapam.c', 'verify_nbtree.c'],
  dependencies: [backend_code],
  c_args: [],
  name_prefix: '',
  install: true)

install_data(
  'amcheck.control',
  'amcheck--1.0.sql',
  'amcheck--1.0--1.1.sql',
  'amcheck--1.1--1.2.sql',
  'amcheck--1.2--1.3.sql',
  install_dir: get_option('datadir') / 'extension'
)

regress_tests += {
  'name': 'amcheck',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': ['check', 'check_btree', 'check_heap'],
}

tap_tests += {
  'name': 'amcheck',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests': [
    't/001_verify_heapam.pl',
  ],
}
