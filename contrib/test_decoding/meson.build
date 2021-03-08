test_decoding_sources = files(
  'test_decoding.c',
)

test_decoding = shared_module('test_decoding',
  test_decoding_sources,
  dependencies: [backend_code, libpq],
  c_args: [],
  link_with: [postgres],
  name_prefix: '',
  install: true)
