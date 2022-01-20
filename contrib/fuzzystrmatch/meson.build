fuzzystrmatch = shared_module('fuzzystrmatch',
  files(
    'fuzzystrmatch.c',
    'dmetaphone.c'
  ),
  kwargs: contrib_mod_args,
)

install_data(
  'fuzzystrmatch.control',
  'fuzzystrmatch--1.0--1.1.sql',
  'fuzzystrmatch--1.1.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'fuzzystrmatch',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'fuzzystrmatch'
  ],
}
