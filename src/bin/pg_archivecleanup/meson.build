pg_archivecleanup = executable('pg_archivecleanup',
  ['pg_archivecleanup.c'],
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)

tests += {
  'name': 'pg_archivecleanup',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/010_pg_archivecleanup.pl',
    ],
  },
}

subdir('po', if_found: libintl)
