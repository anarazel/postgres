backend_build_deps = [backend_code]
backend_deps = [dl_dep, thread_dep]
backend_sources = []
backend_link_with = [pgport_srv, common_srv]
backend_c_args = []

generated_backend_sources = []

subdir('access')
subdir('bootstrap')
subdir('catalog')
subdir('commands')
subdir('executor')
subdir('foreign')
subdir('jit')
subdir('lib')
subdir('libpq')
subdir('main')
subdir('nodes')
subdir('optimizer')
subdir('parser')
subdir('partitioning')
subdir('port')
subdir('postmaster')
subdir('regex')
subdir('replication')
subdir('rewrite')
subdir('statistics')
subdir('storage')
subdir('tcop')
subdir('tsearch')
subdir('utils')


postgres_link_args = []
postgres_link_depends = []

if host_machine.system() == 'windows'
  backend_deps += cc.find_library('secur32', required: true)

  if cc.get_id() == 'msvc'
    postgres_link_args += '/STACK:@0@'.format(cdata.get('WIN32_STACK_RLIMIT'))
  else
    postgres_link_args += '-Wl,--stack,@0@'.format(cdata.get('WIN32_STACK_RLIMIT'))
  endif
endif


# On windows when compiling with msvc we need to make postgres export all its
# symbols so that extension libraries can use them. For that we need to scan
# the constituting objects and generate a file specifying all the functions as
# exported (variables need an "import" declaration in the header, hence
# PGDLLEXPORT, but functions work without that, due to import libraries
# basically being trampolines).
#
#
# On meson there's currently no easy way to do this that I found. So we build
# a static library with all the input objects, run our script to generate
# exports, and build the final executable using that static library
#
#
# XXX: This needs to be improved.
#

# NB: There's an outer and inner layer of == windows checks, to allow to
# exercise most of this on !windows, by widening the outer "layer".

if cc.get_id() == 'msvc' # or true

  postgres_lib = static_library('postgres_lib',
    backend_sources + timezone_sources + generated_backend_sources,
    link_whole: backend_link_with,
    c_pch: '../include/pch/postgres_pch.h',
    c_args: backend_c_args,
    implicit_include_directories: false,
    dependencies: backend_build_deps,
    build_by_default: false,
    install: false,
  )

  postgres_def = custom_target('postgres.def',
    command: [perl, files('../tools/msvc/gendef2.pl'), 'x64', '@OUTPUT@', '@PRIVATE_DIR@', '@INPUT@'],
    input: [postgres_lib, common_srv, pgport_srv],
    output: 'postgres.def',
    depends: [postgres_lib, common_srv, pgport_srv],
    install: false,
  )

  if cc.get_id() == 'msvc'
    postgres_link_args += '/DEF:@0@'.format(postgres_def.full_path())
    postgres_link_depends += postgres_def
  endif

  # Unfortunately the msvc linker whines when building an executable with just
  # libraries, hence the reuse of the 'main' object directly.

  postgres = executable('postgres',
    objects: [postgres_lib.extract_objects(main_file)],
    link_with: [postgres_lib],
    link_args: postgres_link_args,
    link_depends: postgres_link_depends,
    dependencies: backend_deps,
    export_dynamic: true,
    implib: true,
    kwargs: default_bin_args,
  )

else

  postgres = executable('postgres',
    backend_sources + generated_backend_sources + timezone_sources,
    c_pch: '../include/pch/postgres_pch.h',
    c_args: backend_c_args,
    link_args: postgres_link_args,
    link_with: backend_link_with,
    export_dynamic: true,
    dependencies: [backend_build_deps, backend_deps],
    kwargs: default_bin_args,
  )

endif

backend_targets += postgres

pg_mod_args = default_mod_args + {
  'dependencies': [backend_mod_code],
  'c_args': [],
  'cpp_args': [],
 }

if cdata.has('HAVE_VISIBILITY_ATTRIBUTE')
  pg_mod_args = pg_mod_args + {
    'c_args': pg_mod_args['c_args'] + ['-fvisibility=hidden'],
    'cpp_args': pg_mod_args['c_args'] + ['-fvisibility=hidden', '-fvisibility-inlines-hidden'],
  }
endif

# Windows / MacOs link shared modules against postgres. To avoid unnecessary
# build-time dependencies on other operating systems, only add it when
# necessary.
if host_machine.system() == 'windows' or host_machine.system() == 'darwin'
   pg_mod_args = pg_mod_args + {'link_with': [postgres]}
endif
if host_machine.system() == 'darwin'
   pg_mod_args = pg_mod_args + {'link_args': ['-bundle_loader', '@0@'.format(postgres.full_path())]}
endif


# Shared modules that, on some OSs, link against the server binary. Only enter
# these after we defined the server build.

subdir('jit/llvm')
subdir('replication/libpqwalreceiver')
subdir('replication/pgoutput')
subdir('snowball')
subdir('utils/mb/conversion_procs')


if llvm.found()

  # custom_target() insists on targetting files into the current
  # directory. But we have files with the same name in different
  # subdirectories.  generators() don't have that problem, but their results
  # are not installable. The irlink command copies the files for us.
  #
  # FIXME: this needs to be in a central place
  #
  # generator and custom_'t accept CustomTargetIndex as 'depends', nor do they
  # like targets with more than one output. However, a custom target accepts
  # them as input without a problem. So we have the below transitive target :(

  transitive_depend_target = custom_target('stamp',
    input: generated_headers + generated_backend_headers + generated_backend_sources,
    output: 'stamp',
    command: [touch, '@OUTPUT@'],
    install: false)

  llvm_gen = generator(llvm_irgen_command,
    arguments: llvm_irgen_args + g_c_args,
    depends: transitive_depend_target,
    depfile: '@BASENAME@.c.bc.d',
    output: ['@PLAINNAME@.bc']
  )

  bc_backend_sources = llvm_gen.process(backend_sources,
    preserve_path_from: meson.current_source_dir())

  postgres_llvm = custom_target('postgres.index.bc',
    kwargs: llvm_irlink_kw + {
      'input': bc_backend_sources,
      'output': ['bitcode'],
      },
  )

  backend_targets += postgres_llvm

endif
