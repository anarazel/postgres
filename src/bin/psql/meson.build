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
  command: [flex_cmd, flex_backup, flex_fix_warning, '-Cfe', '-p', '-p', '@INPUT@'])

psql_sources += custom_target('psql_help',
  input: ['create_help.pl'],
  output: ['sql_help.c', 'sql_help.h'],
  depfile: 'sql_help.dep',
  command: [perl, '@INPUT0@', '@SOURCE_ROOT@/doc/src/sgml/ref', '@OUTDIR@', 'sql_help'])

if host_machine.system() == 'windows'
  psql_sources += windows.compile_resources(
    custom_target('win32ver.rc', kwargs: rcgen_kw,
      command: rcgen_bin_cmd + [
        '--FILEDESC', 'psql - the PostgreSQL interactive terminal',
        '--NAME', 'psql',]),
    kwargs: rccompile_kw,
  )
endif

executable('psql',
  psql_sources,
  c_pch: pch_c_h,
  include_directories: include_directories('.'),
  dependencies : [frontend_code, libpq, readline],
  kwargs: default_bin_args,
)

install_data('psqlrc.sample',
  install_dir: dir_data,
)

tests += {
  'name': 'psql',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {'with_readline': readline.found() ? 'yes' : 'no'},
    'tests': [
      't/001_basic.pl',
      't/010_tab_completion.pl',
      't/020_cancel.pl',
    ],
  },
}

subdir('po', if_found: libintl)
