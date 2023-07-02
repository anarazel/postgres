backend_sources += files(
 'aio.c',
 'aio_exchange.c',
 'aio_funcs.c',
 'aio_io.c',
 'aio_scb.c',
 'aio_util.c',
 'aio_worker.c',
)

if liburing.found()
  backend_sources += files('aio_uring.c')
endif

if posix_aio.found()
  backend_sources += files('aio_posix.c')
endif
