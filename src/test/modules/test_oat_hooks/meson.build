# FIXME: prevent install during main install, but not during test :/
test_oat_hooks = shared_module('test_oat_hooks',
  ['test_oat_hooks.c'],
  kwargs: pg_mod_args,
)

regress_tests += {
  'name': 'test_oat_hooks',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'test_oat_hooks',
  ]
}
