project('postgresql',
  ['c'],
  version: '16devel',
  license: 'PostgreSQL',

  # We want < 0.56 for python 3.5 compatibility on old platforms. EPEL for
  # RHEL 7 has 0.55. < 0.54 would require replacing some uses of the fs
  # module, < 0.53 all uses of fs. So far there's no need to go to >=0.56.
  meson_version: '>=0.54',
  default_options: [
    'warning_level=2',
    'b_pch=false',
    'buildtype=release',
  ]
)



###############################################################
# Basic prep
###############################################################

host_system = host_machine.system() == 'windows' ? 'win32' : host_machine.system()
build_system = build_machine.system() == 'windows' ? 'win32' : build_machine.system()

host_cpu = host_machine.cpu_family()
build_cpu = build_machine.cpu_family()

fs = import('fs')
pkgconfig = import('pkgconfig')
if host_system == 'win32'
  windows = import('windows')
endif

thread_dep = dependency('threads')


# It's very easy to get into confusing states when the source directory
# contains an in-place build. E.g. the wrong pg_config.h will be used. So just
# refuse to build in that case.
#
# There's a more elaborate check later, that checks for conflicts around all
# generated files. But we can only do that much further down the line, so this
# quick check seems worth it. Adhering to this advice should clean up the
# conflict, but won't protect against somebody doing make distclean or just
# removing pg_config.h
errmsg_nonclean_base ='''
****
Non-clean source code directory detected.

To build with meson the source tree may not have an in-place, ./configure
style, build configured. You can have both meson and ./configure style builds
for the same source tree by building out-of-source / VPATH with
configure. Alternatively use a separate check out for meson based builds.

@0@
****'''
if fs.exists(meson.current_source_dir() / 'src' / 'include' / 'pg_config.h')
  errmsg_cleanup = 'To clean up, run make maintainer-clean in the source tree.'
  error(errmsg_nonclean_base.format(errmsg_cleanup))
endif



###############################################################
# Version and other metadata
###############################################################

pg_version = meson.project_version()

if pg_version.endswith('devel')
  pg_version_arr = [pg_version.split('devel')[0], '0']
elif pg_version.contains('beta')
  pg_version_arr = [pg_version.split('beta')[0], '0']
elif pg_version.contains('rc')
  pg_version_arr = [pg_version.split('rc')[0], '0']
else
  pg_version_arr = pg_version.split('.')
endif

pg_version_major = pg_version_arr[0].to_int()
pg_version_minor = pg_version_arr[1].to_int()
pg_version_num = (pg_version_major*10000)+pg_version_minor

cc = meson.get_compiler('c')

cdata = configuration_data()

pg_url = 'https://www.postgresql.org/'

cdata.set_quoted('PACKAGE_NAME', 'PostgreSQL')
cdata.set_quoted('PACKAGE_BUGREPORT', 'pgsql-bugs@lists.postgresql.org')
cdata.set_quoted('PACKAGE_URL', pg_url)
cdata.set_quoted('PACKAGE_VERSION', pg_version)
cdata.set_quoted('PACKAGE_STRING', 'PostgreSQL @0@'.format(pg_version))
cdata.set_quoted('PACKAGE_TARNAME', 'postgresql')

pg_version += get_option('extra_version')
cdata.set_quoted('PG_VERSION', pg_version)
cdata.set_quoted('PG_VERSION_STR', 'PostgreSQL @0@ on @1@, compiled by @2@-@3@'.format(
  pg_version, build_cpu, cc.get_id(), cc.version()))
cdata.set_quoted('PG_MAJORVERSION', pg_version_major.to_string())
cdata.set('PG_MAJORVERSION_NUM', pg_version_major)
cdata.set('PG_MINORVERSION_NUM', pg_version_minor)
cdata.set('PG_VERSION_NUM', pg_version_num)
cdata.set_quoted('CONFIGURE_ARGS', '')



###############################################################
# Search paths
#
# NB: Arguments added globally (via the below, or CFLAGS etc) are not taken
# into account for configuration-time checks (so they are more
# isolated). Flags that have to be taken into account for configure checks
# have to be explicitly specified in configure tests.
###############################################################

g_c_args = []
g_c_inc = []
g_l_args = []

if host_system == 'darwin'
  # XXX, should this be required?
  xcrun = find_program('xcrun', native: true, required: true)

  sysroot = run_command(xcrun, '--show-sdk-path', check: true).stdout().strip()
  message('sysroot is >@0@<'.format(sysroot))

  g_c_args += ['-isysroot', sysroot]
  g_l_args += ['-isysroot', sysroot]
endif

if host_system == 'linux' or host_system == 'cygwin'
  g_c_args += '-D_GNU_SOURCE'
endif


g_c_inc += include_directories(get_option('extra_include_dirs'))
g_c_lib = get_option('extra_lib_dirs')

functional_cflags = g_c_args
functional_cxxflags = g_c_args
warning_cflags = []
warning_cxxflags = []

ldflags = g_l_args
ldflags_sl = []

postgres_inc_d = ['src/include']

# Windows replacement headers - need them to be usable during the feature
# tests below.
if host_system == 'win32'
  postgres_inc_d += 'src/include/port/win32'
  if cc.get_id() == 'msvc'
    postgres_inc_d += 'src/include/port/win32_msvc'
  endif
endif

postgres_inc = include_directories(postgres_inc_d)
g_c_inc += postgres_inc



###############################################################
# Program paths
###############################################################

# External programs
perl = find_program(get_option('PERL'), required: true, native: true)
python = find_program(get_option('PYTHON'), required: true, native: true)
flex = find_program(get_option('FLEX'), native: true)
bison = find_program(get_option('BISON'), native: true, version: '>= 1.875')
sed = find_program(get_option('SED'), 'sed', native: true)
prove = find_program(get_option('PROVE'), native: true)
tar = find_program(get_option('TAR'), native: true)
gzip = find_program(get_option('GZIP'), native: true)
program_lz4 = find_program(get_option('LZ4'), native: true, required: false)
touch = find_program('touch', native: true)
program_zstd = find_program(get_option('ZSTD'), native: true, required: false)
dtrace = find_program(get_option('DTRACE'), native: true, required: get_option('dtrace'))
missing = find_program('config/missing', native: true)

# Internal programs
testwrap = find_program('src/tools/testwrap', native: true)

# used by PGXS
install_sh = find_program('config/install-sh', native: true)

bison_flags = []
if bison.found()
  bison_version_c = run_command(bison, '--version', check: true)
  # bison version string helpfully is something like
  # >>bison (GNU bison) 3.8.1<<
  bison_version = bison_version_c.stdout().split(' ')[3].split('\n')[0]
  if bison_version.version_compare('>=3.0')
    bison_flags += ['-Wno-deprecated']
  endif
endif
bison_cmd = [bison, bison_flags, '-o', '@OUTPUT0@']

flex_flags = []
flex_wrapper = files('src/tools/pgflex')
flex_cmd = [python, flex_wrapper, '--builddir', meson.build_root(),
 '--privatedir', '@PRIVATE_DIR@', '--flex', flex, '-o', '@OUTPUT0@']

flex_backup = ['-b']
flex_fix_warning_script = files('src/tools/fix-old-flex-code.pl')
flex_fix_warning = ['--perl', perl, '--fix_warning_script', flex_fix_warning_script]

wget = find_program('wget', required: false, native: true)
wget_flags = ['-O', '@OUTPUT0@', '--no-use-server-timestamps']



###############################################################
# Path to meson (for tests etc)
###############################################################

# NB: this should really be part of meson, see
# https://github.com/mesonbuild/meson/issues/8511
meson_binpath_r = run_command(python, 'src/tools/find_meson', check: true)

if meson_binpath_r.returncode() != 0 or meson_binpath_r.stdout() == ''
  error('huh, could not run find_meson.\nerrcode: @0@\nstdout: @1@\nstderr: @2@'.format(
    meson_binpath_r.returncode(),
    meson_binpath_r.stdout(),
    meson_binpath_r.stderr()))
endif

meson_binpath_s = meson_binpath_r.stdout().split('\n')
meson_binpath_len = meson_binpath_s.length()

if meson_binpath_len < 1
  error('unexpected introspect line @0@'.format(meson_binpath_r.stdout()))
endif

i = 0
meson_impl = ''
meson_binpath = ''
meson_args = []
foreach e : meson_binpath_s
  if i == 0
    meson_impl = e
  elif i == 1
    meson_binpath = e
  else
    meson_args += e
  endif
  i += 1
endforeach

if meson_impl not in ['muon', 'meson']
    error('unknown meson implementation "@0@"'.format(meson_impl))
endif

meson_bin = find_program(meson_binpath, native: true)



###############################################################
# Option Handling
###############################################################

cdata.set('USE_ASSERT_CHECKING', get_option('cassert') ? 1 : false)

cdata.set('BLCKSZ', get_option('blocksize').to_int() * 1024, description:
'''Size of a disk block --- this also limits the size of a tuple. You can set
   it bigger if you need bigger tuples (although TOAST should reduce the need
   to have large tuples, since fields can be spread across multiple tuples).
   BLCKSZ must be a power of 2. The maximum possible value of BLCKSZ is
   currently 2^15 (32768). This is determined by the 15-bit widths of the
   lp_off and lp_len fields in ItemIdData (see include/storage/itemid.h).
   Changing BLCKSZ requires an initdb.''')

cdata.set('XLOG_BLCKSZ', get_option('wal-blocksize') * 1024)
cdata.set('RELSEG_SIZE', get_option('segsize') * 131072)
cdata.set('DEF_PGPORT', get_option('pgport'))
cdata.set_quoted('DEF_PGPORT_STR', get_option('pgport'))
cdata.set_quoted('PG_KRB_SRVNAM', get_option('krb-srvnam'))
if get_option('system-tzdata') != ''
  cdata.set_quoted('SYSTEMTZDIR', get_option('system-tzdata'))
endif



###############################################################
# Directories
###############################################################

# These are set by the equivalent --xxxdir configure options.  We
# append "postgresql" to some of them, if the string does not already
# contain "pgsql" or "postgres", in order to avoid directory clutter.

pkg = 'postgresql'

dir_prefix = get_option('prefix')

dir_bin = get_option('bindir')

dir_data = get_option('datadir')
if not (dir_data.contains('pgsql') or dir_data.contains('postgres'))
  dir_data = dir_data / pkg
endif

dir_sysconf = get_option('sysconfdir')
if not (dir_sysconf.contains('pgsql') or dir_sysconf.contains('postgres'))
  dir_sysconf = dir_sysconf / pkg
endif

dir_lib = get_option('libdir')

dir_lib_pkg = dir_lib
if not (dir_lib_pkg.contains('pgsql') or dir_lib_pkg.contains('postgres'))
  dir_lib_pkg = dir_lib_pkg / pkg
endif

dir_pgxs = dir_lib_pkg / 'pgxs'

dir_include = get_option('includedir')

dir_include_pkg = dir_include
dir_include_pkg_rel = ''
if not (dir_include_pkg.contains('pgsql') or dir_include_pkg.contains('postgres'))
  dir_include_pkg = dir_include_pkg / pkg
  dir_include_pkg_rel = pkg
endif

dir_man = get_option('mandir')

# FIXME: These used to be separately configurable - worth adding?
dir_doc = get_option('datadir') / 'doc' / 'postgresql'
dir_doc_html = dir_doc

dir_locale = get_option('localedir')


