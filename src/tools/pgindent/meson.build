# Currently the code requires meson 0.63 or upwards. This could likely be
# lifted (in fact, some older versions actually work, but only some), but for
# now it's not crucial.
typedefs_supported = meson.version().version_compare('>=0.63')
if not typedefs_supported
  subdir_done()
endif

typedefs_from_objs = files('typedefs_from_objs')
typedefs_from_objs_cmd = [typedefs_from_objs, '--host', host_system, '--output', '@OUTPUT@', '@INPUT@']
merge_typedefs = files('merge_typedefs')

# XXX: This list of targets should likely not be maintained here
typedef_src_tgts = [
  backend_targets,
  bin_targets,
  [libpq_st],
  pl_targets,
  contrib_targets,
  ecpg_targets,
]

# We generate partial typedefs files for each binary/library. That makes using
# this during incremental development much faster.
#
# The reason we process the object files instead of executables is that that
# doesn't work on macos. There doesn't seem to be a reason to target
# executables directly on other platforms.
typedef_tgts = []
foreach tgts : typedef_src_tgts
  foreach tgt : tgts
    # can't use tgt.name(), as we have have targets that differ just in suffix
    name = fs.name(tgt.full_path())
    tdname = 'typedefs.list.local-@0@'.format(name)
    objs = tgt.extract_all_objects(recursive: true)
    typedef_tgts += custom_target(tdname,
                                  output: tdname,
                                  command: typedefs_from_objs_cmd,
                                  input: objs,
                                  build_by_default: false,
                                  )

  endforeach
endforeach


# Build new typedefs.list files. typedefs.list.local will contain only
# typedefs in the local build, whereas typedefs.list.merged is the combination
# of typedefs.list.local and the source tree's typedefs.list. However,
# typedefs that are not used in the code anymore are removed even from
# typedefs.list.merged.
typedefs = custom_target('typedefs.list', output: ['typedefs.list.local', 'typedefs.list.merged'],
  command: [merge_typedefs,
            '--output-local', '@OUTPUT0@',
            '--output-merged', '@OUTPUT1@',
            '--current-typedefs', files('typedefs.list'),
            '--filter-source', '@SOURCE_ROOT@',
            '@INPUT@'],
  input: typedef_tgts,
  build_by_default: false,
)
typedefs_local = typedefs[0]
typedefs_merged = typedefs[1]

if cp.found()
  run_target('update-typedefs',
    command: [cp, typedefs_merged, '@SOURCE_ROOT@/src/tools/pgindent/typedefs.list'],
  )
endif
