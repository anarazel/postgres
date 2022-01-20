passwordcheck_sources = files(
  'passwordcheck.c',
)

passwordcheck_c_args = []
passwordcheck_deps = []

# uncomment the following two lines to enable cracklib support
# passwordcheck_c_args += ['-DUSE_CRACKLIB', '-DCRACKLIB_DICTPATH="/usr/lib/cracklib_dict"']
# passwordcheck_deps += [cc.find_library('crack')]

passwordcheck = shared_module('passwordcheck',
  passwordcheck_sources,
  kwargs: contrib_mod_args + {
    'c_args': contrib_mod_args.get('c_args') + passwordcheck_c_args,
    'dependencies': contrib_mod_args.get('dependencies') + passwordcheck_deps,
  }
)

regress_tests += {
  'name': 'passwordcheck',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'sql': [
    'passwordcheck'
  ],
}
