backend_build_deps = [backend_code]
backend_sources = []
backend_link_with = [pgport_srv, common_srv]

generated_backend_sources = []

subdir('access')
subdir('backup')
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


backend_link_args = []
backend_link_depends = []
backend_c_args = []

if host_system == 'win32'
  if cc.get_id() == 'msvc'
    backend_link_args += '/STACK:@0@'.format(cdata.get('WIN32_STACK_RLIMIT'))
  else
    backend_link_args += '-Wl,--stack,@0@'.format(cdata.get('WIN32_STACK_RLIMIT'))
  endif
endif


# On windows when compiling with msvc we need to make postgres export all its
# symbols so that extension libraries can use them. For that we need to scan
# the constituting objects and generate a file specifying all the functions as
# exported (variables need an "import" declaration in the header, hence
# PGDLLEXPORT, but functions work without that, due to import libraries
# basically being trampolines).
#
# For dtrace probes we need to invoke dtrace on all input files, before
# linking the final executable (see more below).
#
#
# On meson there's currently no easy way to do this that I found. So we build
# a static library with all the input objects, run our script to generate
# exports, and build the final executable using that static library
#
# We could do that only if either dtrace or msvc is in use, but it seems
# easier to just always do so.

postgres_lib = static_library('postgres',
  backend_sources + timezone_sources + generated_backend_sources,
  link_whole: backend_link_with,
  c_pch: pch_postgres_h,
  c_args: backend_c_args,
  dependencies: backend_build_deps,
  kwargs: internal_lib_args,
)

if cc.get_id() == 'msvc'
  postgres_def = custom_target('postgres.def',
    command: [perl, files('../tools/msvc/gendef.pl'),
              '--arch', host_cpu,
              '--tempdir', '@PRIVATE_DIR@',
              '--deffile', '@OUTPUT@',
              '@INPUT@'],
    input: [postgres_lib, common_srv, pgport_srv],
    output: 'postgres.def',
    depends: [postgres_lib, common_srv, pgport_srv],
    install: false,
    build_by_default: false,
  )

  backend_link_args += '/DEF:@0@'.format(postgres_def.full_path())
  backend_link_depends += postgres_def
  # Due to the way msvc and meson's precompiled headers implementation
  # interact, we need to have symbols from the full library available. Could
  # be restricted to b_pch=true.
  backend_link_with += postgres_lib
endif

backend_input = []
backend_objs = [postgres_lib.extract_all_objects(recursive: false)]

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
# XXX: The reason we don't use the objects for generated sources is that
# hits a meson bug. Luckily we don't don't have probes in generated
# sources...
if dtrace.found() and host_system != 'darwin'
  backend_input += custom_target(
    'probes.o',
    input: ['utils/probes.d', postgres_lib.extract_objects(backend_sources, timezone_sources)],
    output : 'probes.o',
    command : [dtrace, '-C', '-G', '-o', '@OUTPUT@', '-s', '@INPUT@'],
    install: false,
  )
endif

postgres = executable('postgres',
  backend_input,
  objects: backend_objs,
  link_args: backend_link_args,
  link_with: backend_link_with,
  link_depends: backend_link_depends,
  export_dynamic: true,
  implib: true,
  dependencies: backend_build_deps,
  kwargs: default_bin_args,
)

backend_targets += postgres


pg_mod_args = default_mod_args + {
  'dependencies': [backend_mod_code],
  'c_args': [],
  'cpp_args': [],
  'link_args': [],
  'link_with': [],
}

# only newer versions of meson know not to apply gnu_symbol_visibility=
# inlineshidden to C code as well...
if cdata.has('HAVE_VISIBILITY_ATTRIBUTE')
  pg_mod_args = pg_mod_args + {
    'link_args': pg_mod_args['link_args'] + ['-fvisibility=hidden'],
    'c_args': pg_mod_args['c_args'] + ['-fvisibility=hidden'],
    'cpp_args': pg_mod_args['cpp_args'] + ['-fvisibility=hidden', '-fvisibility-inlines-hidden'],
  }
endif


# Windows / MacOS link shared modules against postgres. To avoid unnecessary
# build-time dependencies on other operating systems, only add it when
# necessary.
if host_system == 'win32' or host_system == 'darwin'
  pg_mod_args += {
    'link_with': pg_mod_args['link_with'] + [postgres],
  }
