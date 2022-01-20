tablefunc = shared_module('tablefunc',
  files(
    'tablefunc.c',
  ),
  c_pch: '../../src/include/pch/postgres_pch.h',
  kwargs: contrib_mod_args,
)

install_data(
  'tablefunc--1.0.sql',
  'tablefunc.control',
  kwargs: contrib_data_args,
)


regress_tests += {
  'name': 'tablefunc',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'tablefunc'
  ],
}
