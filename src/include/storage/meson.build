# FIXME: this creates an unnecessary lwlocknames.c - but it's not
# obvious how to avoid that: meson insist on output files being in the
# current dir.

# FIXME: this leads to lwlocknames.c being installed. Bad.
lwlocknames = custom_target('lwlocknames',
  input : files('../../backend/storage/lmgr/lwlocknames.txt'),
  output : ['lwlocknames.h', 'lwlocknames.c'],
  command : [perl, files('../../backend/storage/lmgr/generate-lwlocknames.pl'), '-o', '@OUTDIR@', '@INPUT@'],
  build_by_default: true,
  install: true,
  install_dir: get_option('includedir')
)

generated_headers += lwlocknames[0]
