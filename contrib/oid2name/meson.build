executable('oid2name',
  ['oid2name.c'],
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args,
)

tests += {
  'name' : 'oid2name',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests' :[
      't/001_basic.pl',
    ],
  },
}
