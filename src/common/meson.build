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
  'hex.c',
  'ip.c',
  'jsonapi.c',
  'keywords.c',
  'kwlookup.c',
  'link-canary.c',
  'md5_common.c',
  'pg_get_line.c',
  'pg_lzcompress.c',
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
  input: '../include/parser/kwlist.h',
  output: 'kwlist_d.h',
  command: [perl, '-I', '@SOURCE_ROOT@/src/tools', '@SOURCE_ROOT@/src/tools/gen_keywordlist.pl',
      '--extern', '--output', '@OUTDIR@', '@INPUT@'])


ryu_sources = files(
  'd2s.c',
  'f2s.c',
)


# The code imported from Ryu gets a pass on declaration-after-statement,
# in order to keep it more closely aligned with its upstream.
ryu_cflags = []
if using_declaration_after_statement_warning
  ryu_cflags += ['-Wno-declaration-after-statement']
endif

ryu_srv = static_library('ryu_srv',
  ryu_sources,
  dependencies: [backend_code],
  c_args: ryu_cflags,
  install: false
)

ryu_shlib = static_library('ryu_shlib',
  ryu_sources,
  dependencies: [frontend_code],
  c_args: ryu_cflags,
  install: false
)

ryu_static = static_library('ryu_static',
  ryu_sources,
  dependencies: [frontend_code],
  c_args: ryu_cflags,
  install: false,
)


# A few files are currently only built for frontend, not server
# (Mkvcbuild.pm has a copy of this list, too).  logging.c is excluded
# from OBJS_FRONTEND_SHLIB (shared library) as a matter of policy,
# because it is not appropriate for general purpose libraries such
# as libpq to report errors directly.

frontend_shlib_sources = common_sources
frontend_shlib_sources += files(
  'fe_memutils.c',
  'restricted_token.c',
  'sprompt.c',
)

frontend_static_sources = frontend_shlib_sources
frontend_static_sources += files(
  'logging.c',
)

common_srv = static_library('common_srv',
  common_sources,
  link_with: [ryu_srv],
  dependencies: [backend_code, ssl],
  install: true)

common_shlib = static_library('common_shlib',
  frontend_shlib_sources,
  link_with: [ryu_shlib],
  dependencies: [frontend_code, ssl],
  install: true)

common_static = static_library('common_static',
  frontend_static_sources,
  link_with: [ryu_static],
  dependencies: [frontend_code, ssl],
  install: true)
