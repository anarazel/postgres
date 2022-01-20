# FIXME: prevent install during main install, but not during test :/
delay_execution = shared_module('delay_execution',
  ['delay_execution.c'],
  kwargs: pg_mod_args,
)

isolation_tests += {
  'name': 'delay_execution',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'specs': [
    'partition-addition',
    'partition-removal-1',
  ]
}
