auto_explain = shared_module('auto_explain',
  files('auto_explain.c'),
  kwargs: contrib_mod_args,
)

tap_tests += {
  'name': 'auto_explain',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests': [
    't/001_auto_explain.pl',
  ]
}
