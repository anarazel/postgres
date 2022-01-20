if not python3_dep.found()
  subdir_done()
endif

jsonb_plpython_sources = files(
  'jsonb_plpython.c',
)

jsonb_plpython = shared_module('jsonb_plpython3',
  jsonb_plpython_sources,
  include_directories: [plpython_inc],
  kwargs: pg_mod_args + {
    'c_args': ['-DPLPYTHON_LIBNAME="plpython3"'] + contrib_mod_args['c_args'],
    'dependencies': [python3_dep, contrib_mod_args['dependencies']],
  },
)

install_data(
  'jsonb_plpython3u.control',
  'jsonb_plpython3u--1.0.sql',
  kwargs: contrib_data_args,
)

jsonb_plpython_regress = [
  'jsonb_plpython'
]

regress_tests += {
  'name': 'jsonb_plpython',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': jsonb_plpython_regress,
}
