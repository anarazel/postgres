backend_libs = []
backend_sources = []
generated_backend_sources = []

subdir('access')
subdir('bootstrap')
subdir('catalog')
subdir('parser')
subdir('commands')
subdir('executor')
subdir('foreign')
subdir('lib')
subdir('libpq')
subdir('main')
subdir('nodes')
subdir('optimizer')
subdir('partitioning')
subdir('port')
subdir('postmaster')
subdir('regex')
subdir('replication')
subdir('rewrite')
subdir('statistics')
subdir('snowball')
subdir('storage')
subdir('tcop')
subdir('tsearch')
subdir('utils')
subdir('jit')


backend_libs += ssl

postgres = executable('postgres',
  backend_sources + generated_backend_sources + timezone_sources,
  link_with: [pgport_srv, common_srv],
  #include_directories: postgres_inc,
  dependencies: [backend_code] + backend_deps + backend_libs,
  export_dynamic: true,
  install: true,
  c_args: ['-DDLSUFFIX=".so"'])

if llvm.found()

  bc_backend_sources = []

  # FIXME: custom_target() insists on targetting files into the
  # current directory. But we have files with the same name in
  # different subdirectories. Add a integer suffix to disambiguate :(.
  #
  #
  # XXX: generators() don't have that problem, but their results are not
  # installable. We could perhaps address that with a custom target that
  # builds the index file, and then copies all the files back?

  i = 0

  #foreach backend_source: backend_sources
  #
  #  input_file = '@0@'.format(backend_source)
  #  output_file = '_'.join(input_file.split('/'))+'.bc'
  #  #warning('in: @0@, out @1@'.format(input_file, output_file))
  #
  #  bc_backend_sources += custom_target(output_file,
  #    kwargs: llvm_irgen_kw + {
  #      'input': backend_source,
  #      'output': output_file,
  #      'depends': [postgres],
  #      'install': true,
  #      'install_dir': get_option('libdir') / 'bitcode/postgres/src/backend'
  #    }
  #  )
  #
  #  i += 1
  #endforeach


  llvm_gen = generator(clang,
    arguments: ['-c', '-o', '@OUTPUT@',
      '-I', '@SOURCE_ROOT@/src/include',
      '-I', '@BUILD_ROOT@/src/include',
      '-I', '@BUILD_ROOT@/src/backend/utils/misc',
      '-I', meson.current_source_dir(),
      '-O2',
      '-DDLSUFFIX=".so"',
      '-Wno-ignored-attributes',
      '-flto=thin', '-emit-llvm', '@INPUT@'],
    depends: [postgres],
    output: ['@PLAINNAME@.bc']
  )

  bc_backend_sources = llvm_gen.process(backend_sources,
    preserve_path_from: meson.current_source_dir())

  #postgres_llvm = custom_target('postgres.index.bc',
  #  kwargs: llvm_irlink_kw + {
  #    'input': bc_backend_sources,
  #    'output': ['postgres.index.bc']}
  #)

  postgres_llvm = custom_target('postgres.index.bc',
    kwargs: llvm_irlink_kw + {
      'input': bc_backend_sources,
      'output': ['bitcode'],
      },
  )

endif
