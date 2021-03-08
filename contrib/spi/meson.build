autoinc = shared_module('autoinc',
  ['autoinc.c'],
  dependencies: [backend_code, libpq],
  c_args: [],
  name_prefix: '',
  install: true)

install_data('autoinc.control', 'autoinc--1.0.sql',
  install_dir: get_option('datadir') / 'extension'
)


insert_username = shared_module('insert_username',
  ['insert_username.c'],
  dependencies: [backend_code, libpq],
  c_args: [],
  name_prefix: '',
  install: true)

install_data('insert_username.control', 'insert_username--1.0.sql',
  install_dir: get_option('datadir') / 'extension'
)


moddatetime = shared_module('moddatetime',
  ['moddatetime.c'],
  dependencies: [backend_code, libpq],
  c_args: [],
  name_prefix: '',
  install: true)

install_data('moddatetime.control', 'moddatetime--1.0.sql',
  install_dir: get_option('datadir') / 'extension'
)

# this is needed for the regression tests;
# comment out if you want a quieter refint package for other uses
refint_cflags = ['-DREFINT_VERBOSE']

refint = shared_module('refint',
  ['refint.c'],
  c_args: refint_cflags,
  name_prefix: '',
  dependencies: [backend_code, libpq],
  install: true)

install_data('refint.control', 'refint--1.0.sql',
  install_dir: get_option('datadir') / 'extension'
)
