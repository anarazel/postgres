# libraries that other subsystems might depend upon first, in their respective
# dependency order

subdir('timezone')

subdir('backend')

subdir('bin')

subdir('pl')

subdir('interfaces')


### Generate a Makefile.global that's complete enough for PGXS to work.
#
# This is somewhat ugly, but allows extensions to use a single buildsystem
# across all the supported postgres versions. Once all supported PG versions
# support meson, we can remove all of this.
#
# XXX: Should we make this optional?

pgxs_kv = {
  'PACKAGE_URL': pg_url,
  'PACKAGE_VERSION': pg_version,
  'PG_MAJORVERSION': pg_version_major,
  'PG_VERSION_NUM': pg_version_num,
  'configure_input': 'meson',

  'vpath_build': 'yes',
  'autodepend': cc.get_argument_syntax() == 'gcc' ? 'yes' : 'no',

  'host_cpu': host_cpu,
  'host': '@0@-@1@'.format(host_cpu, host_system),
  'host_os': host_system,
  'build_os': build_machine.system(),
  'PORTNAME': portname,
  'PG_SYSROOT': pg_sysroot,

  'abs_top_builddir': meson.build_root(),
  'abs_top_srcdir': meson.source_root(),

  'enable_thread_safety': 'yes',
  'enable_rpath': 'yes',
  'enable_nls': libintl.found() ? 'yes' : 'no',
  'enable_tap_tests': tap_tests_enabled ? 'yes' : 'no',
  'enable_debug': get_option('debug') ? 'yes' : 'no',
  'enable_coverage': get_option('b_coverage') ? 'yes' : 'no',
  'enable_dtrace': dtrace.found() ? 'yes' : 'no',

  'DLSUFFIX': dlsuffix,
  'EXEEXT': exesuffix,

  'SUN_STUDIO_CC': 'no', # not supported so far

  # want the chosen option, rather than the library
  'with_ssl' : get_option('ssl'),
  'with_uuid': uuidopt,

  'default_port': get_option('pgport'),
  'with_system_tzdata': get_option('system_tzdata'),

  'with_krb_srvnam': get_option('krb_srvnam'),
  'krb_srvtab': krb_srvtab,

  # FIXME: implement programs.m4 logic in PGAC_CHECK_STRIP
  'STRIP': 'strip',
  'STRIP_STATIC_LIB': 'strip -x',
  'STRIP_SHARED_LIB': 'strip --strip-unneeded',

  # Just always use the install_sh fallback that autoconf uses. Unlikely to
  # matter performance-wise for extensions. If it turns out to do, we can
  # improve that later.
  'MKDIR_P': ' '.join([install_sh.path(), '-d']),

  'CC': var_cc,
  'CPP': var_cpp,
  'GCC': cc.get_argument_syntax() == 'gcc' ? 'yes' : 'no',

  'SOLARIS_EXPORT_DYNAMIC': solaris_export_dynamic ? 'yes' : 'no',

  'CFLAGS': var_cflags,
  'CPPFLAGS': var_cppflags,
  'CXXFLAGS': var_cxxflags,
  'CFLAGS_SL': var_cflags_sl,
  'CFLAGS_SL_MODULE': ' '.join(cflags_mod),
  'CXXFLAGS_SL_MODULE': ' '.join(cxxflags_mod),
  'CFLAGS_CRC': ' '.join(cflags_crc),

  'CFLAGS_UNROLL_LOOPS': ' '.join(unroll_loops_cflags),
  'CFLAGS_VECTORIZE': ' '.join(vectorize_cflags),

  'LDFLAGS': var_ldflags,
  'LDFLAGS_EX': var_ldflags_ex,
  'LDFLAGS_SL': var_ldflags_sl,

  # FIXME:
  'BITCODE_CFLAGS': '',
  'BITCODE_CXXFLAGS': '',

  'BISONFLAGS': ' '.join(bison_flags),
  'FLEXFLAGS': ' '.join(flex_flags),

  'LIBS': var_libs,
}

if llvm.found()
  pgxs_kv += {
    'CLANG': clang.path(),
    'CXX': ' '.join(cpp.cmd_array()),
    'LLVM_BINPATH': llvm_binpath,
  }
else
  pgxs_kv += {
    'CLANG': '',
    'CXX': '',
    'LLVM_BINPATH': '',
  }
endif

pgxs_bins = {
  'BISON': bison,
  'FLEX': flex,
  'GZIP': gzip,
  'LZ4': program_lz4,
  'PERL': perl,
  'PROVE': prove,
  'PYTHON': python,
  'TAR': tar,
  'ZSTD': program_zstd,
  'DTRACE': dtrace,
  'install_bin': install_sh,
}

