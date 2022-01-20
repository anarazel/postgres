libpq_pipeline = executable('libpq_pipeline',
  files(
    'libpq_pipeline.c',
  ),
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  },
)

tap_tests += {
  'name': 'libpq_pipeline',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests': [
    't/001_libpq_pipeline.pl',
  ]
}
