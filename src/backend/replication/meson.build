backend_sources += files(
  'slot.c',
  'slotfuncs.c',
  'syncrep.c',
  'walreceiver.c',
  'walreceiverfuncs.c',
  'walsender.c',
)

repl_scanner = custom_target('repl_scanner',
  input: 'repl_scanner.l',
  output: 'repl_scanner.c',
  command: flex_cmd,
)
generated_backend_sources += repl_scanner

repl_gram = custom_target('repl_gram',
  input: 'repl_gram.y',
  kwargs: bison_kw,
)
generated_backend_sources += repl_gram.to_list()

syncrep_scanner = custom_target('syncrep_scanner',
  input: 'syncrep_scanner.l',
  output: 'syncrep_scanner.c',
  command: flex_cmd,
)
generated_backend_sources += syncrep_scanner

syncrep_gram = custom_target('syncrep_gram',
  input: 'syncrep_gram.y',
  kwargs: bison_kw,
)
generated_backend_sources += syncrep_gram.to_list()

subdir('logical')