# Derived values
dir_bitcode = dir_lib_pkg / 'bitcode'
dir_include_internal = dir_include_pkg / 'internal'
dir_include_server = dir_include_pkg / 'server'
dir_include_extension = dir_include_server / 'extension'
dir_data_extension = dir_data / 'extension'



###############################################################
# Library: bsd-auth
###############################################################

bsd_authopt = get_option('bsd-auth')
bsd_auth = dependency('', required: false)
if cc.check_header('bsd_auth.h', args: g_c_args, required: bsd_authopt)
  cdata.set('USE_BSD_AUTH', 1)
  bsd_auth = declare_dependency()
endif



###############################################################
# Library: bonjour
#
# For now don't search for DNSServiceRegister in a library - only Apple's
# Bonjour implementation, which is always linked, works.
###############################################################

bonjouropt = get_option('bonjour')
bonjour = dependency('', required : false)
if cc.check_header('dns_sd.h', args: g_c_args, required: bonjouropt) \
  and cc.has_function('DNSServiceRegister', args: g_c_args)
  cdata.set('USE_BONJOUR', 1)
  bonjour = declare_dependency()
endif



###############################################################
# Library: GSSAPI
###############################################################

gssapiopt = get_option('gssapi')
krb_srvtab = ''
have_gssapi = false
if not gssapiopt.disabled()
  gssapi = dependency('krb5-gssapi', required: gssapiopt)
  have_gssapi = gssapi.found()

  if not have_gssapi
  elif cc.check_header('gssapi/gssapi.h', args: g_c_args, dependencies: gssapi, required: false)
    cdata.set('HAVE_GSSAPI_GSSAPI_H', 1)
  elif cc.check_header('gssapi.h', args: g_c_args, dependencies: gssapi, required: gssapiopt)
    cdata.set('HAVE_GSSAPI_H', 1)
  else
    have_gssapi = false
  endif

  if not have_gssapi
  elif cc.has_function('gss_init_sec_context',
                       include_directories: g_c_inc, args: g_c_args, dependencies: gssapi)
    cdata.set('ENABLE_GSS', 1)

    krb_srvtab = 'FILE:/@0@/krb5.keytab)'.format(get_option('sysconfdir'))
    cdata.set_quoted('PG_KRB_SRVTAB', krb_srvtab)
  elif gssapiopt.enabled()
    error('''could not find function 'gss_init_sec_context' required for GSSAPI''')
  else
    have_gssapi = false
  endif
endif
if not have_gssapi
  gssapi = dependency('', required : false)
endif



###############################################################
# Library: ldap
###############################################################

ldapopt = get_option('ldap')
if not ldapopt.disabled()

  if host_system == 'win32'
    ldap = cc.find_library('wldap32')
    ldap_r = ldap
  else
    # macos framework dependency is buggy for ldap (one can argue
    # whether it's Apple's or meson's fault), leading to an endless
    # recursion with ldap.h including itself. See
    # https://github.com/mesonbuild/meson/issues/10002
    # Luckily we only need pkg-config support, so the workaround isn't
    # too complicated
    ldap = dependency('ldap', method: 'pkg-config', required: false)

    # Before 2.5 openldap didn't have a pkg-config file..
    if ldap.found()
      ldap_r = ldap
    else
      ldap = cc.find_library('ldap', required: ldapopt, dirs: g_c_lib)
      ldap_r = cc.find_library('ldap_r', required: ldapopt, dirs: g_c_lib)

      # Use ldap_r for FE if available, else assume ldap is thread-safe.
      # On some platforms ldap_r fails to link without PTHREAD_LIBS.
      if ldap.found() and not ldap_r.found()
        ldap_r = ldap
      endif
    endif

    if ldap.found() and cc.has_function('ldap_initialize', args: g_c_args, dependencies: [ldap, thread_dep])
      cdata.set('HAVE_LDAP_INITIALIZE', 1)
    endif

    # If found via cc.find_library() ensure headers are found when using the
    # dependency. On meson < 0.57 one cannot do compiler checks using the
    # dependency returned by declare_dependency(), so we can't do this above.
    if ldap.found() and ldap.type_name() == 'library'
      # declare dependency so that the additional include dir it might reside in
      # is added automatically where necessary
      ldap = declare_dependency(dependencies: ldap,
        include_directories: g_c_inc)
      ldap_r = declare_dependency(dependencies: ldap_r,
        include_directories: g_c_inc)
    endif
  endif

  # FIXME: port autoconf openldap 2.5+ changes, as well as PGAC_LDAP_SAFE

  if ldap.found()
    cdata.set('USE_LDAP', 1)
  endif

else
  ldap = dependency('', required : false)
  ldap_r = ldap
endif



###############################################################
# Library: LLVM
###############################################################

llvmopt = get_option('llvm')
if not llvmopt.disabled()
  add_languages('cpp', required : true, native: false)
  llvm = dependency('llvm', version : '>=3.9', method: 'config-tool', required: llvmopt)

  if llvm.found()

    cdata.set('USE_LLVM', 1)

    cpp = meson.get_compiler('cpp')

    llvm_binpath = llvm.get_variable(configtool: 'bindir')

    ccache = find_program('ccache', native: true, required: false)
    clang = find_program(llvm_binpath / 'clang', required: true)
    llvm_lto = find_program(llvm_binpath / 'llvm-lto', required: true)

    # FIXME: the includes hardcoded here suck
    llvm_irgen_args = [
      '-c', '-o', '@OUTPUT@', '@INPUT@',
      '-flto=thin', '-emit-llvm',
      '-MD', '-MQ', '@OUTPUT@', '-MF', '@DEPFILE@',
      '-I@SOURCE_ROOT@/src/include',
      '-I@BUILD_ROOT@/src/include',
      '-I@BUILD_ROOT@/src/backend/utils/misc',
      '-I@CURRENT_SOURCE_DIR@',
      '-O2',
      '-Wno-ignored-attributes',
      '-Wno-empty-body',
      g_c_args
    ]

    if ccache.found()
      llvm_irgen_command = ccache
      llvm_irgen_args = [clang.path()] + llvm_irgen_args
    else
      llvm_irgen_command = clang
    endif

    llvm_irgen_kw = {
      'command': [llvm_irgen_command] + llvm_irgen_args,
      'depfile': '@BASENAME@.c.bc.d',
    }

    irlink = find_program('src/tools/irlink', native: true)

    llvm_irlink_kw = {
      'command':[
        irlink,
        '--name', 'postgres',
        '--lto', llvm_lto,
        '--outdir', '@OUTPUT0@',
        '--privdir', '@PRIVATE_DIR@',
        '@INPUT@',
      ],
      'install': true,
      'install_dir': dir_lib_pkg,
    }

  endif
else
  llvm = dependency('', required: false)
endif



###############################################################
# Library: icu
###############################################################

if not get_option('icu').disabled()
  icu = dependency('icu-uc', required: get_option('icu').enabled())
  icu_i18n = dependency('icu-i18n', required: get_option('icu').enabled())

  if icu.found()
    cdata.set('USE_ICU', 1)
  endif

else
  icu = dependency('', required : false)
  icu_i18n = dependency('', required : false)
endif



###############################################################
# Library: libxml
###############################################################

libxmlopt = get_option('libxml')
if not libxmlopt.disabled()
  libxml = dependency('libxml-2.0', required: libxmlopt, version: '>= 2.6.23')

  if libxml.found()
    cdata.set('USE_LIBXML', 1)
  endif
else
  libxml = dependency('', required : false)
endif



###############################################################
# Library: libxslt
###############################################################

libxsltopt = get_option('libxslt')
if not libxsltopt.disabled()
  libxslt = dependency('libxslt', required: libxsltopt)

  if libxslt.found()
    cdata.set('USE_LIBXSLT', 1)
  endif
else
  libxslt = dependency('', required : false)
endif



###############################################################
# Library: lz4
###############################################################

lz4opt = get_option('lz4')
if not lz4opt.disabled()
  lz4 = dependency('liblz4', required: lz4opt)

  if lz4.found()
    cdata.set('USE_LZ4', 1)
    cdata.set('HAVE_LIBLZ4', 1)
  endif

else
  lz4 = dependency('', required : false)
endif



###############################################################
# Library: Tcl (for pltcl)
# tclConfig.sh
#
# NB: tclConfig.sh is used in autoconf build for getting
# TCL_SHARED_BUILD, TCL_INCLUDE_SPEC, TCL_LIBS and TCL_LIB_SPEC
# variables. For now we have not seen a need to copy
# that behaviour to the meson build.
###############################################################

tcl_dep = dependency('', required : false)
tclopt = get_option('pltcl')
tcl_version = get_option('tcl_version')
if not tclopt.disabled()

  # via pkg-config
  tcl_dep = dependency(tcl_version, required: false)

  if not tcl_dep.found()
    tcl_dep = cc.find_library(tcl_version,
      required: tclopt,
      dirs: g_c_lib)
  endif

  if not cc.has_header('tcl.h', dependencies: tcl_dep, required: tclopt)
    tcl_dep = dependency('', required: false)
  endif
endif



###############################################################
# Library: pam
###############################################################

pamopt = get_option('pam')
if not pamopt.disabled()
  pam = dependency('pam', required: false)

  if not pam.found()
    pam = cc.find_library('pam', required: pamopt, dirs: g_c_lib)
  endif

  if pam.found()
    pam_header_found = false

    # header file <security/pam_appl.h> or <pam/pam_appl.h> is required for PAM.
    if cc.check_header('security/pam_appl.h', args: g_c_args, dependencies: pam, required: false)
      cdata.set('HAVE_SECURITY_PAM_APPL_H', 1)
      pam_header_found = true
    elif cc.check_header('pam/pam_appl.h', args: g_c_args, dependencies: pam, required: pamopt)
      cdata.set('HAVE_PAM_PAM_APPL_H', 1)
      pam_header_found = true
    endif

    if pam_header_found
      cdata.set('USE_PAM', 1)
    else
      pam = dependency('', required : false)
    endif
  endif
else
  pam = dependency('', required : false)
endif



###############################################################
# Library: Perl (for plperl)
###############################################################

perlopt = get_option('plperl')
perl_dep = dependency('', required: false)

if perlopt.disabled()
  perl_may_work = false
