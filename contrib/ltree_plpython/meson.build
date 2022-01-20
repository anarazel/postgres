if not python3_dep.found()
  subdir_done()
endif

ltree_plpython_sources = files(
  'ltree_plpython.c',
)

ltree_plpython = shared_module('ltree_plpython3',
  ltree_plpython_sources,
  include_directories: [plpython_inc, ltree_inc],
  kwargs: pg_mod_args + {
    'c_args': ['-DPLPYTHON_LIBNAME="plpython3"'] + contrib_mod_args['c_args'],
    'dependencies': [python3_dep, contrib_mod_args['dependencies']],
  },
)

install_data(
  'ltree_plpython3u--1.0.sql',
  'ltree_plpython3u.control',
  kwargs: contrib_data_args,
)

ltree_plpython_regress = [
  'ltree_plpython'
]

regress_tests += {
  'name': 'ltree_plpython',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': ltree_plpython_regress,
  'regress_args': ['--load-extension=ltree'],
}
