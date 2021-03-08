psql_sources = files(
  'command.c',
  'common.c',
  'copy.c',
  'crosstabview.c',
  'describe.c',
  'help.c',
  'input.c',
  'large_obj.c',
  'mainloop.c',
  'prompt.c',
  'startup.c',
  'stringutils.c',
  'tab-complete.c',
  'variables.c',
)

psql_sources += custom_target('psqlscanslash',
  input: ['psqlscanslash.l'],
  output: ['psqlscanslash.c'],
  command: [flex, '-CFe', '-p', '-p', '-o', '@OUTPUT@', '@INPUT@'])

psql_sources += custom_target('psql_help',
  input: ['create_help.pl'],
  output: ['sql_help.c', 'sql_help.h'],
  depfile: 'sql_help.dep',
  command: [perl, '@INPUT0@', '@SOURCE_ROOT@/doc/src/sgml/ref', '@OUTDIR@', 'sql_help'])

executable('psql',
  psql_sources,
  link_with : [pgport_static, common_static, fe_utils],
  dependencies : [frontend_code, libpq, m_dep, readline],
  install : true
)