else
  perl_may_work = true

  # First verify that perl has the necessary dependencies installed
  perl_mods = run_command(
    [perl,
     '-MConfig', '-MOpcode', '-MExtUtils::Embed', '-MExtUtils::ParseXS',
     '-e', ''],
    check: false)
  if perl_mods.returncode() != 0
    perl_may_work = false
    perl_msg = 'perl installation does not have the required modules'
  endif

  # Then inquire perl about its configuration
  if perl_may_work
    # FIXME: include copy-edited comments from perl.m4
    perl_conf_cmd = [perl, '-MConfig', '-e', 'print $Config{$ARGV[0]}']
    perlversion = run_command(perl_conf_cmd, 'api_versionstring', check: true).stdout()
    archlibexp = run_command(perl_conf_cmd, 'archlibexp', check: true).stdout()
    privlibexp = run_command(perl_conf_cmd, 'privlibexp', check: true).stdout()
    useshrplib = run_command(perl_conf_cmd, 'useshrplib', check: true).stdout()
    libperl = run_command(perl_conf_cmd, 'libperl', check: true).stdout()

    perl_inc_dir = '@0@/CORE'.format(archlibexp)

    if useshrplib != 'true'
      perl_may_work = false
      perl_msg = 'need a shared perl'
    endif
  endif

  # XXX: should we only add directories that exist? Seems a bit annoying with
  # macos' sysroot stuff...
  #
  # NB: For unknown reasons msys' python doesn't see these paths, despite gcc
  # et al seeing them. So we can't use include_directories(), as that checks
  # file existence.
  if perl_may_work
    # On most platforms, archlibexp is also where the Perl include files live ...
    perl_ccflags = ['-I@0@'.format(perl_inc_dir)]
    # ... but on newer macOS versions, we must use -iwithsysroot to look
    # under sysroot
    if not fs.is_file('@0@/perl.h'.format(perl_inc_dir)) and \
       fs.is_file('@0@@1@/perl.h'.format(sysroot, perl_inc_dir))
      perl_ccflags = ['-iwithsysroot', perl_inc_dir]
    endif
  endif

  # check required headers are present
  if perl_may_work and not \
    cc.has_header('perl.h', args: g_c_args + perl_ccflags, required: false)
    perl_may_work = false
    perl_msg = 'missing perl.h'
  endif

  # Find perl library. This is made more complicated by the fact that the name
  # Config.pm returns isn't directly usable (sometimes lib needs to be chopped
  # off)
  if perl_may_work
    foreach p : ['perl', 'libperl', libperl, libperl.strip('lib'), fs.stem(libperl), fs.stem(libperl).strip('lib')]
      perl_dep_int = cc.find_library(p,
        dirs: ['@0@/CORE'.format(archlibexp)],
        required: false)
      if perl_dep_int.found()
        break
      endif
    endforeach

    if not perl_dep_int.found()
      perl_may_work = false
      perl_msg = 'missing libperl'
    endif
  endif

  if perl_may_work
    perl_ccflags_r = run_command(perl_conf_cmd, 'ccflags', check: true).stdout()
    message('CCFLAGS recommended by Perl: @0@'.format(perl_ccflags_r))

    foreach flag : perl_ccflags_r.split(' ')
      if flag.startswith('-D') and \
        (not flag.startswith('-D_') or flag == '_USE_32BIT_TIME_T')
        perl_ccflags += flag
      endif
    endforeach

    if host_system == 'win32'
      perl_ccflags += ['-DPLPERL_HAVE_UID_GID']
    endif

    message('CCFLAGS for embedding perl: @0@'.format(' '.join(perl_ccflags)))

    ldopts = run_command(perl, '-MExtUtils::Embed', '-e', 'ldopts', check: true).stdout().strip()

    perl_ldopts = []
    foreach ldopt : ldopts.split(' ')
      if ldopt == ''
        continue
      # strawberry perl unhelpfully has that in ldopts
      elif ldopt == '-s'
        continue
      # AIX perl has export file in ldopts
      elif ldopt.startswith('-bE')
        continue
      endif

      perl_ldopts += ldopt.strip('"')
    endforeach

    # FIXME: check if windows handling is necessary

    message('LDFLAGS for embedding perl: "@0@" (ldopts: "@1@")'.format(
      ' '.join(perl_ldopts), ldopts))

    if perl_dep_int.found()
      perl_dep = declare_dependency(
        compile_args: perl_ccflags,
        link_args: perl_ldopts,
        version: perlversion,
      )
    endif
  endif # perl_may_work

  if not perl_may_work
    if perlopt.enabled()
      error('dependency perl failed: @0@'.format(perl_msg))
    else
      message('disabling optional dependency perl: @0@'.format(perl_msg))
    endif
  endif
endif



###############################################################
# Library: Python (for plpython)
###############################################################

pyopt = get_option('plpython')
if not pyopt.disabled()
  pm = import('python')
  python3_inst = pm.find_installation(required: pyopt.enabled())
  python3_dep = python3_inst.dependency(embed: true, required: pyopt.enabled())
  if not cc.check_header('Python.h', dependencies: python3_dep, required: pyopt.enabled())
    python3_dep = dependency('', required: false)
  endif
else
  python3_dep = dependency('', required: false)
endif



###############################################################
# Library: Readline
###############################################################

if not get_option('readline').disabled()
  libedit_preferred = get_option('libedit_preferred')
  # Set the order of readline dependencies
  check_readline_deps = libedit_preferred ? \
    ['libedit', 'readline'] : ['readline', 'libedit']

  foreach readline_dep : check_readline_deps
    readline = dependency(readline_dep, required: false)
    if not readline.found()
      readline = cc.find_library(readline_dep,
        required: get_option('readline').enabled(),
        dirs: g_c_lib)
    endif
    if readline.found()
      break
    endif
  endforeach

  if readline.found()
    cdata.set('HAVE_LIBREADLINE', 1)

    editline_prefix = {
      'header_prefix': 'editline/',
      'flag_prefix': 'EDITLINE_',
    }
    readline_prefix = {
      'header_prefix': 'readline/',
      'flag_prefix': 'READLINE_',
    }
    default_prefix = {
      'header_prefix': '',
      'flag_prefix': '',
    }

    # Set the order of prefixes
    prefixes = libedit_preferred ? \
      [editline_prefix, default_prefix, readline_prefix] : \
      [readline_prefix, default_prefix, editline_prefix]

    at_least_one_header_found = false
    foreach header: ['history', 'readline']
      is_found = false
      foreach prefix : prefixes
        header_file = '@0@@1@.h'.format(prefix['header_prefix'], header)
        # Check history.h and readline.h
        if not is_found and cc.has_header(header_file,
            args: g_c_args, include_directories: g_c_inc,
            dependencies: [readline], required: false)
          if header == 'history'
            history_h = header_file
          else
            readline_h = header_file
          endif
          cdata.set('HAVE_@0@@1@_H'.format(prefix['flag_prefix'], header).to_upper(), 1)
          is_found = true
          at_least_one_header_found = true
        endif
      endforeach
    endforeach

    if not at_least_one_header_found
      error('''readline header not found
If you have @0@ already installed, see see meson-log/meson-log.txt for details on the
failure. It is possible the compiler isn't looking in the proper directory.
Use -Dreadline=false to disable readline support.'''.format(readline_dep))
    endif

    check_funcs = [
      'append_history',
      'history_truncate_file',
      'rl_completion_matches',
      'rl_filename_completion_function',
      'rl_reset_screen_size',
      'rl_variable_bind',
    ]

    foreach func : check_funcs
      cdata.set('HAVE_'+func.to_upper(),
        cc.has_function(func, args: g_c_args, dependencies: [readline]) ? 1 : false)
    endforeach

    check_vars = [
      'rl_completion_suppress_quote',
      'rl_filename_quote_characters',
      'rl_filename_quoting_function',
    ]

    foreach var : check_vars
      cdata.set('HAVE_'+var.to_upper(),
        cc.has_header_symbol(readline_h, var,
          args: g_c_args, include_directories: g_c_inc,
          prefix: '#include <stdio.h>',
          dependencies: [readline]) ? 1 : false)
    endforeach

    # If found via cc.find_library() ensure headers are found when using the
    # dependency. On meson < 0.57 one cannot do compiler checks using the
    # dependency returned by declare_dependency(), so we can't do this above.
    if readline.type_name() == 'library'
      readline = declare_dependency(dependencies: readline,
        include_directories: g_c_inc)
    endif
  endif

  # XXX: Figure out whether to implement mingw warning equivalent
else
  readline = dependency('', required : false)
endif



###############################################################
# Library: selinux
###############################################################

selinux = dependency('', required : false)
selinuxopt = get_option('selinux')
if meson.version().version_compare('>=0.59')
  selinuxopt = selinuxopt.disable_auto_if(host_system != 'linux')
endif
selinux = dependency('libselinux', required: selinuxopt, version: '>= 2.1.10')
cdata.set('HAVE_LIBSELINUX',
  selinux.found() ? 1 : false)



###############################################################
# Library: systemd
###############################################################

systemd = dependency('', required : false)
systemdopt = get_option('systemd')
if meson.version().version_compare('>=0.59')
  systemdopt = systemdopt.disable_auto_if(host_system != 'linux')
endif
systemd = dependency('libsystemd', required: systemdopt)
cdata.set('USE_SYSTEMD',
  systemd.found() ? 1 : false)



###############################################################
# Library: SSL
###############################################################

if get_option('ssl') == 'openssl'

  # Try to find openssl via pkg-config et al, if that doesn't work
  # (e.g. because it's provided as part of the OS, like on FreeBSD), look for
  # the library names that we know about.

  # via pkg-config et al
  ssl = dependency('openssl', required: false)

  # via library + headers
  if not ssl.found()
    ssl_lib = cc.find_library('ssl',
      dirs: g_c_lib,
      header_include_directories: g_c_inc,
      has_headers: ['openssl/ssl.h', 'openssl/err.h'])
    crypto_lib = cc.find_library('crypto',
      dirs: g_c_lib,
      header_include_directories: g_c_inc)
    ssl_int = [ssl_lib, crypto_lib]

    ssl = declare_dependency(dependencies: ssl_int,
                             include_directories: g_c_inc)
  else
    cc.has_header('openssl/ssl.h', args: g_c_args, dependencies: ssl, required: true)
    cc.has_header('openssl/err.h', args: g_c_args, dependencies: ssl, required: true)

    ssl_int = [ssl]
  endif

  check_funcs = [
    ['CRYPTO_new_ex_data', {'required': true}],
    ['SSL_new', {'required': true}],

    # Function introduced in OpenSSL 1.0.2.
    ['X509_get_signature_nid'],

    # Functions introduced in OpenSSL 1.1.0. We used to check for
    # OPENSSL_VERSION_NUMBER, but that didn't work with 1.1.0, because LibreSSL
    # defines OPENSSL_VERSION_NUMBER to claim version 2.0.0, even though it
    # doesn't have these OpenSSL 1.1.0 functions. So check for individual
    # functions.
    ['OPENSSL_init_ssl'],
    ['BIO_get_data'],
    ['BIO_meth_new'],
    ['ASN1_STRING_get0_data'],
    ['HMAC_CTX_new'],
    ['HMAC_CTX_free'],

    # OpenSSL versions before 1.1.0 required setting callback functions, for
    # thread-safety. In 1.1.0, it's no longer required, and CRYPTO_lock()
    # function was removed.
    ['CRYPTO_lock'],
  ]

  foreach c : check_funcs
    func = c.get(0)
    val = cc.has_function(func, args: g_c_args, dependencies: ssl_int)
    required = c.get(1, {}).get('required', false)
    if required and not val
      error('openssl function @0@ is required'.format(func))
    elif not required
      cdata.set('HAVE_'+func.to_upper(), val ? 1 : false)
    endif
  endforeach

  cdata.set('USE_OPENSSL', 1,
            description: 'Define to 1 to build with OpenSSL support. (-Dssl=openssl)')

  cdata.set('OPENSSL_API_COMPAT', '0x10001000L',
            description: '''Define to the OpenSSL API version in use. This avoids deprecation warnings
   from newer OpenSSL versions.''')
else
  ssl = dependency('', required : false)
endif



###############################################################
# Library: uuid
###############################################################

