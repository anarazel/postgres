backend_sources += files(
  'condition_variable.c',
  'deadlock.c',
  'lmgr.c',
  'lock.c',
  'lwlock.c',
  'predicate.c',
  'proc.c',
  's_lock.c',
  'spin.c',
)

lwlocknames_backend = custom_target('lwlocknames',
  input : files('lwlocknames.txt'),
  output : ['lwlocknames.c', 'lwlocknames.h'],
  command : [perl, files('generate-lwlocknames.pl'), '-o', '@OUTDIR@', '@INPUT@']
)
generated_backend_sources += lwlocknames_backend[0]
