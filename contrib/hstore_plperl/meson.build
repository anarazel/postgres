if not perl_dep.found()
  subdir_done()
endif

hstore_plperl_sources = files(
  'hstore_plperl.c',
)

hstore_plperl = shared_module('hstore_plperl',
  hstore_plperl_sources,
  include_directories: [plperl_inc, hstore_inc],
  kwargs: pg_mod_args + {
    'dependencies': [perl_dep, contrib_mod_args['dependencies']],
  },
)

install_data(
  'hstore_plperl.control',
  'hstore_plperl--1.0.sql',
  kwargs: contrib_data_args,
)

install_data(
  'hstore_plperlu.control',
  'hstore_plperlu--1.0.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'hstore_plperl',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'hstore_plperl',
    'hstore_plperlu',
    'create_transform',
  ],
}
