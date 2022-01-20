if not libxml.found()
  subdir_done()
endif

xml2 = shared_module('pgxml',
  files(
    'xpath.c',
    'xslt_proc.c',
  ),
  c_pch: '../../src/include/pch/postgres_pch.h',
  kwargs: contrib_mod_args + {
    'dependencies': [libxml, libxslt, contrib_mod_args['dependencies']],
  },
)

install_data(
  'xml2--1.0--1.1.sql',
  'xml2--1.1.sql',
  'xml2.control',
  kwargs: contrib_data_args,
)

regress_tests += {
  'name': 'xml2',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'xml2',
  ],
}
