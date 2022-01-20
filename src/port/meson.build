pgport_sources = [
  'bsearch_arg.c',
  'chklocale.c',
  'inet_net_ntop.c',
  'noblock.c',
  'path.c',
  'pg_bitutils.c',
  'pg_strong_random.c',
  'pgcheckdir.c',
  'pgmkdirp.c',
  'pgsleep.c',
  'pgstrcasecmp.c',
  'pgstrsignal.c',
  'pqsignal.c',
  'qsort.c',
  'qsort_arg.c',
  'quotes.c',
  'snprintf.c',
  'strerror.c',
  'tar.c',
  'thread.c',
]

if host_machine.system() == 'windows'
  pgport_sources += files(
    'dirmod.c',
    'kill.c',
    'open.c',
    'system.c',
    'win32env.c',
    'win32error.c',
    'win32ntdll.c',
    'win32security.c',
    'win32setlocale.c',
    'win32stat.c',
  )
endif

if cc.get_id() == 'msvc'
  pgport_sources += files(
    'dirent.c',
  )
endif

# Replacement functionality to be built if corresponding configure symbol
# is false
replace_funcs_neg = [
    ['dlopen'],
    ['explicit_bzero'],
    ['fls'],
    ['getaddrinfo'],
    ['getopt'],
    ['getopt_long'],
    ['getpeereid'],
    ['getrusage'],
    ['gettimeofday'],
    ['inet_aton'],
    ['link'],
    ['mkdtemp'],
    ['pread'],
    ['preadv', 'HAVE_DECL_PREADV'],
    ['pwrite'],
    ['pwritev', 'HAVE_DECL_PWRITEV'],
    ['strlcat'],
    ['strlcpy'],
    ['strnlen'],
]

# Replacement functionality to be built if corresponding configure symbol
# is true
replace_funcs_pos = [
  # x86/x64
  ['pg_crc32c_sse42', 'USE_SSE42_CRC32C'],
  ['pg_crc32c_sse42', 'USE_SSE42_CRC32C_WITH_RUNTIME_CHECK', 'crc'],
  ['pg_crc32c_sse42_choose', 'USE_SSE42_CRC32C_WITH_RUNTIME_CHECK'],
  ['pg_crc32c_sb8', 'USE_SSE42_CRC32C_WITH_RUNTIME_CHECK'],

  # arm / aarch64
  ['pg_crc32c_armv8', 'USE_ARMV8_CRC32C'],
  ['pg_crc32c_armv8', 'USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK', 'crc'],
  ['pg_crc32c_armv8_choose', 'USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK'],
  ['pg_crc32c_sb8', 'USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK'],

  # generic fallback
  ['pg_crc32c_sb8', 'USE_SLICING_BY_8_CRC32C'],
]

pgport_cflags = {'crc': cflags_crc}
pgport_sources_cflags = {'crc': []}

foreach f : replace_funcs_neg
  func = f.get(0)
  varname = f.get(1, 'HAVE_@0@'.format(func.to_upper()))
  filename = '@0@.c'.format(func)

  val = '@0@'.format(cdata.get(varname, 'false'))
  if val == 'false' or val == '0'
    pgport_sources += files(filename)
  endif
endforeach

foreach f : replace_funcs_pos
  func = f.get(0)
  varname = f.get(1, 'HAVE_@0@'.format(func.to_upper()))
  filename = '@0@.c'.format(func)

  val = '@0@'.format(cdata.get(varname, 'false'))
  if val == 'true' or val == '1'
    src = files(filename)
    if f.length() > 2
      pgport_sources_cflags += {f[2]: pgport_sources_cflags[f[2]] + src}
    else
      pgport_sources += src
    endif
  endif
endforeach


if (host_machine.system() == 'windows' or host_machine.system() == 'cygwin') and \
  (cc.get_id() != 'msvc' or cc.version().version_compare('<14.0'))

  # Cygwin and (apparently, based on test results) Mingw both
  # have a broken strtof(), so substitute the same replacement
  # code we use with VS2013. That's not a perfect fix, since
  # (unlike with VS2013) it doesn't avoid double-rounding, but
  # we have no better options. To get that, though, we have to
  # force the file to be compiled despite HAVE_STRTOF.
  pgport_sources += files('strtof.c')
  message('On @0@ with compiler @1@ @2@ we will use our strtof wrapper.'.format(
    host_machine.system(), cc.get_id(), cc.version()))
endif

if not cdata.has('HAVE_PTHREAD_BARRIER_WAIT') and host_machine.system() != 'windows'
   pgport_sources += files('pthread_barrier_wait.c')
endif


# Build pgport once for backend, once for use in frontend binaries, and once
# for use in shared libraries
pgport = {}
pgport_variants = {
  'srv' : internal_lib_args + {
    'dependencies': [backend_port_code],
   },
  'static' : default_lib_args + {
    'dependencies': [frontend_port_code],
   },
  'shlib' : default_lib_args + {
    'pic': true,
    'dependencies': [frontend_port_code],
   },
}

foreach name, opts : pgport_variants

  # Build internal static libraries for sets of files that need to be built
  # with different cflags
  cflag_libs = []
  foreach cflagname, sources: pgport_sources_cflags
    if sources.length() == 0
      continue
    endif
    c_args = opts.get('c_args', []) + pgport_cflags[cflagname]
    cflag_libs += static_library('pgport_@0@_@1@'.format(cflagname, name),
      sources,
      c_pch: '../include/pch/c_pch.h',
      kwargs: opts + {
        'c_args': c_args,
        'build_by_default': false,
        'install': false,
      },
    )
  endforeach

  lib = static_library('pgport_@0@'.format(name),
      pgport_sources,
      link_with: cflag_libs,
      c_pch: '../include/pch/c_pch.h',
      kwargs: opts + {
        'dependencies': opts['dependencies'] + [ssl],
      }
    )
  pgport += {name: lib}
endforeach

pgport_srv = pgport['srv']
pgport_static = pgport['static']
pgport_shlib = pgport['shlib']
