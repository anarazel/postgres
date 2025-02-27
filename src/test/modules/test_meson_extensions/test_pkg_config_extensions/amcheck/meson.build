# Copyright (c) 2022-2025, PostgreSQL Global Development Group

project('amcheck', 'c')

amcheck_path = '../../../../../../contrib/amcheck/'

amcheck_sources = files(
  amcheck_path / 'verify_heapam.c',
  amcheck_path / 'verify_nbtree.c',
)

pg_ext = dependency('postgresql-extension-warnings')

amcheck = shared_module('amcheck',
  amcheck_sources,
  dependencies: pg_ext,
  install_dir: pg_ext.get_variable(pkgconfig: 'dir_mod'),
)

install_data(
  amcheck_path / 'amcheck.control',
  amcheck_path / 'amcheck--1.0.sql',
  amcheck_path / 'amcheck--1.0--1.1.sql',
  amcheck_path / 'amcheck--1.1--1.2.sql',
  amcheck_path / 'amcheck--1.2--1.3.sql',
  amcheck_path / 'amcheck--1.3--1.4.sql',
  install_dir: pg_ext.get_variable(pkgconfig: 'dir_data'),
)
