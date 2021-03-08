backend_sources += files(
  'analyze.c',
  'parse_agg.c',
  'parse_clause.c',
  'parse_coerce.c',
  'parse_collate.c',
  'parse_cte.c',
  'parse_enr.c',
  'parse_expr.c',
  'parse_func.c',
  'parse_node.c',
  'parse_oper.c',
  'parse_param.c',
  'parse_relation.c',
  'parse_target.c',
  'parse_type.c',
  'parse_utilcmd.c',
  'parser.c',
  'scansup.c',
)

backend_scanner = custom_target('scan',
  input: ['scan.l'],
  output: ['scan.c'],
  command: [flex, '-CF', '-p', '-p', '-o', '@OUTPUT@', '@INPUT0@'])
generated_backend_sources += backend_scanner[0]

# gram.h is unused here, separate invocation in src/include/parser/meson.build
backend_parser = custom_target('gram',
  input: ['gram.y'],
  output: ['gram.c', 'gram.h'],
  command: [bison, '-Wno-deprecated', '-d', '-o', '@OUTPUT0@', '@INPUT0@'])
generated_backend_sources += backend_parser[0]
