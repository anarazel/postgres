tap_tests += {
  'name': 'kerberos',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tests': [
    't/001_auth.pl',
  ],
  'env' : {
    'with_gssapi': gssapi.found() ? 'yes' : 'no',
    'with_krb_srvnam': 'postgres',
  },
}
