pgoutput_sources = files(
  'pgoutput.c',
)

pgoutput = shared_module('pgoutput',
  pgoutput_sources,
  dependencies: [backend_code],
  c_args: [],
  name_prefix: '',
  install: true)
