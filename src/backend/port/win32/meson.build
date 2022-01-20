backend_sources += files(
  'signal.c',
  'socket.c',
  'timer.c',
)

if cdata.has('HAVE_MINIDUMP_TYPE')
  backend_sources += files('crashdump.c')
endif
