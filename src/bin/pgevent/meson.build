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

# FIXME: copied from Mkvcbuild.pm, but I don't think that's the right approach
pgevent_link_args = []
if cc.get_id() == 'msvc'
  pgevent_link_args += '/ignore:4104'
endif

shared_library('pgevent',
  pgevent_sources,
  dependencies: [frontend_code],
  link_args: pgevent_link_args,
  vs_module_defs: 'pgevent.def',
  kwargs: default_lib_args + {
    'name_prefix': '',
  },
)
