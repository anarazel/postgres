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
  command: [bison, bisonflags, '-d', '-o', '@OUTPUT0@', '@INPUT0@'])

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
  command: [perl, '-I', '@SOURCE_ROOT@/src/tools', gen_keywordlist, '--output', '@OUTDIR@', '--varname', 'ReservedPLKeywords', '@INPUT@']
)

plpgsql_sources += custom_target('pl_unreserved_kwlist',
  input: ['pl_unreserved_kwlist.h'],
  output: ['pl_unreserved_kwlist_d.h'],
  command: [perl, '-I', '@SOURCE_ROOT@/src/tools', gen_keywordlist, '--output', '@OUTDIR@', '--varname', 'UnreservedPLKeywords', '@INPUT@']
)

shared_module('plpgsql',
  plpgsql_sources,
  c_pch: '../../../include/pch/postgres_pch.h',
  include_directories: include_directories('.'),
  kwargs: pg_mod_args,
)

install_data('plpgsql.control', 'plpgsql--1.0.sql',
  install_dir: get_option('datadir') / 'extension'
)

install_headers('plpgsql.h',
  install_dir: get_option('includedir') / 'server')


regress_tests += {
  'name': 'plpgsql',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
     'plpgsql_array',
     'plpgsql_call',
     'plpgsql_control',
     'plpgsql_copy',
     'plpgsql_domain',
     'plpgsql_record',
     'plpgsql_cache',
     'plpgsql_simple',
     'plpgsql_transaction',
     'plpgsql_trap',
     'plpgsql_trigger',
     'plpgsql_varprops',
  ],
}
