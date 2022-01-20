initdb_sources = files(
  'findtimezone.c',
  'initdb.c'
)

initdb_sources += timezone_localtime_source

#fixme: reimplement libpq_pgport logic

if host_system == 'windows'
  initdb_sources += windows.compile_resources(
    custom_target('win32ver.rc', kwargs: rcgen_kw,
      command: rcgen_bin_cmd + [
        '--FILEDESC', 'initdb - initialize a new database cluster',
        '--NAME', 'initdb',]),
    kwargs: rccompile_kw,
  )
endif

executable('initdb',
  initdb_sources,
  include_directories: [timezone_inc],
  dependencies: [frontend_code, libpq, icu, icu_i18n],
  kwargs: default_bin_args,
)

tests += {
  'name': 'initdb',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {'with_icu': icu.found() ? 'yes' : 'no'},
    'tests': [
      't/001_initdb.pl',
    ],
  },
}

subdir('po', if_found: libintl)
