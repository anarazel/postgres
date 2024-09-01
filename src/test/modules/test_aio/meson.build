# Copyright (c) 2022-2024, PostgreSQL Global Development Group

test_aio_sources = files(
  'test_aio.c',
)

if host_system == 'windows'
  test_aio_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_aio',
    '--FILEDESC', 'test_aio - test code for AIO',])
endif

test_aio = shared_module('test_aio',
  test_aio_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_aio

test_install_data += files(
  'test_aio.control',
  'test_aio--1.0.sql',
)


testfiles = [
  'prep',
  'ownership',
  'io',
]

if get_option('injection_points')
  testfiles += 'inject'
endif


tests += {
  'name': 'test_aio_sync',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': testfiles,
    'regress_args': [
      '--temp-config', files('sync.conf'),
    ],
    # requires custom config
    'runningcheck': false,
  },
}

tests += {
  'name': 'test_aio_worker',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': testfiles,
    'regress_args': [
      '--temp-config', files('worker.conf'),
    ],
    # requires custom config
    'runningcheck': false,
  },
}

if liburing.found()
  tests += {
    'name': 'test_aio_uring',
    'sd': meson.current_source_dir(),
    'bd': meson.current_build_dir(),
    'regress': {
      'sql': testfiles,
      'regress_args': [
        '--temp-config', files('io_uring.conf'),
      ],
      # requires custom config
      'runningcheck': false,
    }
  }
endif
