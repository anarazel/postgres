backend_parser = custom_target('gram',
  input: '../../backend/parser/gram.y',
  install: true,
  # Only install gram.h, not gram.c
  install_dir: [false, dir_include_server / 'parser'],
  kwargs: bison_kw,
)
generated_sources += backend_parser.to_list()

# autoconf generates the file there, ensure we get a conflict
generated_sources_ac += {'src/backend/parser': ['gram.c', 'gram.h']}
