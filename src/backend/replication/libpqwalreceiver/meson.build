libpqwalreceiver_sources = files(
  'libpqwalreceiver.c',
)

libpqwalreceiver = shared_module('pqwalreceiver',
  libpqwalreceiver_sources,
  dependencies: [backend_code, libpq],
  install: true)
