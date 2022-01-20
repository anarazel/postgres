tcn = shared_module('tcn',
  files(
    'tcn.c',
  ),
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)

install_data(
  'tcn--1.0.sql',
  'tcn.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'tcn',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'isolation': {
    'specs': [
      'tcn',
    ],
  },
}
