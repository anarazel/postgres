if not perl_dep.found()
  subdir_done()
endif

jsonb_plperl_sources = files(
  'jsonb_plperl.c',
)

jsonb_plperl = shared_module('jsonb_plperl',
  jsonb_plperl_sources,
  include_directories: [plperl_inc],
  kwargs: pg_mod_args + {
    'dependencies': [perl_dep, contrib_mod_args['dependencies']],
  },
)

install_data(
  'jsonb_plperl.control',
  'jsonb_plperl--1.0.sql',
  kwargs: contrib_data_args,
)

install_data(
  'jsonb_plperlu.control',
  'jsonb_plperlu--1.0.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'jsonb_plperl',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'jsonb_plperl',
    'jsonb_plperlu',
  ],
}
