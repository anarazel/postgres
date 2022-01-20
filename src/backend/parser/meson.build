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
  'parse_merge.c',
  'parse_node.c',
  'parse_oper.c',
  'parse_param.c',
  'parse_relation.c',
  'parse_target.c',
  'parse_type.c',
  'parse_utilcmd.c',
  'scansup.c',
)

# Build a small utility static lib for the parser. This makes it easier to not
# depend on gram.h already having been generated for most of the other code
# (which depends on generated headers having been generated). The generation
# of the parser is slow...

parser_sources = [files('parser.c')]

backend_scanner = custom_target('scan',
  input: 'scan.l',
  output: 'scan.c',
  command: [flex_cmd, '--no-backup', '--fix-warnings', '--', '-CF', '-p', '-p'],
)
generated_sources += backend_scanner
parser_sources += backend_scanner

parser_sources += backend_parser_code
parser_sources += backend_parser_header

generated_sources += backend_parser_code
generated_sources += backend_parser_header

parser = static_library('parser',
  parser_sources + generated_headers,
  dependencies: [backend_code],
  kwargs: internal_lib_args,
)
backend_link_with += parser