uuidopt = get_option('uuid')
if uuidopt != 'none'
  uuidname = uuidopt.to_upper()
  if uuidopt == 'e2fs'
    uuid = dependency('uuid', required: true)
    uuidfunc = 'uuid_generate'
    uuidheader = 'uuid/uuid.h'
  elif uuidopt == 'bsd'
    # libc should have uuid function
    uuid = declare_dependency()
    uuidfunc = 'uuid_to_string'
    uuidheader = 'uuid.h'
  elif uuidopt == 'ossp'
    uuid = dependency('ossp-uuid', required: true)
    uuidfunc = 'uuid_export'
    uuidheader = 'ossp/uuid.h'
  else
    error('huh')
  endif

  if not cc.has_header_symbol(uuidheader, uuidfunc, args: g_c_args, dependencies: uuid)
    error('uuid library @0@ missing required function @1@'.format(uuidopt, uuidfunc))
  endif
  cdata.set('HAVE_@0@'.format(uuidheader.underscorify().to_upper()), 1)

  cdata.set('HAVE_UUID_@0@'.format(uuidname), 1,
           description: 'Define to 1 if you have @0@ UUID support.'.format(uuidname))
else
  uuid = dependency('', required : false)
endif



###############################################################
# Library: zlib
###############################################################

zlibopt = get_option('zlib')
zlib = dependency('', required : false)
if not zlibopt.disabled()
  zlib_t = dependency('zlib', required: zlibopt)

  if zlib_t.type_name() == 'internal'
    # if fallback was used, we don't need to test if headers are present (they
    # aren't built yet, so we can't test)
    zlib = zlib_t
  elif not zlib_t.found()
    warning('did not find zlib')
  elif not cc.has_header('zlib.h',
    args: g_c_args, include_directories: g_c_inc,
    dependencies: [zlib_t], required: zlibopt.enabled())
    warning('zlib header not found')
  elif not cc.has_type('z_streamp', args: g_c_args, dependencies: [zlib_t], prefix: '#include <zlib.h>')
    if zlibopt.enabled()
      error('zlib version is too old')
    else
      warning('zlib version is too old')
    endif
  else
    zlib = zlib_t
  endif

  if zlib.found()
    cdata.set('HAVE_LIBZ', 1)
  endif
endif



###############################################################
# Library: tap test dependencies
###############################################################

# Check whether tap tests are enabled or not
tap_tests_enabled = false
tapopt = get_option('tap-tests')
if not tapopt.disabled()
  # Checking for perl modules for tap tests
  perl_ipc_run_check = run_command(perl, 'config/check_modules.pl', check: false)
  if perl_ipc_run_check.returncode() != 0
    message(perl_ipc_run_check.stderr().strip())
    if tapopt.enabled()
      error('Additional Perl modules are required to run TAP tests.')
    else
      warning('Additional Perl modules are required to run TAP tests.')
    endif
  else
    tap_tests_enabled = true
  endif
endif



###############################################################
# Library: zstd
###############################################################

zstdopt = get_option('zstd')
if not zstdopt.disabled()
  zstd = dependency('libzstd', required: zstdopt, version: '>=1.4.0')

  if zstd.found()
    cdata.set('USE_ZSTD', 1)
    cdata.set('HAVE_LIBZSTD', 1)
  endif

else
  zstd = dependency('', required : false)
endif



###############################################################
# Compiler tests
###############################################################

# Do we need -std=c99 to compile C99 code?
c99_test = '''
#include <stdbool.h>
#include <complex.h>
#include <tgmath.h>
#include <inttypes.h>

struct named_init_test {
  int a;
  int b;
};

extern void structfunc(struct named_init_test);

int main(int argc, char **argv)
{
  struct named_init_test nit = {
    .a = 3,
    .b = 5,
  };

  for (int loop_var = 0; loop_var < 3; loop_var++)
  {
    nit.a += nit.b;
  }

  structfunc((struct named_init_test){1, 0});

  return nit.a != 0;
}
'''

if not cc.compiles(c99_test, name: 'c99', args: g_c_args)
  if cc.compiles(c99_test, name: 'c99 with -std=c99', args: g_c_args + ['-std=c99'])
    g_c_args += '-std=c99'
    functional_cflags += '-std=c99'
  else
    error('C compiler does not support C99')
  endif
endif

sizeof_long = cc.sizeof('long', args: g_c_args)
cdata.set('SIZEOF_LONG', sizeof_long)
if sizeof_long == 8
  cdata.set('HAVE_LONG_INT_64', 1)
  cdata.set('PG_INT64_TYPE', 'long int')
  cdata.set_quoted('INT64_MODIFIER', 'l')
elif sizeof_long == 4 and cc.sizeof('long long', args: g_c_args) == 8
  cdata.set('HAVE_LONG_LONG_INT_64', 1)
  cdata.set('PG_INT64_TYPE', 'long long int')
  cdata.set_quoted('INT64_MODIFIER', 'll')
else
  error('do not know how to get a 64bit int')
endif

if host_machine.endian() == 'big'
  cdata.set('WORDS_BIGENDIAN', 1)
endif

alignof_types = ['short', 'int', 'long', 'double']
maxalign = 0
foreach t: alignof_types
  align = cc.alignment(t, args: g_c_args)
  if maxalign < align
    maxalign = align
  endif
  cdata.set('ALIGNOF_@0@'.format(t.to_upper()), align)
endforeach
cdata.set('MAXIMUM_ALIGNOF', maxalign)

cdata.set('SIZEOF_VOID_P', cc.sizeof('void *', args: g_c_args))
cdata.set('SIZEOF_SIZE_T', cc.sizeof('size_t', args: g_c_args))


# Check if __int128 is a working 128 bit integer type, and if so
# define PG_INT128_TYPE to that typename.
#
# This currently only detects a GCC/clang extension, but support for other
# environments may be added in the future.
#
# For the moment we only test for support for 128bit math; support for
# 128bit literals and snprintf is not required.
if cc.links('''
  /*
   * We don't actually run this test, just link it to verify that any support
   * functions needed for __int128 are present.
   *
   * These are globals to discourage the compiler from folding all the
   * arithmetic tests down to compile-time constants.  We do not have
   * convenient support for 128bit literals at this point...
   */
  __int128 a = 48828125;
  __int128 b = 97656250;

  int main(void)
  {
      __int128 c,d;
      a = (a << 12) + 1; /* 200000000001 */
      b = (b << 12) + 5; /* 400000000005 */
      /* try the most relevant arithmetic ops */
      c = a * b;
      d = (c + b) / b;
      /* must use the results, else compiler may optimize arithmetic away */
      return d != a+1;
  }''',
  name: '__int128',
  args: g_c_args)

  buggy_int128 = false

  # Use of non-default alignment with __int128 tickles bugs in some compilers.
  # If not cross-compiling, we can test for bugs and disable use of __int128
  # with buggy compilers.  If cross-compiling, hope for the best.
  # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=83925
  if not meson.is_cross_build()
    r =  cc.run('''
    /* This must match the corresponding code in c.h: */
    #if defined(__GNUC__) || defined(__SUNPRO_C) || defined(__IBMC__)
    #define pg_attribute_aligned(a) __attribute__((aligned(a)))
    #endif
    typedef __int128 int128a
    #if defined(pg_attribute_aligned)
    pg_attribute_aligned(8)
    #endif
    ;

    int128a holder;
    void pass_by_val(void *buffer, int128a par) { holder = par; }

    int main(void)
    {
        long int i64 = 97656225L << 12;
        int128a q;
        pass_by_val(main, (int128a) i64);
        q = (int128a) i64;
        return q != holder;
    }''',
    name: '__int128 alignment bug',
    args: g_c_args)
    assert(r.compiled())
    if r.returncode() != 0
      buggy_int128 = true
    endif
  endif

  if not buggy_int128
    cdata.set('PG_INT128_TYPE', '__int128')
    cdata.set('ALIGNOF_PG_INT128_TYPE', cc.alignment('__int128', args: g_c_args))
  endif
endif


# Check if the C compiler knows computed gotos (gcc extension, also
# available in at least clang).  If so, define HAVE_COMPUTED_GOTO.
#
# Checking whether computed gotos are supported syntax-wise ought to
# be enough, as the syntax is otherwise illegal.
if cc.compiles('''
    static inline int foo(void)
    {
      void *labeladdrs[] = {&&my_label};
      goto *labeladdrs[0];
      my_label:
      return 1;
    }''',
    args: g_c_args)
  cdata.set('HAVE_COMPUTED_GOTO', 1)
endif


# Check if the C compiler understands _Static_assert(),
# and define HAVE__STATIC_ASSERT if so.
#
# We actually check the syntax ({ _Static_assert(...) }), because we need
# gcc-style compound expressions to be able to wrap the thing into macros.
if cc.compiles('''
    int main(int arg, char **argv)
    {
        ({ _Static_assert(1, "foo"); });
    }
    ''',
    args: g_c_args)
  cdata.set('HAVE__STATIC_ASSERT', 1)
endif

# We use <stdbool.h> if we have it and it declares type bool as having
# size 1.  Otherwise, c.h will fall back to declaring bool as unsigned char.
if cc.has_type('_Bool', args: g_c_args) \
  and cc.has_type('bool', prefix: '#include <stdbool.h>', args: g_c_args) \
  and cc.sizeof('bool', prefix: '#include <stdbool.h>', args: g_c_args) == 1
  cdata.set('HAVE__BOOL', 1)
  cdata.set('PG_USE_STDBOOL', 1)
endif


# Need to check a call with %m because netbsd supports gnu_printf but emits a
# warning for each use of %m.
printf_attributes = ['gnu_printf', '__syslog__', 'printf']
testsrc = '''
extern void emit_log(int ignore, const char *fmt,...) __attribute__((format(@0@, 2,3)));
static void call_log(void)
{
    emit_log(0, "error: %s: %m", "foo");
}
'''
attrib_error_args = cc.get_supported_arguments('-Werror=format', '-Werror=ignored-attributes')
foreach a : printf_attributes
  if cc.compiles(testsrc.format(a), args: g_c_args + attrib_error_args, name: 'format ' + a)
    cdata.set('PG_PRINTF_ATTRIBUTE', a)
    break
  endif
endforeach

if cc.has_function_attribute('visibility:default') and \
  cc.has_function_attribute('visibility:hidden')
  cdata.set('HAVE_VISIBILITY_ATTRIBUTE', 1)
endif

# Check if various builtins exist. Some builtins are tested separately,
# because we want to test something more complicated than the generic case.
builtins = [
  'bswap16',
  'bswap32',
  'bswap64',
  'clz',
  'ctz',
  'constant_p',
  'frame_address',
  'popcount',
  'unreachable',
]

foreach builtin : builtins
  fname = '__builtin_@0@'.format(builtin)
  if cc.has_function(fname, args: g_c_args)
    cdata.set('HAVE@0@'.format(fname.to_upper()), 1)
  endif
endforeach

# Check if the C compiler understands __builtin_types_compatible_p,
# and define HAVE__BUILTIN_TYPES_COMPATIBLE_P if so.
#
# We check usage with __typeof__, though it's unlikely any compiler would
# have the former and not the latter.
if cc.compiles('''
    static int x;
    static int y[__builtin_types_compatible_p(__typeof__(x), int)];
    ''',
    name: '__builtin_types_compatible_p',
    args: g_c_args)
  cdata.set('HAVE__BUILTIN_TYPES_COMPATIBLE_P', 1)
endif

