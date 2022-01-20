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

# guc-file.c is compiled as part of guc.c, generate guc-file.c.h so it can be
# added as a dependency for the executable. Not pretty, but there's no
# obviously better solution right now
guc_scan = custom_target('guc_scan',
  input: 'guc-file.l',
  output: ['guc-file.c', 'guc-file.c.h'],
  command: [flex_cmd, '--stamp-header', '@OUTPUT1@'])

backend_build_deps += declare_dependency(
  sources: guc_scan[1],
  include_directories: include_directories('.'))
generated_backend_headers += guc_scan[1]
generated_sources += guc_scan[0]

install_data('postgresql.conf.sample',
  install_dir: dir_data,
)
