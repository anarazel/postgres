backend_sources += files(
  'backup_manifest.c',
  'basebackup.c',
  'basebackup_copy.c',
  'basebackup_gzip.c',
  'basebackup_lz4.c',
  'basebackup_progress.c',
  'basebackup_server.c',
  'basebackup_sink.c',
  'basebackup_target.c',
  'basebackup_throttle.c',
  'basebackup_zstd.c',
  'slot.c',
  'slotfuncs.c',
  'syncrep.c',
  'walreceiver.c',
  'walreceiverfuncs.c',
  'walsender.c',
)

# [sync]repl_scanner is compiled as part of [sync]repl_gram. The
# ordering is enforced by making the generation of grammar depend on
# the scanner generation. That's unnecessarily strict, but overall
# harmless.

repl_scanner = custom_target('repl_scanner',
  input : files('repl_scanner.l'),
  output : ['repl_scanner.c'],
  command: [flex_cmd, '@INPUT@'],
)

generated_backend_sources += custom_target('repl_gram',
  input: 'repl_gram.y',
  output: 'repl_gram.c',
  depends: repl_scanner,
  command: [bison_cmd, '@INPUT@'],)


syncrep_scanner = custom_target('syncrep_scanner',
  input: 'syncrep_scanner.l',
  output: 'syncrep_scanner.c',
  command: [flex_cmd, '@INPUT@'],)

generated_backend_sources += custom_target('syncrep_gram',
  input: 'syncrep_gram.y',
  output: 'syncrep_gram.c',
  depends: syncrep_scanner,
  command: [bison_cmd, '@INPUT@'],)


subdir('logical')