# Check if the C compiler understands __builtin_$op_overflow(),
# and define HAVE__BUILTIN_OP_OVERFLOW if so.
#
# Check for the most complicated case, 64 bit multiplication, as a
# proxy for all of the operations.  To detect the case where the compiler
# knows the function but library support is missing, we must link not just
# compile, and store the results in global variables so the compiler doesn't
# optimize away the call.
if cc.links('''
    INT64 a = 1;
    INT64 b = 1;
    INT64 result;

    int main(void)
    {
        return __builtin_mul_overflow(a, b, &result);
    }''',
    name: '__builtin_mul_overflow',
    args: g_c_args + ['-DINT64=@0@'.format(cdata.get('PG_INT64_TYPE'))],
    )
  cdata.set('HAVE__BUILTIN_OP_OVERFLOW', 1)
endif

# XXX: The configure.ac check for __cpuid() is broken, we don't copy that
# here. To prevent problems due to two detection methods working, stop
# checking after one.
if cc.links('''
    #include <cpuid.h>
    int main(int arg, char **argv)
    {
        unsigned int exx[4] = {0, 0, 0, 0};
        __get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
    }
    ''', name: '__get_cpuid',
    args: g_c_args)
  cdata.set('HAVE__GET_CPUID', 1)
elif cc.links('''
    #include <intrin.h>
    int main(int arg, char **argv)
    {
        unsigned int exx[4] = {0, 0, 0, 0};
        __cpuid(exx, 1);
    }
    ''', name: '__cpuid',
    args: g_c_args)
  cdata.set('HAVE__CPUID', 1)
endif



###############################################################
# Compiler flags
###############################################################

common_functional_flags = [
  # Disable strict-aliasing rules; needed for gcc 3.3+
  '-fno-strict-aliasing',
  # Disable optimizations that assume no overflow; needed for gcc 4.3+
  '-fwrapv',
  '-fexcess-precision=standard'
]

functional_cflags += cc.get_supported_arguments(common_functional_flags)
if llvm.found()
  functional_cxxflags += cpp.get_supported_arguments(common_functional_flags)
endif

vectorize_cflags = cc.get_supported_arguments(['-ftree-vectorize'])
unroll_loops_cflags = cc.get_supported_arguments(['-funroll-loops'])

common_warning_flags = [
  '-Wmissing-prototypes',
  '-Wpointer-arith',
  # Really don't want VLAs to be used in our dialect of C
  '-Werror=vla',
  # On macOS, complain about usage of symbols newer than the deployment target
  '-Werror=unguarded-availability-new',
  '-Wendif-labels',
  '-Wmissing-format-attribute',
  '-Wimplicit-fallthrough=3',
  '-Wcast-function-type',
  # This was included in -Wall/-Wformat in older GCC versions
  '-Wformat-security',
]

warning_cflags += cc.get_supported_arguments(common_warning_flags)
if llvm.found()
  warning_cxxflags += cpp.get_supported_arguments(common_warning_flags)
endif

# A few places with imported code get a pass on -Wdeclaration-after-statement, remember
# the result for them
if cc.has_argument('-Wdeclaration-after-statement')
  warning_cflags += '-Wdeclaration-after-statement'
  using_declaration_after_statement_warning = true
else
  using_declaration_after_statement_warning = false
endif


# The following tests want to suppress various unhelpful warnings by adding
# -Wno-foo switches.  But gcc won't complain about unrecognized -Wno-foo
# switches, so we have to test for the positive form and if that works,
# add the negative form.

negative_warning_flags = [
  # Suppress clang's unhelpful unused-command-line-argument warnings.
  'unused-command-line-argument',

  # Remove clang 12+'s compound-token-split-by-macro, as this causes a lot
  # of warnings when building plperl because of usages in the Perl headers.
  'compound-token-split-by-macro',

  # Similarly disable useless truncation warnings from gcc 8+
  'format-truncation',
  'stringop-truncation',

  # FIXME: from andres's local config
  'clobbered',
  'missing-field-initializers',
  'sign-compare',
  'unused-parameter',
]

foreach w : negative_warning_flags
  if cc.has_argument('-W'+w)
    warning_cflags += '-Wno-'+w
  endif
  if llvm.found() and cpp.has_argument('-W'+w)
    warning_cxxflags += '-Wno-'+w
  endif
endforeach

# FIXME: copy "Defend against clang being used on x86-32 without SSE2 enabled"

# From Project.pm
if cc.get_id() == 'msvc'
  warning_cflags += [
    '/wd4018', '/wd4244', '/wd4273', '/wd4101', '/wd4102', '/wd4090',
    '/wd4267',
  ]

  # Dirty hack to disable warnings due to some files defining
  # UMDF_USING_NTSTATUS before including postgres.h. We need a better fix, but
  # for now this allows us to see "real" warnings.
  if get_option('b_pch')
    warning_cflags += '/wd4005'
  endif

  functional_cflags += [
    '/DWIN32', '/DWINDOWS', '/D__WINDOWS__', '/D__WIN32__',
    '/DWIN32_STACK_RLIMIT=4194304', '/D_CRT_SECURE_NO_DEPRECATE',
    '/D_CRT_NONSTDC_NO_DEPRECATE',
  ]
endif


###############################################################
# Atomics
###############################################################

if not get_option('spinlocks')
  warning('Not using spinlocks will cause poor performance')
else
  cdata.set('HAVE_SPINLOCKS', 1)
endif

if not get_option('atomics')
  warning('Not using atomics will cause poor performance')
else
  # XXX: perhaps we should require some atomics support in this case these
  # days?
  cdata.set('HAVE_ATOMICS', 1)

  atomic_checks = [
    {'name': 'HAVE_GCC__SYNC_CHAR_TAS',
     'desc': '__sync_lock_test_and_set(char)',
     'test': '''
char lock = 0;
__sync_lock_test_and_set(&lock, 1);
__sync_lock_release(&lock);'''},

    {'name': 'HAVE_GCC__SYNC_INT32_TAS',
     'desc': '__sync_lock_test_and_set(int32)',
     'test': '''
int lock = 0;
__sync_lock_test_and_set(&lock, 1);
__sync_lock_release(&lock);'''},

    {'name': 'HAVE_GCC__SYNC_INT32_CAS',
     'desc': '__sync_val_compare_and_swap(int32)',
     'test': '''
int val = 0;
__sync_val_compare_and_swap(&val, 0, 37);'''},

    {'name': 'HAVE_GCC__SYNC_INT64_CAS',
     'desc': '__sync_val_compare_and_swap(int64)',
     'test': '''
INT64 val = 0;
__sync_val_compare_and_swap(&val, 0, 37);'''},

    {'name': 'HAVE_GCC__ATOMIC_INT32_CAS',
     'desc': ' __atomic_compare_exchange_n(int32)',
     'test': '''
int val = 0;
int expect = 0;
__atomic_compare_exchange_n(&val, &expect, 37, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);'''},

    {'name': 'HAVE_GCC__ATOMIC_INT64_CAS',
     'desc': ' __atomic_compare_exchange_n(int64)',
     'test': '''
INT64 val = 0;
INT64 expect = 0;
__atomic_compare_exchange_n(&val, &expect, 37, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);'''},
  ]

  foreach check : atomic_checks
    test = '''
int main(void)
{
@0@
}'''.format(check['test'])

    cdata.set(check['name'],
      cc.links(test,
        name: check['desc'],
        args: g_c_args + ['-DINT64=@0@'.format(cdata.get('PG_INT64_TYPE'))]) ? 1 : false
    )
  endforeach

endif



###############################################################
# Select CRC-32C implementation.
#
# If we are targeting a processor that has Intel SSE 4.2 instructions, we can
# use the special CRC instructions for calculating CRC-32C. If we're not
# targeting such a processor, but we can nevertheless produce code that uses
# the SSE intrinsics, perhaps with some extra CFLAGS, compile both
# implementations and select which one to use at runtime, depending on whether
# SSE 4.2 is supported by the processor we're running on.
#
# Similarly, if we are targeting an ARM processor that has the CRC
# instructions that are part of the ARMv8 CRC Extension, use them. And if
# we're not targeting such a processor, but can nevertheless produce code that
# uses the CRC instructions, compile both, and select at runtime.
###############################################################

have_optimized_crc = false
cflags_crc = []
if host_cpu == 'x86' or host_cpu == 'x86_64'

  if cc.get_id() == 'msvc'
    cdata.set('USE_SSE42_CRC32C', false)
    cdata.set('USE_SSE42_CRC32C_WITH_RUNTIME_CHECK', 1)
    have_optimized_crc = true
  else

    prog = '''
#include <nmmintrin.h>

int main(void)
{
    unsigned int crc = 0;
    crc = _mm_crc32_u8(crc, 0);
    crc = _mm_crc32_u32(crc, 0);
    /* return computed value, to prevent the above being optimized away */
    return crc == 0;
}
'''

    if cc.links(prog, name: '_mm_crc32_u8 and _mm_crc32_u32 without -msse4.2', args: g_c_args)
      # Use Intel SSE 4.2 unconditionally.
      cdata.set('USE_SSE42_CRC32C', 1)
      have_optimized_crc = true
    elif cc.links(prog, name: '_mm_crc32_u8 and _mm_crc32_u32 with -msse4.2', args: g_c_args + ['-msse4.2'])
      # Use Intel SSE 4.2, with runtime check. The CPUID instruction is needed for
      # the runtime check.
      cflags_crc += '-msse4.2'
      cdata.set('USE_SSE42_CRC32C', false)
      cdata.set('USE_SSE42_CRC32C_WITH_RUNTIME_CHECK', 1)
      have_optimized_crc = true
    endif

  endif

elif host_cpu == 'arm' or host_cpu == 'aarch64'

  prog = '''
#include <arm_acle.h>

int main(void)
{
    unsigned int crc = 0;
    crc = __crc32cb(crc, 0);
    crc = __crc32ch(crc, 0);
    crc = __crc32cw(crc, 0);
    crc = __crc32cd(crc, 0);

    /* return computed value, to prevent the above being optimized away */
    return crc == 0;
}
'''

  if cc.links(prog, name: '__crc32cb, __crc32ch, __crc32cw, and __crc32cd without -march=armv8-a+crc',
      args: g_c_args)
    # Use ARM CRC Extension unconditionally
    cdata.set('USE_ARMV8_CRC32C', true)
    have_optimized_crc = true
  elif cc.links(prog, name: '__crc32cb, __crc32ch, __crc32cw, and __crc32cd with -march=armv8-a+crc',
      args: g_c_args + ['-march=armv8-a+crc'])
    # Use ARM CRC Extension, with runtime check
    cflags_crc += '-march=armv8-a+crc'
    cdata.set('USE_ARMV8_CRC32C', false)
    cdata.set('USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK', 1)
    have_optimized_crc = true
  endif
endif

if not have_optimized_crc
  # fall back to slicing-by-8 algorithm, which doesn't require any special CPU
  # support.
  cdata.set('USE_SLICING_BY_8_CRC32C', 1)
endif



###############################################################
# Other CPU specific stuff
###############################################################

if host_cpu == 'x86_64'

  if cc.compiles('''
      void main(void)
      {
          long long x = 1; long long r;
          __asm__ __volatile__ (" popcntq %1,%0\n" : "=q"(r) : "rm"(x));
      }''',
      name: '@0@: popcntq instruction'.format(host_cpu),
      args: g_c_args)
    cdata.set('HAVE_X86_64_POPCNTQ', 1)
  endif

