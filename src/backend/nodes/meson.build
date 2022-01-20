backend_sources += files(
  'bitmapset.c',
  'extensible.c',
  'list.c',
  'makefuncs.c',
  'nodeFuncs.c',
  'nodes.c',
  'params.c',
  'print.c',
  'read.c',
  'tidbitmap.c',
  'value.c',
)

# these include .c files generated in ../../include/nodes, seems nicer to not
# add that as an include path for the whole backend
nodefunc_sources = files(
  'copyfuncs.c',
  'equalfuncs.c',
  'outfuncs.c',
  'readfuncs.c',
)
nodefuncs = static_library('nodefuncs',
  nodefunc_sources,
  c_pch: pch_c_h,
  dependencies: [backend_code],
  kwargs: default_lib_args + {'install': false},
  include_directories: include_directories('../../include/nodes'),
)
backend_link_with += nodefuncs
