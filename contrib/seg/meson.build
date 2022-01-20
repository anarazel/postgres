seg_sources = files(
  'seg.c',
)

# segscan is compiled as part of segparse
segscan = custom_target('segscan',
  input: ['segscan.l'],
  output: ['segscan.c'],
  command: [flex, '-o', '@OUTPUT@', '@INPUT@'])

seg_sources += custom_target('segparse',
  input: 'segparse.y',
  output: 'segparse.c',
  depends: segscan,
  command: [bison, bisonflags, '-o', '@OUTPUT@', '@INPUT0@'])

seg = shared_module('seg',
  seg_sources,
  include_directories: include_directories('.'),
  kwargs: contrib_mod_args,
)

install_data(
  'seg.control',
  'seg--1.0--1.1.sql',
  'seg--1.1--1.2.sql',
  'seg--1.1.sql',
  'seg--1.2--1.3.sql',
  'seg--1.3--1.4.sql',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'seg',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'seg',
  ],
}
