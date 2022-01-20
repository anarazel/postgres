tsm_system_rows = shared_module('tsm_system_rows',
  files(
    'tsm_system_rows.c',
  ),
  c_pch: '../../src/include/pch/postgres_pch.h',
  kwargs: contrib_mod_args,
)

install_data(
  'tsm_system_rows--1.0.sql',
  'tsm_system_rows.control',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'tsm_system_rows',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'tsm_system_rows',
  ],
}
