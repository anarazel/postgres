pgrowlocks = shared_module('pgrowlocks',
  files(
    'pgrowlocks.c',
  ),
  kwargs: contrib_mod_args,
)

install_data(
  'pgrowlocks--1.0--1.1.sql',
  'pgrowlocks--1.1--1.2.sql',
  'pgrowlocks--1.2.sql',
  'pgrowlocks.control',
  kwargs: contrib_data_args,
)
