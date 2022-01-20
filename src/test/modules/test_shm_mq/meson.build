# FIXME: prevent install during main install, but not during test :/
test_shm_mq = shared_module('test_shm_mq',
  files(
    'setup.c',
    'test.c',
    'worker.c',
  ),
  kwargs: pg_mod_args,
)

install_data(
  'test_shm_mq.control',
  'test_shm_mq--1.0.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'test_shm_mq',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'test_shm_mq',
  ]
}
