pg_walinspect_sources = files('pg_walinspect.c')

if host_system == 'windows'
  pg_walinspect_sources += windows.compile_resources(
    custom_target('win32ver.rc', kwargs: rcgen_kw,
      command: rcgen_bin_cmd + [
        '--FILEDESC', 'pg_walinspect - functions to inspect contents of PostgreSQL Write-Ahead Log',
        '--NAME', 'pg_walinspect',
      ]),
    kwargs: rccompile_kw,
  )
endif

pg_walinspect = shared_module('pg_walinspect',
  pg_walinspect_sources,
  kwargs: contrib_mod_args + {
      'dependencies': contrib_mod_args['dependencies'],
  },
)
contrib_targets += pg_walinspect

install_data(
  'pg_walinspect.control',
  'pg_walinspect--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_walinspect',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_walinspect',
    ],
    'regress_args': ['--temp-config', files('walinspect.conf')],
  },
}
