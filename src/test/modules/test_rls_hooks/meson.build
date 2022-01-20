# FIXME: prevent install during main install, but not during test :/
test_rls_hooks = shared_module('test_rls_hooks',
  ['test_rls_hooks.c'],
  kwargs: pg_mod_args,
)

install_data(
  'test_rls_hooks.control',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'test_rls_hooks',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'test_rls_hooks',
  ]
}
