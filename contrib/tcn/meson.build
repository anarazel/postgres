tcn = shared_module('tcn',
  files(
    'tcn.c',
  ),
  c_pch: '../../src/include/pch/postgres_pch.h',
  kwargs: contrib_mod_args,
)

install_data(
  'tcn--1.0.sql',
  'tcn.control',
  kwargs: contrib_data_args,
)
