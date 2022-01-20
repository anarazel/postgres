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
  command: [flex, '-b', '-Cfe', '-p', '-p', '-o', '@OUTPUT@', '@INPUT@'])

psql_sources += custom_target('psql_help',
  input: ['create_help.pl'],
  output: ['sql_help.c', 'sql_help.h'],
  depfile: 'sql_help.dep',
  command: [perl, '@INPUT0@', '@SOURCE_ROOT@/doc/src/sgml/ref', '@OUTDIR@', 'sql_help'])

if host_machine.system() == 'windows'
  psql_cdata = rc_bin_cdata
  psql_cdata.set_quoted('FILEDESC', 'psql - the PostgreSQL interactive terminal')
  psql_cdata.set_quoted('INTERNAL_NAME', 'psql')
  psql_cdata.set_quoted('ORIGINAL_NAME', 'psql.exe')

  win32_ver_rc = configure_file(
    output: 'win32ver.rc',
    input: win32ver_rc_in,
    configuration: psql_cdata
  )

  psql_sources += windows.compile_resources(
    win32_ver_rc,
    include_directories: postgres_inc,
  )
endif

executable('psql',
  psql_sources,
  c_pch: '../../include/pch/c_pch.h',
  include_directories: include_directories('.'),
  dependencies : [frontend_code, libpq, readline],
  kwargs: default_bin_args,
)

tap_tests += {
  'name': 'psql',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'env': {'with_readline': readline.found() ? 'yes' : 'no'},
  'tests': [
    't/001_basic.pl',
    't/010_tab_completion.pl',
    't/020_cancel.pl',
  ],
}
