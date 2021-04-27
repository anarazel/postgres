pgport_sources = [
  'bsearch_arg.c',
  'chklocale.c',
  'erand48.c',
  'inet_net_ntop.c',
  'noblock.c',
  'path.c',
  'pg_bitutils.c',
  'pg_strong_random.c',
  'pgcheckdir.c',
  'pgmkdirp.c',
  'pgsleep.c',
  'pgstrcasecmp.c',
  'pgstrsignal.c',
  'pqsignal.c',
  'qsort.c',
  'qsort_arg.c',
  'quotes.c',
  'snprintf.c',
  'strerror.c',
  'tar.c',
  'thread.c',
]

# FIXME: This needs to be made dependent on config tests obviously...
#

pgport_sources += files('strlcpy.c')
pgport_sources += files('strlcat.c')
pgport_sources += files('fls.c')

#if not cc.has_function('getpeereid')
  pgport_sources += files('getpeereid.c')
#endif

#pgport_sources += files('pwrite.c')
pgport_sources += files('pg_crc32c_sse42.c')


pgport_srv = static_library('pgport_srv',
  pgport_sources + generated_headers,
  dependencies: [backend_code],
  install: true)

pgport_static = static_library('pgport',
  pgport_sources + generated_headers,
  dependencies: [frontend_code],
  install: true)

# FIXME: add relevant cflags
pgport_shlib = static_library('pgport_shlib',
  pgport_sources + generated_headers,
  dependencies: [frontend_code],
  install: true)