elif host_cpu == 'ppc' or host_cpu == 'ppc64'
  # Check if compiler accepts "i"(x) when __builtin_constant_p(x).
  if cdata.has('HAVE__BUILTIN_CONSTANT_P')
    if cc.compiles('''
      static inline int
      addi(int ra, int si)
      {
          int res = 0;
          if (__builtin_constant_p(si))
              __asm__ __volatile__(
                  " addi %0,%1,%2\n" : "=r"(res) : "b"(ra), "i"(si));
          return res;
      }
      int test_adds(int x) { return addi(3, x) + addi(x, 5); }
      ''',
      args: g_c_args)
      cdata.set('HAVE_I_CONSTRAINT__BUILTIN_CONSTANT_P', 1)
    endif
  endif
endif



###############################################################
# Library / OS tests
###############################################################

# XXX: Might be worth conditioning some checks on the OS, to avoid doing
# unnecessary checks over and over, particularly on windows.
header_checks = [
  'atomic.h',
  'copyfile.h',
  'crtdefs.h',
  'execinfo.h',
  'getopt.h',
  'ifaddrs.h',
  'langinfo.h',
  'mbarrier.h',
  'net/if.h',
  'netinet/tcp.h',
  'stdbool.h',
  'strings.h',
  'sys/epoll.h',
  'sys/event.h',
  'sys/personality.h',
  'sys/prctl.h',
  'sys/procctl.h',
  'sys/signalfd.h',
  'sys/sockio.h',
  'sys/ucred.h',
  'termios.h',
  'ucred.h',
]

foreach header : header_checks
  varname = 'HAVE_'+header.underscorify().to_upper()

  # Emulate autoconf behaviour of not-found->undef, found->1
  found = cc.has_header(header, include_directories: g_c_inc, args: g_c_args)
  cdata.set(varname, found ? 1 : false,
            description: 'Define to 1 if you have the <@0@> header file.'.format(header))
endforeach


decl_checks = [
  ['F_FULLFSYNC', 'fcntl.h'],
  ['fdatasync', 'unistd.h'],
  ['posix_fadvise', 'fcntl.h'],
  ['strlcat', 'string.h'],
  ['strlcpy', 'string.h'],
  ['strnlen', 'string.h'],
]

# Need to check for function declarations for these functions, because
# checking for library symbols wouldn't handle deployment target
# restrictions on macOS
decl_checks += [
  ['preadv', 'sys/uio.h'],
  ['pwritev', 'sys/uio.h'],
]

foreach c : decl_checks
  func = c.get(0)
  header = c.get(1)
  args = c.get(2, {})
  varname = 'HAVE_DECL_'+func.underscorify().to_upper()

  found = cc.has_header_symbol(header, func,
    args: g_c_args, include_directories: g_c_inc,
    kwargs: args)
  cdata.set10(varname, found, description:
'''Define to 1 if you have the declaration of `@0@', and to 0 if you
   don't.'''.format(func))
endforeach



if cc.has_type('struct cmsgcred',
    args: g_c_args + ['@0@'.format(cdata.get('HAVE_SYS_UCRED_H')) == 'false' ? '' : '-DHAVE_SYS_UCRED_H'],
    include_directories: g_c_inc,
    prefix: '''
#include <sys/socket.h>
#include <sys/param.h>
#ifdef HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif''')
  cdata.set('HAVE_STRUCT_CMSGCRED', 1)
else
  cdata.set('HAVE_STRUCT_CMSGCRED', false)
endif

if cc.has_type('struct option',
    args: g_c_args, include_directories: g_c_inc,
    prefix: '@0@'.format(cdata.get('HAVE_GETOPT_H')) == '1' ? '#include <getopt.h>' : '')
  cdata.set('HAVE_STRUCT_OPTION', 1)
endif


foreach c : ['opterr', 'optreset']
  varname = 'HAVE_INT_'+c.underscorify().to_upper()

  if cc.links('''
#include <unistd.h>
int main(void)
{
    extern int @0@;
    @0@ = 1;
}
'''.format(c), name: c, args: g_c_args)
    cdata.set(varname, 1)
  else
    cdata.set(varname, false)
  endif
endforeach

if cc.has_type('socklen_t',
    args: g_c_args, include_directories: g_c_inc,
    prefix: '''
#include <sys/socket.h>''')
  cdata.set('HAVE_SOCKLEN_T', 1)
endif

# Check the members of `struct sockaddr_storage'.  We need to know about
# ss_family and ss_len.  (Some platforms follow RFC 2553 and call them
# __ss_family and __ss_len.)  We also check struct sockaddr's sa_len;
# if we have to define our own `struct sockaddr_storage', this tells us
# whether we need to provide an ss_len field.
foreach member : ['ss_family', '__ss_family', 'ss_len', '__ss_len']
  if cc.has_member('struct sockaddr_storage', member,
                   args: g_c_args, include_directories: g_c_inc,
                   prefix: '''
    #include <sys/types.h>
    #include <sys/socket.h>''')
    cdata.set('HAVE_STRUCT_SOCKADDR_STORAGE_'+member.to_upper(), 1)
  endif
endforeach
if cc.has_member('struct sockaddr', 'sa_len',
    args: g_c_args, include_directories: g_c_inc,
    prefix: '''
#include <sys/types.h>
#include <sys/socket.h>''')
  cdata.set('HAVE_STRUCT_SOCKADDR_SA_LEN', 1)
endif

if cc.has_type('struct sockaddr_in6',
    args: g_c_args, include_directories: g_c_inc,
    prefix: '''
#include <netinet/in.h>''')
  cdata.set('HAVE_IPV6', 1)
endif


if cc.has_member('struct tm', 'tm_zone',
    args: g_c_args, include_directories: g_c_inc,
    prefix: '''
#include <sys/types.h>
#include <time.h>
''')
  cdata.set('HAVE_STRUCT_TM_TM_ZONE', 1)
endif

if cc.compiles('''
#include <time.h>
extern int foo(void);
int foo(void)
{
    return timezone / 60;
}
''',
    name: 'Check if the global variable `timezone\' exists',
    args: g_c_args, include_directories: g_c_inc)
  cdata.set('HAVE_INT_TIMEZONE', 1)
else
  cdata.set('HAVE_INT_TIMEZONE', false)
endif

if cc.has_type('union semun',
    args: g_c_args,
    include_directories: g_c_inc,
    prefix: '''
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
''')
  cdata.set('HAVE_UNION_SEMUN', 1)
endif

if cc.compiles('''
#include <string.h>
int main(void)
{
  char buf[100];
  switch (strerror_r(1, buf, sizeof(buf)))
  { case 0: break; default: break; }
}''',
    name: 'strerror_r',
    args: g_c_args, include_directories: g_c_inc)
  cdata.set('STRERROR_R_INT', 1)
else
  cdata.set('STRERROR_R_INT', false)
endif

# Check for the locale_t type and find the right header file.  macOS
# needs xlocale.h; standard is locale.h, but glibc also has an
# xlocale.h file that we should not use.
if cc.has_type('locale_t', prefix: '#include <locale.h>')
  cdata.set('HAVE_LOCALE_T', 1)
elif cc.has_type('locale_t', prefix: '#include <xlocale.h>')
  cdata.set('HAVE_LOCALE_T', 1)
  cdata.set('LOCALE_T_IN_XLOCALE', 1)
endif

# Check if the C compiler understands typeof or a variant.  Define
# HAVE_TYPEOF if so, and define 'typeof' to the actual key word.
foreach kw : ['typeof', '__typeof__', 'decltype']
  if cc.compiles('''
int main(void)
{
    int x = 0;
    @0@(x) y;
    y = x;
    return y;
}
'''.format(kw),
    name: 'typeof()',
    args: g_c_args, include_directories: g_c_inc)

    cdata.set('HAVE_TYPEOF', 1)
    if kw != 'typeof'
      cdata.set('typeof', kw)
    endif

    break
  endif
endforeach


# MSVC doesn't cope well with defining restrict to __restrict, the
# spelling it understands, because it conflicts with
# __declspec(restrict). Therefore we define pg_restrict to the
# appropriate definition, which presumably won't conflict.
#
# FIXME: We used to check if restrict is available, but it really should these
# days?
#
# FIXME: Historically we allowed platforms to disable restrict in template
# files. Is that really still necessary?
cdata.set('pg_restrict', '__restrict')


cdata.set('MEMSET_LOOP_LIMIT', 1024)


if cc.links('''
#include <machine/vmparam.h>
#include <sys/exec.h>

int main(void)
{
    PS_STRINGS->ps_nargvstr = 1;
    PS_STRINGS->ps_argvstr = "foo";
}
''',
  name: 'PS_STRINGS', args: g_c_args)
  cdata.set('HAVE_PS_STRINGS', 1)
else
  cdata.set('HAVE_PS_STRINGS', false)
endif


m_dep = cc.find_library('m', required : false)

# Most libraries are included only if they demonstrably provide a function we
# need, but libm is an exception: always include it, because there are too
# many compilers that play cute optimization games that will break probes for
# standard functions such as pow().
os_deps = [m_dep]

rt_dep = cc.find_library('rt', required : false)

dl_dep = cc.find_library('dl', required : false)

util_dep = cc.find_library('util', required : false)
posix4_dep = cc.find_library('posix4', required : false)

getopt_dep = cc.find_library('getopt', required : false)
gnugetopt_dep = cc.find_library('gnugetopt', required : false)

execinfo_dep = cc.find_library('execinfo', required : false)

cygipc_dep = cc.find_library('cygipc', required : false)

# XXX: Might be worth conditioning some checks on the OS, to avoid doing
# unnecessary checks over and over, particularly on windows.
func_checks = [
  ['_configthreadlocale'],
  ['backtrace_symbols', {'dependencies': [execinfo_dep]}],
  ['clock_gettime', {'dependencies': [rt_dep, posix4_dep], 'define': false}],
  ['copyfile'],
  ['dlsym', {'dependencies': [dl_dep], 'define': false}],
  ['explicit_bzero'],
  ['fdatasync', {'dependencies': [rt_dep, posix4_dep], 'define': false}], # Solaris
  ['getifaddrs'],
  ['getopt', {'dependencies': [getopt_dep, gnugetopt_dep]}],
  ['getopt_long',{'dependencies': [getopt_dep, gnugetopt_dep]}],
  ['getpeereid'],
  ['getpeerucred'],
  ['inet_aton'],
  ['inet_pton'],
  ['kqueue'],
  ['mbstowcs_l'],
  ['memset_s'],
  ['mkdtemp'],
  ['posix_fadvise'],
  ['posix_fallocate'],
  ['ppoll'],
  ['pstat'],
  ['pthread_is_threaded_np', {'dependencies': [thread_dep]}],
  ['pthread_barrier_wait', {'dependencies': [thread_dep]}],
  ['setproctitle', {'dependencies': [util_dep]}],
  ['setproctitle_fast'],
  ['shm_open', {'dependencies': [rt_dep], 'define': false}],
  ['shm_unlink', {'dependencies': [rt_dep], 'define': false}],
  ['shmget', {'dependencies': [cygipc_dep], 'define': false}],
  ['strchrnul'],
  ['strerror_r', {'dependencies': [thread_dep]}],
  ['strlcat'],
  ['strlcpy'],
  ['strnlen'],
  ['strsignal'],
  ['sync_file_range'],
  ['syncfs'],
  ['uselocale'],
  ['wcstombs_l'],
]

