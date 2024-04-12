# Copyright (c) 2022-2024, PostgreSQL Global Development Group

backend_sources += files(
  'condition_variable.c',
  'deadlock.c',
  'lmgr.c',
  'lock.c',
  'lwlock.c',
  'predicate.c',
  'proc.c',
  's_lock.c',
  'spin.c',
)

# Test binary for spinlocks. This isn't run as a test as
# a) it takes a while
# b) it doesn't report failures in a usable way
# but it is still useful to be able to build and run it manually.
s_lock_test = executable('s_lock_test',
  files('s_lock.c'),
  dependencies: [backend_code],
  link_with: [common_static, pgport_static],
  kwargs: default_bin_args + {'install': false},
  c_args: ['-DS_LOCK_TEST=1'],
  build_by_default: false,
)
