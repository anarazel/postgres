# Copyright (c) 2022-2023, PostgreSQL Global Development Group
#
# emit LLVM bitcode for JIT inlining

assert(llvm.found())

foreach bitcode_module : bitcode_modules
  bitcode_targets = []
  bitcode_obj = bitcode_module['target']
  if 'name' not in bitcode_module
    bitcode_name = bitcode_obj.name()
  else
    bitcode_name = bitcode_module['name']
  endif

  foreach srcfile : bitcode_module['srcfiles']
    srcfilename = '@0@'.format(srcfile)
    targetname = '@0@_@1@_@2@.bc'.format(
      bitcode_name,
      fs.parent(srcfilename).underscorify(), fs.name(srcfilename))
    bitcode_targets += custom_target(
      targetname,
      input: [srcfile, bitcode_obj.extract_objects(srcfile)],
      output: targetname,
      command: [llvm_irgen_command, bitcode_cflags],
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
