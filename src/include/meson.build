configure_file(input : 'pg_config_ext.h.meson',
  output : 'pg_config_ext.h',
  configuration : cdata)

system = host_machine.system()
if system == 'windows'
  system = 'win32'
endif

configure_file(
  output : 'pg_config_os.h',
  input: files('port/@0@.h'.format(system)),
  install: true,
  install_dir : get_option('includedir'),
  copy : true)

configure_file(
  output : 'pg_config.h',
  install : true,
  install_dir : get_option('includedir'),
  configuration : cdata)


config_paths_data = configuration_data()
config_paths_data.set_quoted('PGBINDIR', get_option('prefix') / get_option('bindir'))
config_paths_data.set_quoted('PGSHAREDIR', get_option('prefix') / get_option('datadir'))
config_paths_data.set_quoted('SYSCONFDIR', get_option('prefix') / get_option('sysconfdir'))
config_paths_data.set_quoted('INCLUDEDIR', get_option('prefix') / get_option('includedir'))
# FIXME: shouldn't be the same
config_paths_data.set_quoted('PKGINCLUDEDIR', get_option('prefix') / get_option('includedir'))
config_paths_data.set_quoted('INCLUDEDIRSERVER', get_option('prefix') / get_option('includedir'))
config_paths_data.set_quoted('LIBDIR', get_option('prefix') / get_option('libdir'))
# FIXME: figure out logic for pkglibdir
config_paths_data.set_quoted('PKGLIBDIR', get_option('prefix') / get_option('libdir'))
config_paths_data.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
config_paths_data.set_quoted('DOCDIR', get_option('prefix') / get_option('datadir') / 'doc/postgresql')
config_paths_data.set_quoted('HTMLDIR', get_option('prefix') / get_option('datadir') / 'doc/postgresql')
config_paths_data.set_quoted('MANDIR', get_option('prefix') / get_option('datadir') / 'doc/postgresql')

configure_file(
  output: 'pg_config_paths.h',
  configuration: config_paths_data,
  install: false
)


subdir('utils')
subdir('storage')
subdir('catalog')
subdir('parser')
