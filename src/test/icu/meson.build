tap_tests += {
  'name': 'icu',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests': [
    't/010_database.pl',
  ],
  'env' : {'with_icu': icu.found() ? 'yes' : 'no'},
}
