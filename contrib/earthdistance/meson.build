earthdistance = shared_module('earthdistance',
  files('earthdistance.c'),
  kwargs: contrib_mod_args,
)

install_data(
  'earthdistance.control',
  'earthdistance--1.0--1.1.sql',
  'earthdistance--1.1.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'earthdistance',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'earthdistance'
  ],
}