foreach c : func_checks
  func = c.get(0)
  kwargs = c.get(1, {})
  deps = kwargs.get('dependencies', [])

  varname = 'HAVE_'+func.underscorify().to_upper()

  found = cc.has_function(func, args: g_c_args)

  if not found
    foreach dep : deps
      if not dep.found()
        continue
      endif
      found = cc.has_function(func, args: g_c_args,
                              dependencies: [dep])
      if found
        os_deps += dep
        break
      endif
    endforeach
  endif

  if kwargs.get('define', true)
    # Emulate autoconf behaviour of not-found->undef, found->1
    cdata.set(varname, found  ? 1 : false,
              description: 'Define to 1 if you have the `@0@\' function.'.format(func))
  endif
endforeach


if cc.has_function('syslog', args: g_c_args) and cc.check_header('syslog.h', args: g_c_args)
  cdata.set('HAVE_SYSLOG', 1)
endif


exesuffix = ''
export_file_suffix = 'list'
if host_system == 'aix'
  dlsuffix = '.so'
  export_file_format = 'aix'
  export_fmt = '-Wl,-bE:@0@'
elif host_system == 'darwin'
  dlsuffix = '.dylib'
  export_file_format = 'darwin'
  export_fmt = '-exported_symbols_list=@0@'
elif host_system == 'win32'
  exesuffix = '.exe'
  dlsuffix = '.dll'
  export_file_format = 'win'
  export_file_suffix = 'def'
  if cc.get_id() == 'msvc'
    export_fmt = '/DEF:@0@'
  else
    export_fmt = '@0@'
  endif
else
  dlsuffix = '.so'
  export_file_format = 'gnu'
  export_fmt = '-Wl,--version-script=@0@'
endif

gen_export_kwargs = {
  'input': 'exports.txt',
  'output': '@BASENAME@.'+export_file_suffix,
  'command': [perl, files('src/tools/gen_export.pl'),
   '--format', export_file_format,
   '--input', '@INPUT0@', '--output', '@OUTPUT0@'],
  'build_by_default': false,
  'install': false,
}

cdata.set_quoted('DLSUFFIX', dlsuffix)

# FIXME: needs some autoconfiguration
if host_system == 'darwin'
  cdata.set('USE_SYSV_SEMAPHORES', 1)
  cdata.set('USE_SYSV_SHARED_MEMORY', 1)
elif host_system == 'openbsd' or host_system == 'netbsd'
  cdata.set('USE_SYSV_SEMAPHORES', 1)
  cdata.set('USE_SYSV_SHARED_MEMORY', 1)
elif host_system == 'win32'
  cdata.set('USE_WIN32_SEMAPHORES', 1)
  cdata.set('USE_WIN32_SHARED_MEMORY', 1)
else
  cdata.set('USE_UNNAMED_POSIX_SEMAPHORES', 1)
  cdata.set('USE_SYSV_SHARED_MEMORY', 1)
endif


if host_system == 'win32'
  cdata.set('HAVE_IPV6', 1)
  cdata.set('WIN32_STACK_RLIMIT', 4194304)
  cdata.set('HAVE__CONFIGTHREADLOCALE', 1)
endif

if cc.get_id() == 'msvc'
  ldflags += ['/fixed:no','/dynamicbase', '/nxcompat']
endif

if host_system == 'win32'
  os_deps += cc.find_library('ws2_32', required: true)
endif

if host_system == 'win32'
  # M:SRE sets a flag indicating that an object is a shared library. Seems to
  # work in some circumstances without, but required in others.
  ldflags_sl += '-Wl,-bM:SRE'
endif

add_project_arguments(functional_cflags, language: ['c'])
add_project_arguments(warning_cflags, language: ['c'])
add_project_arguments(functional_cxxflags, language: ['cpp'])
add_project_arguments(warning_cxxflags, language: ['cpp'])
add_project_link_arguments(ldflags, language: ['c', 'cpp'])


###############################################################
# Threading
###############################################################

# XXX: About to rely on thread safety in the autoconf build, so not worth
# implementing a fallback.
cdata.set('ENABLE_THREAD_SAFETY', 1)



###############################################################
# NLS / Gettext
###############################################################

i18n = import('i18n')
nlsopt = get_option('nls')
libintl = dependency('', required: false)

if not nlsopt.disabled()
  # otherwise there'd be lots of
  # "Gettext not found, all translation (po) targets will be ignored."
  # warnings if not found.
  msgfmt = find_program('msgfmt', required: nlsopt.enabled(), native: true)

  # meson 0.59 has this wrapped in dependency('int')
  if msgfmt.found() and \
    cc.check_header('libintl.h', args: g_c_args, required: nlsopt)

    # in libc
    if cc.has_function('ngettext')
      libintl = declare_dependency()
    else
      libintl = cc.find_library('intl', has_headers: ['libintl.h'], required: nlsopt)
    endif
  endif

  if libintl.found()
    cdata.set('ENABLE_NLS', 1)
  endif
endif



###############################################################
# Build
###############################################################

# Collect a number of lists of things while recursing through the source
# tree. Later steps then can use those.

test_deps = []
backend_targets = []


# Define the tests to distribute them to the correct test styles later
tests = []


# Default options for targets

# First identify rpaths
bin_install_rpaths = []
lib_install_rpaths = []
mod_install_rpaths = []

# If the host can form relative rpaths, use that to make the installation
# properly relocatable
test_library_path_var = ''
if host_system == 'win32'
  # nothing to do
elif host_system == 'openbsd' or host_system == 'netbsd'
  # openbsd's $ORIGIN doesn't use an absolute path to the binary, but argv[0]
  # (i.e. absolute when invoked with an absolute name, but e.g. not absolute
  # when invoked via PATH search).
  #
  # netbsd patched their meson in a broken way:
  # https://gnats.netbsd.org/cgi-bin/query-pr-single.pl?number=56959
  # until there's a way out of that, just use LD_LIBRARY_PATH.
  test_library_path_var = 'LD_LIBRARY_PATH'
else
  if host_system == 'darwin'
    rpath_var = '@loader_path'
  else
    rpath_var = '$ORIGIN'
  endif

  # PG binaries might need to link to libpq, use relative path to reference
  bin_to_lib = run_command(python, files('src/tools/relpath.py'),
    dir_bin, dir_lib, check: true).stdout().strip()
  bin_install_rpaths += rpath_var / bin_to_lib

  # PG extensions might need to link to libpq, use relative path to reference
  # (often just .)
  mod_to_lib = run_command(python, files('src/tools/relpath.py'),
    dir_lib_pkg, dir_lib, check: true).stdout().strip()
  mod_install_rpaths += rpath_var / mod_to_lib
endif

# add extra_lib_dirs to rpath
bin_install_rpaths += g_c_lib
lib_install_rpaths += g_c_lib
mod_install_rpaths += g_c_lib


# Define arguments for default targets

default_target_args = {
  'implicit_include_directories': false,
  'install': true,
}

default_lib_args = default_target_args + {
  'name_prefix': '',
  'install_rpath': ':'.join(lib_install_rpaths),
}

internal_lib_args = default_lib_args + {
  'build_by_default': false,
  'install': false,
}

default_mod_args = default_lib_args + {
  'name_prefix': '',
  'install_dir': dir_lib_pkg,
  'install_rpath': ':'.join(mod_install_rpaths),
}

default_bin_args = default_target_args + {
  'install_dir': dir_bin,
  'install_rpath': ':'.join(bin_install_rpaths),
}



###
### windows resources related stuff
###

pg_ico = meson.source_root() / 'src' / 'port' / 'win32.ico'
win32ver_rc_in = files('src/port/win32ver.rc.in')
rcgen = find_program('src/tools/rcgen', native: true)

rcgen_cmd = [rcgen, '@INPUT@', '-o', '@OUTPUT@', '--depfile', '@DEPFILE@']
rcgen_bin_cmd = rcgen_cmd + [
  '--VFT_TYPE', 'VFT_APP',
  '--FILEENDING', 'exe',
  '--ICO', pg_ico,
]

rcgen_lib_cmd = rcgen_cmd + [
  '--VFT_TYPE', 'VFT_DLL',
  '--FILEENDING', 'dll',
]

rcgen_inc = [postgres_inc]

rcgen_kw = {
  'input': win32ver_rc_in,
  'output': 'win32ver.rc',
  'depfile': '@PLAINNAME@.d',
}

rccompile_kw = {
  'include_directories': rcgen_inc,
}


# First visit src/include - all targets creating headers are defined
# within. That makes it easy to add the necessary dependencies for the
# subsequent build steps.

# headers that the whole build tree depends on
generated_headers = []
# headers that the backend build depends on
generated_backend_headers = []
# configure_files() output, needs a way of converting to file names
configure_files = []

# generated files that might conflict with a partial in-tree autoconf build
generated_sources = []
# same, for paths that differ between autoconf / meson builds
# elements are [dir, [files]]
generated_sources_ac = {}


subdir('src/include')

subdir('config')

# Then through src/port and src/common, as most other things depend on them

frontend_port_code = declare_dependency(
  compile_args: ['-DFRONTEND'],
  include_directories: [postgres_inc],
  sources: [errcodes],
  dependencies: os_deps,
)

backend_port_code = declare_dependency(
  compile_args: ['-DBUILDING_DLL'],
  include_directories: [postgres_inc],
  sources: [errcodes],
  dependencies: os_deps,
)

subdir('src/port')

frontend_common_code = declare_dependency(
  compile_args: ['-DFRONTEND'],
  include_directories: [postgres_inc],
  sources: generated_headers,
  dependencies: os_deps,
)

backend_common_code = declare_dependency(
  compile_args: ['-DBUILDING_DLL'],
  include_directories: [postgres_inc],
  sources: generated_headers,
)

subdir('src/common')

frontend_shlib_code = declare_dependency(
  compile_args: ['-DFRONTEND'],
  include_directories: [postgres_inc],
  link_args: ldflags_sl,
  link_with: [pgport_shlib, common_shlib],
  sources: generated_headers,
  dependencies: [os_deps, libintl],
)

libpq_deps = [
  frontend_shlib_code,
  thread_dep,

  gssapi,
  ldap,
  libintl,
  ssl,
]

subdir('src/interfaces/libpq')
# fe_utils depends on libpq
subdir('src/fe_utils')

frontend_code = declare_dependency(
  compile_args: ['-DFRONTEND'],
  include_directories: [postgres_inc],
  link_with: [fe_utils, common_static, pgport_static],
  sources: generated_headers,
  dependencies: [os_deps, libintl],
)

backend_both_deps = [
  thread_dep,
  os_deps,

  bsd_auth,
  gssapi,
  icu,
  icu_i18n,
  ldap,
  libintl,
  libxml,
  lz4,
  pam,
  ssl,
  systemd,
  zlib,
  zstd,
]

backend_deps = backend_both_deps

if host_system == 'win32'
  backend_deps += cc.find_library('secur32', required: true)
endif

backend_mod_deps = backend_both_deps

backend_code = declare_dependency(
  compile_args: ['-DBUILDING_DLL'],
  include_directories: [postgres_inc],
  link_with: [],
  sources: generated_headers + generated_backend_headers,
  dependencies: backend_deps,
)

# Note there's intentionally no dependency on pgport/common here - we want the
# symbols from the main binary for extension modules, rather than the
# extension linking separately to pgport/common.
backend_mod_code = declare_dependency(
  compile_args: [],
  include_directories: [postgres_inc],
  link_with: [],
  sources: generated_headers + generated_backend_headers,
  dependencies: backend_mod_deps,
)

