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
  c_pch: '../../src/include/pch/c_pch.h',
  kwargs: contrib_mod_args,
)

install_data(
  'bloom.control',
  'bloom--1.0.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'bloom',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'bloom'
  ],
}

tap_tests += {
  'name': 'bloom',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests': [
    't/001_wal.pl',
  ],
}
