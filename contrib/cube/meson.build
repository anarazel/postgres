cube_sources = files(
  'cube.c',
)

# cubescan is compiled as part of cubeparse
cubescan = custom_target('cubescan',
  input: ['cubescan.l'],
  output: ['cubescan.c'],
  command: [flex_cmd, '@INPUT@'],)

cube_sources += custom_target('cubeparse',
  input: 'cubeparse.y',
  output: 'cubeparse.c',
  depends: cubescan,
  command: [bison_cmd, '@INPUT@'],)

cube = shared_module('cube',
  cube_sources,
  include_directories: include_directories('.'),
  kwargs: contrib_mod_args,
)

install_data(
  'cube.control',
  'cube--1.0--1.1.sql',
  'cube--1.1--1.2.sql',
  'cube--1.2.sql',
  'cube--1.2--1.3.sql',
  'cube--1.3--1.4.sql',
  'cube--1.4--1.5.sql',
  kwargs: contrib_data_args,
)

install_headers(
  'cubedata.h',
  install_dir: dir_include_extension / 'cube',
)

tests += {
  'name': 'cube',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'cube',
      'cube_sci',
    ],
  },
}
