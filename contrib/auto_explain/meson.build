auto_explain = shared_module('auto_explain',
  files('auto_explain.c'),
  dependencies: [backend_code, libpq],
  c_args: [],
  link_with: [postgres],
  name_prefix: '',
  install: true)

tap_tests += {
  'sd': meson.current_source_dir(),
  'name' : 'auto_explain',
  'tests' :['t/001_auto_explain.pl']}
