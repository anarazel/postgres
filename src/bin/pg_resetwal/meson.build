executable('pg_resetwal',
  files('pg_resetwal.c'),
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)

tap_tests += {
  'name': 'pg_resetwal',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests': [
    't/001_basic.pl',
    't/002_corrupted.pl'
  ]
}
