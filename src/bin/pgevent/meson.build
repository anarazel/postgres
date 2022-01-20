if host_system != 'win32'
  subdir_done()
endif

pgevent_sources = files(
    'pgevent.c',
)

# built as part of pgmsgevent.rc, via #include
win32ver_rc = custom_target(
  'win32ver.rc',
  kwargs: rcgen_kw,
  command: rcgen_lib_cmd + [
    '--FILEDESC', 'Eventlog message formatter',
    '--NAME', 'pgevent',],
)

pgevent_sources += windows.compile_resources(
  'pgmsgevent.rc',
  depends: win32ver_rc, # included in file
  kwargs: rccompile_kw,
)

shared_library('pgevent',
  pgevent_sources,
  dependencies: [frontend_code],
  vs_module_defs: 'pgevent.def',
  kwargs: default_lib_args + {
    'name_prefix': '',
  },
)
