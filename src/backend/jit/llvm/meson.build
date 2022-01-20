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
    kwargs: pg_mod_args + {
      'dependencies': pg_mod_args['dependencies'] + [llvm],
    }
  )

  backend_targets += llvmjit

  # Note this is intentionally is not installed to bitcodedir, as it's not
  # for inlining
  llvmjit_types = custom_target('llvmjit_types.bc',
    kwargs: llvm_irgen_kw + {
      'input': 'llvmjit_types.c',
      'output': 'llvmjit_types.bc',
      'depends': [postgres],
      'install': true,
      'install_dir': get_option('libdir')
    }
  )
  backend_targets += llvmjit_types

endif
