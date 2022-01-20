backend_parser_header = custom_target('gram',
  input: [files('../../backend/parser/gram.y')],
  output: ['gram.c', 'gram.h'],
  command: [bison, bisonflags, '-d', '-o', '@OUTPUT0@', '@INPUT0@'],
  install: true,
  # Only install gram.h, not gram.c
  install_dir: [false, get_option('includedir')]
)

#generated_backend_headers += backend_parser[1]
