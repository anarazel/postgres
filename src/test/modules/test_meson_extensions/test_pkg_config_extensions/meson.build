# Copyright (c) 2022-2025, PostgreSQL Global Development Group

# pkgconfig is not available on Windows, so skip it.
if host_machine.system() == 'windows'
  subdir_done()
endif

meson_extension_tests += {
  'name': 'amcheck',
  'kind': 'pkg_config',
  'sd': meson.current_source_dir() / 'amcheck',
}

meson_extension_tests += {
  'name': 'auth_delay',
  'kind': 'pkg_config',
  'sd': meson.current_source_dir() / 'auth_delay',
}

meson_extension_tests += {
  'name': 'postgres_fdw',
  'kind': 'pkg_config',
  'sd': meson.current_source_dir() / 'postgres_fdw',
}