endif
if host_system == 'darwin'
  pg_mod_args += {
    'link_args': pg_mod_args['link_args'] + ['-bundle_loader', '@0@'.format(postgres.full_path())],
  }
endif


# Shared modules that, on some OSs, link against the server binary. Only enter
# these after we defined the server build.

subdir('jit/llvm')
subdir('replication/libpqwalreceiver')
subdir('replication/pgoutput')
subdir('snowball')
subdir('utils/mb/conversion_procs')



###############################################################
# Define a .pc file that can be used to build server extensions
###############################################################

pg_ext_vars = []
pg_ext_vars_inst = []
pg_ext_vars_uninst = []

pg_ext_cflags = []
pg_ext_libs = [backend_mod_code, thread_dep, ldflags]
pg_ext_subdirs = ['']

# Compute directories to add include directories to the .pc files for.
# This is a bit more complicated due to port/win32 etc.
i = 0
foreach incdir : postgres_inc_d
  if incdir.startswith('src/include')
    subincdir = dir_include_pkg_rel / 'server' / incdir.split('src/include/').get(1, '')
  else
    subincdir = ''
  endif
  pg_ext_subdirs += subincdir

  # Add directories in source / build dir containing headers to cflags for the
  # -uninstalled.pc
  pg_ext_vars_uninst += ['build_inc@0@=-I${prefix}/@1@'.format(i, incdir),
                         'src_inc@0@=-I${srcdir}/@1@'.format(i, incdir)]
  pg_ext_cflags += ['${build_inc@0@}'.format(i),
                    '${src_inc@0@}'.format(i)]

  i += 1
endforeach


# Extension modules should likely also use -fwrapv etc. But it it's a bit odd
# to expose it to a .pc file?
pg_ext_cflags += functional_cflags

if cdata.has('HAVE_VISIBILITY_ATTRIBUTE')
  pg_ext_cflags += ['-fvisibility=hidden']
endif


# Directories for extensions to install into
# XXX: more needed
pg_ext_vars += 'dir_bin=${prefix}/@0@'.format(dir_bin)
pg_ext_vars += 'dir_mod=${prefix}/@0@'.format(dir_lib_pkg)
pg_ext_vars += 'dir_data=${prefix}/@0@'.format(dir_data_extension)


# XXX: Define variables making it easy to define tests, too


# Ensure that external libraries
if host_system == 'win32'
  if cc.get_id() == 'msvc'
	name = 'postgres.exe.lib'
  else
	name = 'libpostgres.exe.a'
  endif
  pg_ext_libs += ['${mod_link_with}']
  pg_ext_vars_inst += 'mod_link_with=@0@'.format('${prefix}' / dir_bin / name)
  pg_ext_vars_uninst += 'mod_link_with=@0@'.format(meson.current_build_dir() / name)
elif host_system == 'darwin'
  pg_ext_libs += ['-bundle_loader', '${mod_link_with}']
  pg_ext_vars_inst += 'mod_link_with=@0@'.format('${prefix}' / dir_bin / 'postgres')
  pg_ext_vars_uninst += 'mod_link_with=@0@'.format(postgres.full_path())
endif

# main .pc to build extensions
pg_ext = pkgconfig.generate(
  name: 'postgresql-extension',
  description: 'PostgreSQL Extension Support',
  url: pg_url,

  subdirs: pg_ext_subdirs,
  libraries: pg_ext_libs,
  extra_cflags: pg_ext_cflags,

  variables: pg_ext_vars + pg_ext_vars_inst,
  uninstalled_variables: pg_ext_vars + pg_ext_vars_uninst,
)

# a .pc depending on the above, but with all our warnings enabled
pkgconfig.generate(
  name: 'postgresql-extension-warnings',
  description: 'PostgreSQL Extension Support - Compiler Warnings',
  requires: 'postgresql-extension',
  url: pg_url,
  extra_cflags: warning_cflags,

  variables: pg_ext_vars + pg_ext_vars_inst,
  uninstalled_variables: pg_ext_vars + pg_ext_vars_uninst,
)


###############################################################
# emit LLVM bitcode of backend code for JIT inlining
###############################################################

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
