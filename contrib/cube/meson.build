cube_sources = files(
  'cube.c',
)

# cubescan is compiled as part of cubeparse
cubescan = custom_target('cubescan',
  input: ['cubescan.l'],
  output: ['cubescan.c'],
  command: [flex, '-CFe', '-p', '-p', '-o', '@OUTPUT@', '@INPUT@'])

cube_sources += custom_target('cubeparse',
  input: 'cubeparse.y',
  output: 'cubeparse.c',
  depends: cubescan,
  command: [bison, '-Wno-deprecated', '-o', '@OUTPUT@', '@INPUT0@'])

cube = shared_module('cube',
  cube_sources,
  dependencies: [backend_code],
  c_args: [],
  link_with: [postgres],
  name_prefix: '',
  install: true)

install_data(
  'cube.control',
  'cube--1.0--1.1.sql',
  'cube--1.1--1.2.sql',
  'cube--1.2.sql',
  'cube--1.2--1.3.sql',
  'cube--1.3--1.4.sql',
  'cube--1.4--1.5.sql',
  install_dir: get_option('datadir') / 'extension'
)

regress_tests += {
  'name': 'cube',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'cube',
    'cube_sci',
  ],
}
