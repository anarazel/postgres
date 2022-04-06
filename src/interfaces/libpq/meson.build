# test/ is entered via top-level meson.build, that way it can use the default
# args for executables (which depend on libpq).

libpq_sources = files(
  'fe-auth-scram.c',
  'fe-connect.c',
  'fe-exec.c',
  'fe-lobj.c',
  'fe-misc.c',
  'fe-print.c',
  'fe-protocol3.c',
  'fe-secure.c',
  'fe-trace.c',
  'legacy-pqsignal.c',
  'libpq-events.c',
  'pqexpbuffer.c',
  'fe-auth.c',
)

if host_machine.system() == 'windows'
  libpq_sources += files('win32.c', 'pthread-win32.c')
endif

if ssl.found()
  libpq_sources += files('fe-secure-common.c')
  libpq_sources += files('fe-secure-openssl.c')
endif

if gssapi.found()
  libpq_sources += files(
    'fe-secure-gssapi.c',
    'fe-gssapi-common.c'
  )
endif

export_file = custom_target('libpq_exports.list',
  input: [files('exports.txt')],
  output: ['@BASENAME@.list'],
  command: [perl, files('../../tools/gen_versioning_script.pl'),
    export_file_format,
    '@INPUT0@', '@OUTPUT0@'],
  build_by_default: false,
  install: false,
)

libpq_def = custom_target('libpq.def',
  command: [perl, files('../../tools/msvc/export2def.pl'), '@OUTPUT@', '@INPUT0@', 'libpq'],
  input: files('exports.txt'),
  output: 'libpq.def',
  build_by_default: false,
  install: false,
)

# port needs to be in include path due to pthread-win32.h
libpq_inc = include_directories('.', '../../port')
libpq_deps = [frontend_shlib_code, thread_dep, ssl, ldap, gssapi]
libpq_link_depends = []

libpq_kwargs = default_lib_args + {
  'version': '5.'+pg_version_major.to_string(),
}


if host_machine.system() == 'aix'
  libpq_kwargs = libpq_kwargs + {
    'link_args': ['-Wl,-bE:' + export_file.full_path()],
    'link_depends': export_file,
    'soversion': '5',
  }
elif host_machine.system() == 'darwin'
  libpq_kwargs = libpq_kwargs + {
    'link_args': ['-exported_symbols_list', export_file.full_path()],
    'link_depends': export_file,
    'soversion': '5',
  }
elif host_machine.system() == 'windows'
  libpq_deps += cc.find_library('secur32', required: true)

  libpq_kwargs = libpq_kwargs + {
    'vs_module_defs': libpq_def,
    'soversion': '',
  }
else
  libpq_kwargs = libpq_kwargs + {
    'link_args': '-Wl,--version-script=' + export_file.full_path(),
    'link_depends': export_file,
    'soversion': '5',
  }
endif

libpq_so = shared_library('pq',
  libpq_sources,
  include_directories : [libpq_inc, postgres_inc],
  c_args: ['-DFRONTEND'],
  c_pch: '../../include/pch/c_pch.h',
  dependencies: libpq_deps,
  kwargs: libpq_kwargs,
)

libpq = declare_dependency(
  link_with: [libpq_so],
  include_directories: [include_directories('.')]
)

install_headers('libpq-fe.h', 'libpq-events.h')
# FIXME: adjust path
install_headers('libpq-int.h', 'pqexpbuffer.h')
install_data('pg_service.conf.sample', install_dir: get_option('datadir'))


tests += {
  'name': 'libpq',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_uri.pl',
    ],
  },
}
