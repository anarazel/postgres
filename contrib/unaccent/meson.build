unaccent = shared_module('unaccent',
  files(
    'unaccent.c',
  ),
  c_pch: '../../src/include/pch/postgres_pch.h',
  kwargs: contrib_mod_args,
)

install_data(
  'unaccent--1.0--1.1.sql',
  'unaccent--1.1.sql',
  'unaccent.control',
  kwargs: contrib_data_args,
)

install_data(
  'unaccent.rules',
   install_dir: get_option('datadir') / 'tsearch_data'
)

# XXX: Implement downlo
regress_tests += {
  'name': 'unaccent',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'unaccent',
  ],
  'regress_args': ['--encoding=UTF8'],
}
