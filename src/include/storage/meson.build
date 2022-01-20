# FIXME: this creates an unnecessary lwlocknames.c - but it's not
# obvious how to avoid that: meson insist on output files being in the
# current dir.

lwlocknames = custom_target('lwlocknames',
  input : files('../../backend/storage/lmgr/lwlocknames.txt'),
  output : ['lwlocknames.h', 'lwlocknames.c'],
  command : [perl, files('../../backend/storage/lmgr/generate-lwlocknames.pl'), '-o', '@OUTDIR@', '@INPUT@'],
  build_by_default: true,
  install: true,
  install_dir: [get_option('includedir'), false],
)

lwlocknames_h = lwlocknames[0]

generated_backend_headers += lwlocknames_h
