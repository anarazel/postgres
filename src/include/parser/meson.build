# gram.c is unused, separate invocation in src/backend/parser/meson.build

backend_parser = custom_target('gram',
  input: [files('../../backend/parser/gram.y')],
  output: ['gram.c', 'gram.h'],
  command: [bison, '-Wno-deprecated', '-d', '-o', '@OUTPUT0@', '@INPUT0@'])

generated_headers += backend_parser[1]
