# FIXME: prevent install during main install, but not during test :/
dummy_seclabel = shared_module('dummy_seclabel',
  ['dummy_seclabel.c'],
  kwargs: pg_mod_args,
)

install_data(
  'dummy_seclabel.control',
  'dummy_seclabel--1.0.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'dummy_seclabel',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'dummy_seclabel',
  ]
}
