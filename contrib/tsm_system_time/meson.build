tsm_system_time = shared_module('tsm_system_time',
  files(
    'tsm_system_time.c',
  ),
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)

install_data(
  'tsm_system_time--1.0.sql',
  'tsm_system_time.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'tsm_system_time',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'tsm_system_time',
    ],
  },
}
