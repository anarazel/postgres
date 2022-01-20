common_sources = files(
  'archive.c',
  'base64.c',
  'checksum_helper.c',
  'config_info.c',
  'controldata_utils.c',
  'encnames.c',
  'exec.c',
  'file_perm.c',
  'file_utils.c',
  'hashfn.c',
  'ip.c',
  'jsonapi.c',
  'keywords.c',
  'kwlookup.c',
  'link-canary.c',
  'md5_common.c',
  'pg_get_line.c',
  'pg_lzcompress.c',
  'pg_prng.c',
  'pgfnames.c',
  'psprintf.c',
  'relpath.c',
  'rmtree.c',
  'saslprep.c',
  'scram-common.c',
  'string.c',
  'stringinfo.c',
  'unicode_norm.c',
  'username.c',
  'wait_error.c',
  'wchar.c',
)

# FIXME: implement openssl
if ssl.found()
  common_sources += files(
    'cryptohash_openssl.c',
    'hmac_openssl.c',
    'protocol_openssl.c',
  )
else
  common_sources += files(
    'cryptohash.c',
    'hmac.c',
    'md5.c',
    'sha1.c',
    'sha2.c',
  )
endif

common_sources += custom_target('kwlist',
  input: files('../include/parser/kwlist.h'),
  output: 'kwlist_d.h',
  command: [perl, '-I', '@SOURCE_ROOT@/src/tools', files('../tools/gen_keywordlist.pl'),
      '--extern', '--output', '@OUTDIR@', '@INPUT@'])


# The code imported from Ryu gets a pass on declaration-after-statement,
# in order to keep it more closely aligned with its upstream.
ryu_sources = files(
  'd2s.c',
  'f2s.c',
)
ryu_cflags = []

if using_declaration_after_statement_warning
  ryu_cflags += ['-Wno-declaration-after-statement']
endif

common_cflags = {'ryu': ryu_cflags}
common_sources_cflags = {'ryu': ryu_sources}


# A few files are currently only built for frontend, not server
# (Mkvcbuild.pm has a copy of this list, too).  logging.c is excluded
# from OBJS_FRONTEND_SHLIB (shared library) as a matter of policy,
# because it is not appropriate for general purpose libraries such
# as libpq to report errors directly.

common_sources_frontend_shlib = common_sources
common_sources_frontend_shlib += files(
  'fe_memutils.c',
  'restricted_token.c',
  'sprompt.c',
)

common_sources_frontend_static = common_sources_frontend_shlib
common_sources_frontend_static += files(
  'logging.c',
)

# Build pgport once for backend, once for use in frontend binaries, and once
# for use in shared libraries
#
# XXX: in most environments we could probably link_whole pgcommon_shlib
# against pgcommon_static, instead of compiling twice.
#
# For the server build of pgcommon, depend on lwlocknames_h, because at least
# cryptohash_openssl.c, hmac_openssl.c depend on it. That's arguably a
# layering violation, but ...
pgcommon = {}
pgcommon_variants = {
  'srv' : internal_lib_args + {
    'sources': common_sources + [lwlocknames_h],
    'dependencies': [backend_common_code],
   },
  'static' : default_lib_args + {
    'sources': common_sources_frontend_static,
    'dependencies': [frontend_common_code],
   },
  'shlib' : default_lib_args + {
    'pic': true,
    'sources': common_sources_frontend_shlib,
    'dependencies': [frontend_common_code],
   },
}

foreach name, opts : pgcommon_variants

  # Build internal static libraries for sets of files that need to be built
  # with different cflags
  cflag_libs = []
  foreach cflagname, sources: common_sources_cflags
    if sources.length() == 0
      continue
    endif
    c_args = opts.get('c_args', []) + common_cflags[cflagname]
    cflag_libs += static_library('pgcommon_@0@_@1@'.format(cflagname, name),
      c_pch: '../include/pch/c_pch.h',
      include_directories: include_directories('.'),
      kwargs: opts + {
        'sources': sources,
        'c_args': c_args,
        'build_by_default': false,
        'install': false,
      },
    )
  endforeach

  lib = static_library('pgcommon_@0@'.format(name),
      link_with: cflag_libs,
      c_pch: '../include/pch/c_pch.h',
      include_directories: include_directories('.'),
      kwargs: opts + {
        'dependencies': opts['dependencies'] + [ssl],
      }
    )
  pgcommon += {name: lib}
endforeach

common_srv = pgcommon['srv']
common_shlib = pgcommon['shlib']
common_static = pgcommon['static']

subdir('unicode')
