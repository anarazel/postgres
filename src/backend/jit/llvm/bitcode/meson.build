# Copyright (c) 2022-2024, PostgreSQL Global Development Group
#
# emit LLVM bitcode for JIT inlining

assert(llvm.found())

foreach bitcode_module : bitcode_modules
  bitcode_targets = []
  bitcode_obj = bitcode_module['target']
  bitcode_cflags_local = bitcode_cflags + bitcode_module.get('additional_flags', [])

  if 'name' not in bitcode_module
    bitcode_name = bitcode_obj.name()
  else
    bitcode_name = bitcode_module['name']
  endif

  foreach srcfile : bitcode_module['srcfiles']
    #message('File = @0@'.format(srcfile))
    #message('File2 = @0@'.format(srcfile.full_path()))
    if meson.version().version_compare('>=1.3')
      srcfilename = fs.parent(srcfile) / fs.name(srcfile)
    else
      srcfilename = '@0@'.format(srcfile)
    endif
    targetname = '@0@_@1@.bc'.format(
      bitcode_name,
      srcfilename.underscorify()
    )

    bitcode_targets += custom_target(
      targetname,
      depends: [bitcode_module.get('depends', []), bitcode_module['target']],
      input: [srcfile], #, bitcode_obj.extract_objects(srcfile)],
      output: targetname,
      command: [llvm_irgen_command, bitcode_cflags_local],
      install: true,
      install_dir: dir_bitcode,
    )
  endforeach

  index_name = '@0@.index.bc'.format(bitcode_name)
  bitcode_index = custom_target('@0@'.format(bitcode_name),
    output: index_name,
    input: bitcode_targets,
    command: [irlink, '--lto', llvm_lto, '--outdir', '@OUTDIR@', '--index', index_name, '@INPUT@'],
    install: true,
    install_dir: dir_bitcode,
  )
  backend_targets += bitcode_index
endforeach
