backend_sources += files(
  'guc.c',
  'help_config.c',
  'pg_config.c',
  'pg_controldata.c',
  'pg_rusage.c',
  'ps_status.c',
  'queryenvironment.c',
  'queryjumble.c',
  'rls.c',
  'sampling.c',
  'superuser.c',
  'timeout.c',
  'tzparser.c',
)

# guc-file.c.h is compiled as part of guc.c
guc_scan = custom_target('guc_scan',
  input: ['guc-file.l'],
  output: ['guc-file.c.h'],
  command: [flex_cmd, '@INPUT@'])

backend_build_deps += declare_dependency(sources: [guc_scan],
  include_directories: include_directories('.'))
generated_backend_headers += guc_scan

install_data('postgresql.conf.sample',
  install_dir: dir_data,
)
