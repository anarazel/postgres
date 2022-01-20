executable('vacuumlo',
  ['vacuumlo.c'],
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args,
)

tap_tests += {
  'name' : 'vacuumlo',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests' :[
    't/001_basic.pl',
  ]
}
