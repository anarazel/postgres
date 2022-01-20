if host_machine.system() != 'windows'
  subdir_done()
endif

pgevent_sources = files(
    'pgevent.c',
)

pgevent_cdata = rc_lib_cdata
pgevent_cdata.set_quoted('FILEDESC', 'Eventlog message formatter')
pgevent_cdata.set_quoted('INTERNAL_NAME', 'pgevent')
pgevent_cdata.set_quoted('ORIGINAL_NAME', 'pgevent.dll')

configure_file(
  output: 'win32ver.rc',
  input: win32ver_rc_in,
  configuration: pgevent_cdata
)

pgevent_sources += windows.compile_resources(
  'pgmsgevent.rc',
  include_directories: [include_directories('.'), postgres_inc],
)

shared_library('pgevent',
  pgevent_sources,
  dependencies: [frontend_code],
  vs_module_defs: 'pgevent.def',
  kwargs: default_lib_args + {
    'name_prefix': '',
  },
)
