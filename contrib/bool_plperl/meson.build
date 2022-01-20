if not perl_dep.found()
  subdir_done()
endif

bool_plperl_sources = files(
  'bool_plperl.c',
)

bool_plperl = shared_module('bool_plperl',
  bool_plperl_sources,
  include_directories: [plperl_inc, include_directories('.')],
  kwargs: pg_mod_args + {
    'dependencies': [perl_dep, contrib_mod_args['dependencies']],
  },
)

install_data(
  'bool_plperl.control',
  'bool_plperl--1.0.sql',
  kwargs: contrib_data_args,
)

install_data(
  'bool_plperlu.control',
  'bool_plperlu--1.0.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'bool_plperl',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'bool_plperl',
    'bool_plperlu',
  ],
}
