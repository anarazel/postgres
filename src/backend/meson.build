backend_build_deps = [backend_code]
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

subdir('po', if_found: libintl)


postgres_link_args = []
postgres_link_depends = []

if host_system == 'win32'
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
    c_pch: pch_postgres_h,
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
    export_dynamic: true,
    implib: true,
    kwargs: default_bin_args,
  )

else

  postgres_objs = []
  postgres_tmpl = static_library('postgres_lib',
    backend_sources, timezone_sources, generated_backend_sources,
    link_whole: backend_link_with,
    c_pch: pch_postgres_h,
    c_args: backend_c_args,
    implicit_include_directories: false,
    dependencies: backend_build_deps,
    build_by_default: false,
    install: false,
  )

  # As of 1/2010:
  # The probes.o file is necessary for dtrace support on Solaris, and on recent
  # versions of systemtap.  (Older systemtap releases just produce an empty
  # file, but that's okay.)  However, macOS's dtrace doesn't use it and doesn't
  # even recognize the -G option.  So, build probes.o except on macOS.
  # This might need adjustment as other platforms add dtrace support.
  #
  # On at least linux we don't actually need to pass in all the objects, but
  # at least on FreeBSD and Solaris we have to.
  #
  # XXX: The reason we only use the objects for generated sources is that hits
  # a meson bug. Luckily we don't don't have probes in generated sources...
  if dtrace.found() and host_system != 'darwin'
    postgres_objs += custom_target(
      'probes.o',
      input: ['utils/probes.d', postgres_tmpl.extract_objects(backend_sources, timezone_sources)],
      output : 'probes.o',
      command : [dtrace, '-C', '-G', '-o', '@OUTPUT@', '-s', '@INPUT@'],
      install: false,
    )
  endif

  postgres = executable('postgres',
    postgres_objs,
    objects: postgres_tmpl.extract_all_objects(recursive: false),
    link_args: postgres_link_args,
    link_with: backend_link_with,
    export_dynamic: true,
    dependencies: [backend_build_deps],
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
    'link_args': pg_mod_args['c_args'] + ['-fvisibility=hidden'],
    'c_args': pg_mod_args['c_args'] + ['-fvisibility=hidden'],
    'cpp_args': pg_mod_args['c_args'] + ['-fvisibility=hidden', '-fvisibility-inlines-hidden'],
  }
endif

# Windows / MacOs link shared modules against postgres. To avoid unnecessary
# build-time dependencies on other operating systems, only add it when
# necessary.
if host_system == 'win32' or host_system == 'darwin'
   pg_mod_args = pg_mod_args + {'link_with': [postgres]}
endif
if host_system == 'darwin'
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
  # generators don't accept CustomTargetIndex as input or 'depends', nor do
  # they like targets with more than one output. However, a custom target
  # accepts them as input without a problem. So we have the below transitive
  # target :(

  transitive_depend_target = custom_target('stamp',
    input: generated_headers + generated_backend_headers + generated_backend_sources,
    output: 'stamp',
    command: [touch, '@OUTPUT@'],
    install: false)

  llvm_gen = generator(llvm_irgen_command,
    arguments: llvm_irgen_args,
    depends: transitive_depend_target,
    depfile: '@BASENAME@.c.bc.d',
    output: '@PLAINNAME@.bc',
  )

  bc_backend_sources = llvm_gen.process(backend_sources,
    preserve_path_from: meson.current_source_dir())

  postgres_llvm = custom_target('bitcode',
    output: ['bitcode'],
    input: bc_backend_sources,
    kwargs: llvm_irlink_kw,
  )

  backend_targets += postgres_llvm

endif
