if llvm.found()

  llvmjit_sources = []

  # Infrastructure
  llvmjit_sources += files(
    'llvmjit.c',
    'llvmjit_error.cpp',
    'llvmjit_inline.cpp',
    'llvmjit_wrap.cpp',
  )

  # Code generation
  llvmjit_sources += files(
    'llvmjit_deform.c',
    'llvmjit_expr.c',
  )

  llvmjit = shared_module('llvmjit',
    llvmjit_sources,
    dependencies: [backend_code, llvm],
    c_args: [],
    #include_directories: [include_directories('.')],
    link_with: [postgres],
    name_prefix: '',
    install: true)

    # Note this is intentionally is not installed to bitcodedir, as it's not
    # for inlining
    custom_target('llvmjit_types.bc',
      kwargs: llvm_irgen_kw + {
        'input': 'llvmjit_types.c',
        'output': 'llvmjit_types.bc',
        'depends': [postgres],
        'install': true,
        'install_dir': get_option('libdir')
      }
    )

endif
