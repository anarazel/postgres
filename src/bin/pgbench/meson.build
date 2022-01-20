pgbench_sources = files(
  'pgbench.c',
)

# exprscan is compiled as part ofexprparse. The ordering is enforced by making
# the generation of grammar depend on the scanner generation. That's
# unnecessarily strict, but overall harmless.

exprscan = custom_target('exprscan',
  input : files('exprscan.l'),
  output : ['exprscan.c'],
  command : [flex, '-o', '@OUTPUT0@', '@INPUT@']
)

exprparse = custom_target('exprparse',
  input: 'exprparse.y',
  output: 'exprparse.c',
  depends: exprscan,
  command: [bison, bisonflags, '-o', '@OUTPUT@', '@INPUT0@'])
pgbench_sources += exprparse

executable('pgbench',
  pgbench_sources,
  dependencies: [frontend_code, libpq, thread_dep],
  include_directories: include_directories('.'),
  kwargs: default_bin_args,
)


tap_tests += {
  'name' : 'pgbench',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests' :[
    't/001_pgbench_with_server.pl',
    't/002_pgbench_no_server.pl'
  ]
}