pgxs_empty = [
  'PERMIT_DECLARATION_AFTER_STATEMENT',
  'ICU_CFLAGS', # needs to be added, included by public server headers

  # probably need most of these?
  'LN_S',
  'AR',
  'AWK',

  # hard to see why we'd need either?
  'ZIC',
  'TCLSH',

  # docs don't seem to be supported by pgxs
  'XMLLINT',
  'XSLTPROC',
  'DBTOEPUB',
  'FOP',

  # supporting coverage for pgxs-in-meson build doesn't seem worth it
  'GENHTML',
  'LCOV',
  'GCOV',
  'MSGFMT_FLAGS',

  # translation doesn't appear to be supported by pgxs
  'MSGFMT',
  'XGETTEXT',
  'MSGMERGE',
  'WANTED_LANGUAGES',

  # Not needed because we don't build the server / PLs with the generated makefile
  'LIBOBJS', 'PG_CRC32C_OBJS', 'TAS',
  'DTRACEFLAGS', # only server has dtrace probes

  'perl_archlibexp', 'perl_embed_ccflags', 'perl_embed_ldflags', 'perl_includespec', 'perl_privlibexp',
  'python_additional_libs', 'python_includespec', 'python_libdir', 'python_libspec', 'python_majorversion', 'python_version',

  # possible that some of these are referenced explicitly in pgxs makefiles?
  # For now not worth it.
  'TCL_INCLUDE_SPEC', 'TCL_LIBS', 'TCL_LIB_SPEC', 'TCL_SHARED_BUILD',

  'LLVM_CFLAGS', 'LLVM_CPPFLAGS', 'LLVM_CXXFLAGS', 'LLVM_LIBS',

  'LDAP_LIBS_BE', 'LDAP_LIBS_FE',

  'UUID_LIBS',

  'PTHREAD_CFLAGS', 'PTHREAD_LIBS',

  'ICU_LIBS',
]

if host_system == 'windows' and cc.get_argument_syntax() != 'msvc'
  pgxs_bins += {'WINDRES': windres}
else
  pgxs_empty += 'WINDRES'
endif

pgxs_dirs = {
  'prefix': get_option('prefix'),

  'bindir': '${exec_prefix}' / get_option('bindir'),
  'datarootdir': '${prefix}' / get_option('datadir'),
  'datadir': '${datarootdir}',
  'docdir': '${prefix}' / dir_doc,
  'exec_prefix': '${prefix}',
  'htmldir': '${prefix}' / dir_doc_html, #?
  'includedir': '${prefix}' / get_option('includedir'),
  'libdir': '${exec_prefix}' / get_option('libdir'),
  'localedir': '${prefix}' / get_option('localedir'),
  'mandir': '${prefix}' / get_option('mandir'),
  'sysconfdir': '${prefix}' / get_option('sysconfdir'),
}

pgxs_deps = {
  'bonjour': bonjour,
  'bsd_auth': bsd_auth,
  'gssapi': gssapi,
  'icu': icu,
  'ldap': ldap,
  'libxml': libxml,
  'libxslt': libxslt,
  'llvm': llvm,
  'lz4': lz4,
  'nls': libintl,
  'pam': pam,
  'perl': perl_dep,
  'python': python3_dep,
  'readline': readline,
  'selinux': selinux,
  'systemd': systemd,
  'tcl': tcl_dep,
  'zlib': zlib,
  'zstd': zstd,
}


pgxs_cdata = configuration_data(pgxs_kv)

foreach b, p : pgxs_bins
  pgxs_cdata.set(b, p.found() ? p.path() : '')
endforeach

foreach pe : pgxs_empty
  pgxs_cdata.set(pe, '')
endforeach

foreach d, p : pgxs_dirs
  pgxs_cdata.set(d, p)
endforeach

foreach d, v : pgxs_deps
  pgxs_cdata.set('with_@0@'.format(d), v.found() ? 'yes' : 'no')
endforeach


makefile_global = configure_file(
  input: 'Makefile.global.in',
  output: 'Makefile.global',
  configuration: pgxs_cdata,
  install: true,
  install_dir: dir_pgxs / 'src',
)
configure_files += makefile_global

makefile_port = configure_file(
  input: 'makefiles' / 'Makefile.@0@'.format(portname),
  output: 'Makefile.port',
  copy: true,
  install_dir: dir_pgxs / 'src')
configure_files += makefile_port

install_data(
  'Makefile.shlib', 'nls-global.mk',
  install_dir: dir_pgxs / 'src')

install_data(
  'makefiles/pgxs.mk',
  install_dir: dir_pgxs / 'src' / 'makefiles')


run_target('headerscheck',
  command: [files('tools/pginclude/headerscheck'), '@SOURCE_ROOT@', '@BUILD_ROOT@'])

run_target('cpluspluscheck',
  command: [files('tools/pginclude/cpluspluscheck'), '@SOURCE_ROOT@', '@BUILD_ROOT@'])
