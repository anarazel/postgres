# Copyright (c) 2022-2025, PostgreSQL Global Development Group

project('auth_delay', 'c')

auth_delay_path = '../../../../../../contrib/auth_delay/'

auth_delay_sources = files(
  auth_delay_path / 'auth_delay.c',
)

pg_ext = dependency('postgresql-extension-warnings')

auth_delay = shared_module('auth_delay',
  auth_delay_sources,
  dependencies: pg_ext,
  install_dir: pg_ext.get_variable(pkgconfig: 'dir_mod'),
)
