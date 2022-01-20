dict_xsyn = shared_module('dict_xsyn',
  files('dict_xsyn.c'),
  kwargs: contrib_mod_args,
)

install_data(
  'dict_xsyn.control',
  'dict_xsyn--1.0.sql',
  kwargs: contrib_data_args,
)

install_data(
  'xsyn_sample.rules',
  kwargs: contrib_data_args + {
    'install_dir': get_option('datadir') / 'tsearch_data'
  }
)

regress_tests += {
  'name': 'dict_xsyn',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'dict_xsyn'
  ],
}
