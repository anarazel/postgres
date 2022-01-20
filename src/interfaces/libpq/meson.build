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

if host_system == 'win32'
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
libpq_link_depends = []

libpq_kwargs = default_lib_args + {
  'version': '5.'+pg_version_major.to_string(),
}


if host_system == 'aix'
  libpq_kwargs = libpq_kwargs + {
    'link_args': ['-Wl,-bE:' + export_file.full_path()],
    'link_depends': export_file,
    'soversion': '5',
  }
elif host_system == 'darwin'
  libpq_kwargs = libpq_kwargs + {
    'link_args': ['-exported_symbols_list', export_file.full_path()],
    'link_depends': export_file,
    'soversion': '5',
  }
elif host_system == 'win32'
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

libpq_both = both_libraries('libpq',
  libpq_sources,
  include_directories : [libpq_inc, postgres_inc],
  c_args: ['-DFRONTEND'],
  dependencies: libpq_deps,
  kwargs: libpq_kwargs,
)

libpq_so = libpq_both.get_shared_lib()

libpq = declare_dependency(
  link_with: [libpq_so],
  include_directories: [include_directories('.')]
)

pkgconfig.generate(
  libpq_so,
  name: 'libpq',
  description: 'PostgreSQL libpq library',
  url: pg_url,
)

install_headers(
  'libpq-fe.h',
  'libpq-events.h',
)

install_headers(
  'libpq-int.h',
  'pqexpbuffer.h',
  'fe-auth-sasl.h',
  install_dir: dir_include_internal,
)
install_data('pg_service.conf.sample',
  install_dir: dir_data,
)


tests += {
  'name': 'libpq',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_uri.pl',
      't/002_api.pl',
    ],
    'env' : {'with_ssl': get_option('ssl')},
  },
}

subdir('po', if_found: libintl)
