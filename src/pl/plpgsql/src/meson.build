plpgsql_sources = files(
  'pl_comp.c',
  'pl_exec.c',
  'pl_funcs.c',
  'pl_handler.c',
  'pl_scanner.c',
)

plpgsql_sources += custom_target('gram',
  input: ['pl_gram.y'],
  output: ['pl_gram.c', 'pl_gram.h'],
  command: [bison, '-Wno-deprecated', '-d', '-o', '@OUTPUT0@', '@INPUT0@'])

gen_plerrcodes = files('generate-plerrcodes.pl')
plpgsql_sources += custom_target('plerrcodes',
  input: ['../../../../src/backend/utils/errcodes.txt'],
  output: ['plerrcodes.h'],
  command: [perl, gen_plerrcodes, '@INPUT0@'],
  capture: true)

gen_keywordlist = files('../../../../src/tools/gen_keywordlist.pl')
plpgsql_sources += custom_target('pl_reserved_kwlist',
  input: ['pl_reserved_kwlist.h'],
  output: ['pl_reserved_kwlist_d.h'],
  command: [perl, '-I', '@SOURCE_ROOT@/src/tools', gen_keywordlist, '--output', '@OUTDIR@', '--varname', 'ReservedPLKeywords', '@BUILD_ROOT@/@INPUT@']
)

plpgsql_sources += custom_target('pl_unreserved_kwlist',
  input: ['pl_unreserved_kwlist.h'],
  output: ['pl_unreserved_kwlist_d.h'],
  command: [perl, '-I', '@SOURCE_ROOT@/src/tools', gen_keywordlist, '--output', '@OUTDIR@', '--varname', 'UnreservedPLKeywords', '@BUILD_ROOT@/@INPUT@']
)

shared_module('plpgsql',
  plpgsql_sources,
  name_prefix: '',
  dependencies: [backend_code],
  install: true)

install_data('plpgsql.control', 'plpgsql--1.0.sql',
  install_dir: get_option('datadir') / 'extension'
)

install_headers('plpgsql.h',
  install_dir: get_option('includedir') / 'server')
