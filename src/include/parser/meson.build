backend_parser = custom_target('gram',
  input: [files('../../backend/parser/gram.y')],
  output: ['gram.c', 'gram.h'],
  command: [bison_cmd, '-d', '@INPUT@'],
  install: true,
  # Only install gram.h, not gram.c
  install_dir: [false, dir_include_server / 'parser'],
)

backend_parser_code = backend_parser[0]
backend_parser_header = backend_parser[1]

# autoconf generates the file there, ensure we get a conflict
generated_sources_ac += {'src/backend/parser': ['gram.c', 'gram.h']}
