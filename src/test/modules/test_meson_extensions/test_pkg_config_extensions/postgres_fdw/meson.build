# Copyright (c) 2022-2025, PostgreSQL Global Development Group

project('auth_delay', 'c')

postgres_fdw_path = '../../../../../../contrib/postgres_fdw/'

postgres_fdw_sources = files(
  postgres_fdw_path / 'connection.c',
  postgres_fdw_path / 'deparse.c',
  postgres_fdw_path / 'option.c',
  postgres_fdw_path / 'postgres_fdw.c',
  postgres_fdw_path / 'shippable.c',
)

pg_ext = dependency('postgresql-extension-warnings')
libpq = dependency('libpq')

postgres_fdw = shared_module('postgres_fdw',
  postgres_fdw_sources,
  dependencies: [pg_ext, libpq],
  install_dir: pg_ext.get_variable(pkgconfig: 'dir_mod'),
)

install_data(
  postgres_fdw_path / 'postgres_fdw.control',
  postgres_fdw_path / 'postgres_fdw--1.0.sql',
  postgres_fdw_path / 'postgres_fdw--1.0--1.1.sql',
  postgres_fdw_path / 'postgres_fdw--1.1--1.2.sql',
  install_dir: pg_ext.get_variable(pkgconfig: 'dir_data'),
)

