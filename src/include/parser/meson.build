backend_parser_header = custom_target('gram',
  input: [files('../../backend/parser/gram.y')],
  output: ['gram.c', 'gram.h'],
  command: [bison_cmd, '-d', '@INPUT@'],
  install: true,
  # Only install gram.h, not gram.c
  install_dir: [false, dir_include_server / 'parser'],
)

#generated_backend_headers += backend_parser[1]
