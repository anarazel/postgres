# Copyright (c) 2026, PostgreSQL Global Development Group

deform_bench_sources = files(
  'deform_bench.c',
)

if host_system == 'windows'
  deform_bench_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'deform_bench',
    '--FILEDESC', 'deform_bench - benchmarking tuple deformation',])
endif

deform_bench = shared_module('deform_bench',
  deform_bench_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += deform_bench

test_install_data += files(
  'deform_bench--1.0.sql',
  'deform_bench.control',
)
