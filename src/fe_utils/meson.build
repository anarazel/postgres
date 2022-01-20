fe_utils_sources = files(
  'archive.c',
  'cancel.c',
  'conditional.c',
  'connect_utils.c',
  'mbprint.c',
  'option_utils.c',
  'parallel_slot.c',
  'print.c',
  'query_utils.c',
  'recovery_gen.c',
  'simple_list.c',
  'string_utils.c',
)

fe_utils_sources += custom_target('psqlscan',
  input: ['psqlscan.l'],
  output: ['psqlscan.c'],
  command: [flex, '-b', '-Cfe', '-p', '-p', '-o', '@OUTPUT@', '@INPUT@'])

fe_utils = static_library('fe_utils',
  fe_utils_sources + generated_headers,
  c_pch: '../include/pch/c_pch.h',
  include_directories :  [postgres_inc, libpq_inc],
  c_args: ['-DFRONTEND'],
  kwargs: default_lib_args,
)