# Then through the main sources. That way contrib can have dependencies on
# main sources. Note that this explicitly doesn't enter src/test, right now a
# few regression tests depend on contrib files.

subdir('src')

subdir('contrib')

subdir('src/test')
subdir('src/interfaces/libpq/test')
subdir('src/interfaces/ecpg/test')

subdir('doc/src/sgml')

generated_sources_ac += {'': ['GNUmakefile']}


# If there are any files in the source directory that we also generate in the
# build directory, they might get preferred over the newly generated files,
# e.g. because of a #include "file", which always will search in the current
# directory first.
message('checking for file conflicts between source and build directory')
conflicting_files = []
potentially_conflicting_files_t = []
potentially_conflicting_files_t += generated_headers
potentially_conflicting_files_t += generated_backend_headers
potentially_conflicting_files_t += generated_backend_sources
potentially_conflicting_files_t += generated_sources

potentially_conflicting_files = []

# convert all sources of potentially conflicting files into uniform shape
foreach t : potentially_conflicting_files_t
  potentially_conflicting_files += t.full_path()
endforeach
foreach t : configure_files
  t = '@0@'.format(t)
  potentially_conflicting_files += meson.current_build_dir() / t
endforeach
foreach sub, fnames : generated_sources_ac
  sub = meson.build_root() / sub
  foreach fname: fnames
    potentially_conflicting_files += sub/fname
  endforeach
endforeach

# find and report conflicting files
foreach build_path : potentially_conflicting_files
  build_path = host_system == 'win32' ? fs.as_posix(build_path) : build_path
  # str.replace is in 0.56
  src_path = meson.current_source_dir() / build_path.split(meson.current_build_dir() / '')[1]
  if fs.exists(src_path) or fs.is_symlink(src_path)
    conflicting_files += src_path
    have_conflicts = true
  endif
endforeach
# XXX: Perhaps we should generate a file that would clean these up? The list
# can be long.
if conflicting_files.length() > 0
  errmsg_cleanup = '''
Conflicting files in source directory:
  @0@

The conflicting files need to be removed, either by removing the files listed
above, or by running configure and then make maintainer clean.
'''
  errmsg_cleanup = errmsg_cleanup.format(' '.join(conflicting_files))
  error(errmsg_nonclean_base.format(errmsg_cleanup))
endif


# To make MacOS installation work without a prior make install, even with SIP
# enabled, make rpaths relative after installation. This also makes the
# installation relocatable.
if host_system == 'darwin'
  meson.add_install_script('src/tools/relativize_shared_library_references')
endif



###############################################################
# Test prep
###############################################################

# The determination of where a DESTDIR install points to is ugly, it's somewhat hard
# to combine two absolute paths portably...

prefix = get_option('prefix')

test_prefix = prefix

if fs.is_absolute(get_option('prefix'))
  if host_system == 'win32'
    if prefix.split(':\\').length() == 1
        # just a drive
        test_prefix = ''
    else
        test_prefix = prefix.split(':\\')[1]
    endif
  else
    assert(prefix.startswith('/'))
    test_prefix = './@0@'.format(prefix)
  endif
endif

# DESTDIR for the installation used to run tests in
test_install_destdir = meson.build_root() / 'tmp_install/'
# DESTDIR + prefix appropriately munged
test_install_location = test_install_destdir / test_prefix


meson_install_args = meson_args + ['install'] + {
    'meson': ['--quiet', '--only-changed', '--no-rebuild'],
    'muon': []
}[meson_impl]

test('tmp_install',
    meson_bin, args: meson_install_args ,
    env: {'DESTDIR':test_install_destdir},
    priority: 100,
    timeout: 300,
    is_parallel: false,
    suite: ['setup'])

test_result_dir = meson.build_root() / 'testrun'


# XXX: pg_regress doesn't assign unique ports on windows. To avoid the
# inevitable conflicts from running tests in parallel, hackishly assign
# different ports for different tests.

testport=40000

test_env = environment()

temp_install_bindir = test_install_location / get_option('bindir')
test_env.set('PG_REGRESS', pg_regress.full_path())
test_env.set('REGRESS_SHLIB', regress_module.full_path())

# Test suites that are not safe by default but can be run if selected
# by the user via the whitespace-separated list in variable PG_TEST_EXTRA.
# Export PG_TEST_EXTRA so it can be checked in individual tap tests.
test_env.set('PG_TEST_EXTRA', get_option('PG_TEST_EXTRA'))

# On platforms without $ORIGIN support we need to add the temporary
# installation to the library search path.
if test_library_path_var != ''
  test_env.prepend(test_library_path_var, test_install_location / get_option('libdir'))
endif


###############################################################
# Test Generation
###############################################################

foreach test: tests
  test_default = {
    'name': test['name'],
    'sd': test['sd'],
    'bd': test['bd'],
  }
  # Define all 'pg_regress' style tests
  if 'regress' in test
    t = test_default + test['regress']
    test_command = [
      t.get('test_runner', pg_regress).full_path(),
      '--temp-instance', test_result_dir / t['name'] / 'regress' / 'tmp_check',
      '--inputdir', t.get('inputdir', t['sd']),
      '--expecteddir', t.get('expecteddir', t['sd']),
      '--outputdir', test_result_dir / t['name'] / 'regress',
      '--bindir', '',
      '--dlpath', t['bd'],
      '--max-concurrent-tests=20',
      '--port=@0@'.format(testport),
    ]

    if t.has_key('regress_args')
      test_command += t['regress_args']
    endif

    if t.has_key('schedule')
      test_command += ['--schedule', t['schedule'],]
    endif
    if t.has_key('sql')
      test_command += t['sql']
    endif

    env = test_env
    env.prepend('PATH', temp_install_bindir, t['bd'])

    test_kwargs = {
      'suite': [t['name']],
      'priority': 10,
      'timeout': 1000,
      'depends': test_deps + t.get('deps', []),
      'env': env,
      'args': [
        testwrap.path(),
        '--srcdir', t['sd'],
        '--basedir', meson.build_root(),
        '--testgroup', t['name'],
        '--testname', 'regress',
        test_command,
      ]
    }

    if t.has_key('test_kwargs')
      test_kwargs += t['test_kwargs']
    endif

    test(t['name'] / 'regress',
      python,
      kwargs: test_kwargs,
    )

    testport = testport + 1
  endif

  # Define all 'isolationtester' style tests
  if 'isolation' in test
    t = test_default + test['isolation']
    test_command = [
      pg_isolation_regress.full_path(),
      '--temp-instance', test_result_dir / t['name'] / 'isolation' / 'tmp_check',
      '--inputdir', t['sd'],
      '--outputdir', test_result_dir / t['name'] / 'isolation',
      '--bindir', '',
      '--dlpath', t['bd'],
      '--max-concurrent-tests=20',
      '--port=@0@'.format(testport),
    ]

    if t.has_key('regress_args')
      test_command += t['regress_args']
    endif

    if t.has_key('schedule')
      test_command += ['--schedule', t['schedule'],]
    else
      test_command += t['specs']
    endif

    env = test_env
    env.prepend('PATH', temp_install_bindir, t['bd'])

    test_kwargs = {
      'suite': [t['name']],
      'priority': 20,
      'timeout': 1000,
      'depends': test_deps + t.get('deps', []),
      'env': env,
      'args': [
        testwrap.path(),
        '--srcdir', t['sd'],
        '--basedir', meson.build_root(),
        '--testgroup', t['name'],
        '--testname', 'isolation',
        test_command,
      ]
    }

    if t.has_key('test_kwargs')
      test_kwargs += t['test_kwargs']
    endif

    test(t['name'] / 'isolation',
      python,
      kwargs: test_kwargs,
    )

    testport = testport + 1
  endif

  # Define all 'tap' style tests
  # FIXME: dependencies for each test
  if tap_tests_enabled and 'tap' in test
    t = test_default + test['tap']
    test_command = [
      perl.path(),
      '-I', meson.source_root() / 'src/test/perl',
      '-I', t['sd'],
    ]

    env = test_env

    # Add temporary install, the build directory for non-installed binaries and
    # also test/ for non-installed test binaries built separately.
    env.prepend('PATH', temp_install_bindir, t['bd'], t['bd'] / 'test')

    foreach name, value : t.get('env', {})
      env.set(name, value)
    endforeach

    test_kwargs = {
      'protocol': 'tap',
      'suite': [t['name']],
      'timeout': 1000,
      'depends': test_deps + t.get('deps', []),
      'env': env,
    }

    if t.has_key('test_kwargs')
      test_kwargs += t['test_kwargs']
    endif

    foreach onetap : t['tests']
      # Make tap test names prettier, remove t/ and .pl
      onetap_p = onetap
      if onetap_p.startswith('t/')
        onetap_p = onetap.split('t/')[1]
      endif
      if onetap_p.endswith('.pl')
        onetap_p = fs.stem(onetap_p)
      endif

      test(t['name'] / onetap_p,
        python,
        kwargs: test_kwargs,
        args: [
          testwrap.path(),
          '--srcdir', t['sd'],
          '--basedir', meson.build_root(),
          '--testgroup', t['name'],
          '--testname', onetap_p,
          test_command,
          t['sd'] / onetap,
        ]
      )
    endforeach
  endif
endforeach


###############################################################
# Pseudo targets
###############################################################

alias_target('backend', backend_targets)



###############################################################
# The End, The End, My Friend
###############################################################

if meson.version().version_compare('>=0.57')

  summary({
    'data block size' : cdata.get('BLCKSZ'),
    'WAL block size' : cdata.get('XLOG_BLCKSZ'),
    'segment size' : cdata.get('RELSEG_SIZE')
    }, section: 'Data layout'
  )

  summary(
    {
      'host system' : '@0@ @1@'.format(host_system, host_cpu),
      'build system' : '@0@ @1@'.format(build_system, build_cpu),
    },
    section: 'System'
  )

  summary(
    {
      'linker': '@0@'.format(cc.get_linker_id()),
      'C compiler': '@0@ @1@'.format(cc.get_id(), cc.version()),
    },
    section: 'Compiler'
  )

  summary(
    {
      'C FLAGS, functional': ' '.join(functional_cflags),
      'C FLAGS, warnings': ' '.join(warning_cflags),
    },
    section: 'Compiler Flags'
  )

  if llvm.found()
    summary(
      {
        'C++ compiler': '@0@ @1@'.format(cpp.get_id(), cpp.version())
      },
      section: 'Compiler')

      summary(
      {
        'C++ FLAGS, functional': ' '.join(functional_cxxflags),
        'C++ FLAGS, warnings': ' '.join(warning_cxxflags),
      },
      section: 'Compiler Flags')
  endif

  summary(
    {
      'bison' : '@0@ @1@'.format(bison.full_path(), bison_version),
      'dtrace' : dtrace,
    },
    section: 'Programs'
  )

  summary(
    {
      'bonjour': bonjour,
      'bsd_auth': bsd_auth,
      'gss': gssapi,
      'icu': icu,
      'ldap': ldap,
      'libxml': libxml,
      'libxslt': libxslt,
      'llvm': llvm,
      'lz4': lz4,
      'nls' : libintl,
      'pam' : pam,
      'plperl': perl_dep,
      'plpython': python3_dep,
      'pltcl': tcl_dep,
      'readline': readline,
      'selinux': selinux,
      'ssl': ssl,
      'systemd': systemd,
      'uuid': uuid,
      'zlib': zlib,
      'zstd': zstd,
    },
    section: 'External libraries'
  )

endif
