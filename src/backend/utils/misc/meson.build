backend_sources += files(
  'help_config.c',
  'pg_config.c',
  'pg_controldata.c',
  'pg_rusage.c',
  'ps_status.c',
  'queryenvironment.c',
  'rls.c',
  'sampling.c',
  'superuser.c',
  'timeout.c',
  'tzparser.c',
  'guc.c',
)

# guc-file is compiled as part of guc.c
# FIXME: the guc-file.h header doesn't exist, but ensures ordering -
# at the price of constant regeneration

guc_scan = custom_target('guc_scan',
  input: ['guc-file.l'],
  output: ['guc-file.c.h'],
  command: [flex, '-CF', '-p', '-p', '-o', '@OUTPUT@', '@INPUT@'])

generated_backend_sources += guc_scan

#backend_libs += declare_dependency(sources: [guc_scan])

install_data('postgresql.conf.sample', install_dir: 'share/')
