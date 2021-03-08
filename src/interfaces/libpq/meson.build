libpq_sources = files(
  'fe-auth-scram.c',
  'fe-connect.c',
  'fe-exec.c',
  'fe-lobj.c',
  'fe-misc.c',
  'fe-print.c',
  'fe-protocol3.c',
  'fe-secure.c',
  'legacy-pqsignal.c',
  'libpq-events.c',
  'pqexpbuffer.c',
  'fe-auth.c',
)

if ssl.found()
  libpq_sources += files('fe-secure-common.c')
  libpq_sources += files('fe-secure-openssl.c')
endif


export_file = custom_target('libpq_exports.list',
  input: [files('exports.txt')],
  output: ['@BASENAME@.list'],
  command: [perl, files('../../tools/gen_versioning_script.pl'), 'gnu', '@INPUT0@', '@OUTPUT0@']
)

libpq_so = shared_library('pq', libpq_sources,
  include_directories : [postgres_inc],
  c_args: ['-DFRONTEND'],
  link_args: ['-Wl,--version-script=' + export_file.full_path()],
  link_depends: export_file,
  link_with: [pgport_shlib, common_shlib],
  install: true,
  soversion: '5',
  version: '5.14',
  dependencies: [ssl]
  )

libpq = declare_dependency(
  link_with: [libpq_so],
  include_directories: [include_directories('.')])

install_headers('libpq-fe.h', 'libpq-events.h')
# FIXME: adjust path
install_headers('libpq-int.h', 'pqexpbuffer.h')
install_data('pg_service.conf.sample', install_dir: get_option('datadir'))
