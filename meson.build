project('postgresql', ['c'],
        version: '14devel',
        license: 'PostgreSQL',
	meson_version: '>=0.52',
	default_options: [
	  'warning_level=2',
	  'b_pie=true',
	  'b_pch=false',
	  'buildtype=release',
	])

pg_version = meson.project_version()
pg_version_major = '14'
#pg_version.split('.')[0]

cc = meson.get_compiler('c')

cdata = configuration_data()

perl = find_program('perl', required: true)


###############################################################
# Path to meson (for tests etc)
###############################################################

# FIXME: write something better, or wait for meson to have it
# https://github.com/mesonbuild/meson/issues/8511
meson_binary_r = run_command([perl, '-e', 'print $ENV{"MESONINTROSPECT"};'])

if meson_binary_r.returncode() != 0 or meson_binary_r.stdout() == ''
  error('huh, could not run perl')
endif

# XXX: if there are spaces in the invocation, this'd be broken
meson_introspect = meson_binary_r.stdout().split(' ')
meson_introspect_len = meson_introspect.length()
meson_path = []

if meson_introspect.length() == 0 or meson_introspect[meson_introspect.length() - 1] != 'introspect'
  error('unexpected introspect line', meson_binary_r.stdout())
endif

i = 0
meson_binpath = ''
meson_args = []
foreach e : meson_introspect
  if i == 0
    meson_binpath = e
  elif e == 'introspect'
    break
  else
    meson_args += e
  endif
  i += 1
endforeach

meson_bin = find_program(meson_binpath, native: true)


###############################################################
# Program paths
###############################################################

flex = find_program('flex', native: true)
bison = find_program('bison', native: true)
sed = find_program('sed', native: true)
prove = find_program('prove')
rm_bin = find_program('rm', native: true)

testwrap = find_program(meson.source_root() / 'src/tools/testwrap', native: true)


###############################################################
# LLVM
###############################################################

if get_option('llvm')
  add_languages('cpp', required : true)
  cpp = meson.get_compiler('cpp')

  #if get_option('llvm-config') != ''
  #  llvm_config = find_program(get_option('llvm-config'))
  #  meson.override_find_program('llvm-config', llvm_config)
  #  warning('set llvm-config program to ' + llvm_config.path())
  #endif

  llvm = dependency('llvm', required: true, version : '>=3.9', method : 'config-tool')

  llvm_binpath = llvm.get_variable(configtool: 'bindir')

  ccache = find_program('ccache', required: true)
  clang = find_program(llvm_binpath / 'clang', required: true)
  llvm_lto = find_program(llvm_binpath / 'llvm-lto', required: true)

  cdata.set('USE_LLVM', 1)

  # FIXME: the includes hardcoded here suck
  llvm_irgen_command = [ccache, clang,
	'-MD', '-MQ', '@OUTPUT@', '-MF', '@DEPFILE@',
	'-o', '@OUTPUT@',
	'-c',
        '-I', '@SOURCE_ROOT@/src/include',
        '-I', '@BUILD_ROOT@/src/include',
        '-I', '@BUILD_ROOT@/src/backend/utils/misc',
        '-I', meson.current_source_dir(),
	'-O2',
        '-DDLSUFFIX=".so"',
        '-Wno-ignored-attributes',
        '-flto=thin', '-emit-llvm', '@INPUT@']

  llvm_irgen_kw = {
    'command': llvm_irgen_command,
    'depfile': '@BASENAME@.c.bc.d',
  }

  #llvm_irlink_kw = {
  #  'command':[
  #    llvm_lto,
  #    '-thinlto',
  #    '-thinlto-action=thinlink',
  #    '--thinlto-prefix-replace=src/backend;dfdpostgres',
  #    '--thinlto-save-objects=dfdfdpostgres',
  #    '-o', '@OUTPUT@',
  #    '@INPUT@',
  #  ],
  #  'install': true,
  #  'install_dir': get_option('libdir') / 'bitcode',
  #}
  ##error(llvm.name())

  irlink = find_program(meson.source_root() / 'src/tools/irlink', native: true)

  llvm_irlink_kw = {
    'command':[
      irlink,
      '@SOURCE_ROOT@',
      '@BUILD_ROOT@',
      llvm_lto,
      '-o', '@OUTPUT0@',
      '@PRIVATE_DIR@',
      '@INPUT@',
    ],
    'install': true,
    'install_dir': get_option('libdir'),
  }

else
  llvm = dependency('', required: false)
endif

header_checks = [
  ['atomic.h', 'HAVE_ATOMIC_H'],
  ['stdbool.h', 'HAVE_STDBOOL_H'],
  ['copyfile.h', 'HAVE_COPYFILE_H'],
  ['execinfo.h', 'HAVE_EXECINFO_H'],
  ['getopt.h', 'HAVE_GETOPT_H'],
  ['ifaddrs.h', 'HAVE_IFADDRS_H'],
  ['langinfo.h', 'HAVE_LANGINFO_H'],
  ['mbarrier.h', 'HAVE_MBARRIER_H'],
  ['poll.h', 'HAVE_POLL_H'],
  ['sys/epoll.h', 'HAVE_SYS_EPOLL_H'],
  ['sys/event.h', 'HAVE_SYS_EVENT_H'],
  ['sys/ipc.h', 'HAVE_SYS_IPC_H'],
  ['sys/prctl.h', 'HAVE_SYS_PRCTL_H'],
  ['sys/procctl.h', 'HAVE_SYS_PROCCTL_H'],
  ['sys/pstat.h', 'HAVE_SYS_PSTAT_H'],
  ['sys/resource.h', 'HAVE_SYS_RESOURCE_H'],
  ['sys/select.h', 'HAVE_SYS_SELECT_H'],
  ['sys/sem.h', 'HAVE_SYS_SEM_H'],
  ['sys/shm.h', 'HAVE_SYS_SHM_H'],
  ['sys/sockio.h', 'HAVE_SYS_SOCKIO_H'],
  ['sys/tas.h', 'HAVE_SYS_TAS_H'],
  ['sys/uio.h', 'HAVE_SYS_UIO_H'],
  ['sys/un.h', 'HAVE_SYS_UN_H'],
  ['termios.h', 'HAVE_TERMIOS_H'],
  ['ucred.h', 'HAVE_UCRED_H'],
  # FIXME: openbsd workaround
  ['sys/ucred.h', 'HAVE_SYS_UCRED_H'],
  ['wctype.h', 'HAVE_WCTYPE_H'],
  ['netinet/tcp.h', 'HAVE_NETINET_TCP_H'],
  ['net/if.h', 'HAVE_NET_IF_H'],
]

foreach hc : header_checks
  varname = 'HAVE_'+hc.get(0).underscorify().to_upper()
  cdata.set(varname, cc.has_header(hc.get(0)))
endforeach

# XXX: this is borked, HAVE_SYS_UCRED_H not defined
if cc.has_type('struct cmsgcred', prefix: '''
#include <sys/socket.h>
#include <sys/param.h>
#ifdef HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif''',
    args: cdata.get('HAVE_SYS_UCRED_H') ? '-DHAVE_SYS_UCRED_H' : '')
  cdata.set('HAVE_STRUCT_CMSGCRED')
endif

if cc.has_function('getopt') and \
   cc.has_function('getopt_long') and \
   cc.has_type('struct option', prefix: '#include <getopt.h>')
  cdata.set('HAVE_GETOPT', 1)
  cdata.set('HAVE_GETOPT_LONG', 1)
  cdata.set('HAVE_STRUCT_OPTION', 1)
else
  warning('not yet implemented')
endif


if cc.has_type('struct sockaddr_storage', prefix: '''
#include <sys/types.h>
#include <sys/socket.h>''')
  cdata.set('HAVE_STRUCT_SOCKADDR_STORAGE', true)
endif

if cc.has_member('struct sockaddr_storage',
                 'ss_family',
  prefix: '''#include <sys/types.h>
#include <sys/socket.h>''')
  cdata.set('HAVE_STRUCT_SOCKADDR_STORAGE_SS_FAMILY', true)
endif

if cc.has_member('struct sockaddr_storage',
                 '__ss_family',
  prefix: '''
#include <sys/types.h>
#include <sys/socket.h>''')
  cdata.set('HAVE_STRUCT_SOCKADDR_STORAGE___SS_FAMILY', true)
endif

if cc.has_type('struct sockaddr_un', prefix: '''
#include <sys/types.h>
#include <sys/un.h>''')
  cdata.set('HAVE_STRUCT_SOCKADDR_UN', true)
endif

if cc.has_type('struct addrinfo', prefix: '''
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
''')
  cdata.set('HAVE_STRUCT_ADDRINFO', true)
endif


if cc.has_type('struct sockaddr_in6', prefix: '''
#include <netinet/in.h>''')
  cdata.set('HAVE_IPV6', true)
endif


if cc.has_member('struct tm', 'tm_zone', prefix: '''
#include <sys/types.h>
#include <time.h>
''')
  cdata.set('HAVE_STRUCT_TM_TM_ZONE', true)
endif

if cc.compiles('''
#include <time.h>
extern int foo(void);
int foo(void)
{
    return timezone / 60;
}
''', name: 'Check if the global variable `timezone\' exists')
  cdata.set('HAVE_VAR_INT_TIMEZONE', true)
endif


m_dep = cc.find_library('m', required : false)
rt_dep = cc.find_library('rt', required : false)
dl_dep = cc.find_library('dl', required : false)
thread_dep = dependency('threads')

cdata.set('HAVE_PWRITE', cc.has_function('pwrite'))
cdata.set('HAVE_PWRITEV', cc.has_function('pwritev'))
cdata.set('HAVE_PREAD', cc.has_function('pread'))
cdata.set('HAVE_PREADV', cc.has_function('preadv'))
cdata.set('HAVE_READLINK', cc.has_function('readlink'))
cdata.set('HAVE_LINK', cc.has_function('link'))
cdata.set('HAVE_SYMLINK', cc.has_function('symlink'))
cdata.set('HAVE_FDATASYNC', cc.has_function('fdatasync'))
cdata.set('HAVE_GETADDRINFO', cc.has_function('getaddrinfo'))
cdata.set('HAVE_GETPEEREID', cc.has_function('getpeereid'))
cdata.set('HAVE_GETPEERUCRED', cc.has_function('getpeerucred'))

cdata.set('HAVE_STRLCAT', cc.has_function('strlcat'))
cdata.set('HAVE_STRLCPY', cc.has_function('strlcpy'))
cdata.set('HAVE_STRNLEN', cc.has_function('strnlen'))
cdata.set('HAVE_STRCHRNUL', cc.has_function('strchrnul'))

cdata.set('HAVE_SHM_OPEN', cc.has_function('shm_open', dependencies: rt_dep))
cdata.set('HAVE_SHM_UNLINK', cc.has_function('shm_unlink', dependencies: rt_dep))

cdata.set('HAVE_GETRLIMIT', cc.has_function('getrlimit'))
cdata.set('HAVE_POLL', cc.has_function('poll'))
cdata.set('HAVE_CLOCK_GETTIME', cc.has_function('clock_gettime'))
cdata.set('HAVE_POSIX_FALLOCATE', cc.has_function('posix_fallocate'))


cdata.set('HAVE_LONG_INT_64', true)
cdata.set('PG_INT64_TYPE', 'long int')
cdata.set_quoted('INT64_MODIFIER', 'l')

if cc.has_header_symbol('unistd.h', 'fdatasync')
  cdata.set('HAVE_DECL_FDATASYNC', 1)
endif
if cc.has_header_symbol('fcntl.h', 'F_FULLSYNC')
  cdata.set('HAVE_DECL_F_FULLSYNC', 1)
endif

cdata.set('pg_restrict', '__restrict')


cdata.set('ACCEPT_TYPE_ARG1', 'int')
cdata.set('ACCEPT_TYPE_ARG2', 'struct sockaddr')
cdata.set('ACCEPT_TYPE_ARG3', 'socklen_t')
cdata.set('ACCEPT_TYPE_RETURN', 'int')

cdata.set('HAVE_STRUCT_ADDRINFO', 1)


cdata.set('MAXIMUM_ALIGNOF', 8)
cdata.set('ALIGNOF_SHORT', cc.alignment('short'))
cdata.set('ALIGNOF_INT', cc.alignment('int'))
cdata.set('ALIGNOF_LONG', cc.alignment('long'))
cdata.set('ALIGNOF_DOUBLE', cc.alignment('double'))
cdata.set('SIZEOF_VOID_P', cc.sizeof('void *'))

cdata.set('HAVE_GETRUSAGE', cc.has_function('getrusage'))

cdata.set('HAVE__BUILTIN_UNREACHABLE', 1)
cdata.set('HAVE_I_CONSTRAINT__BUILTIN_CONSTANT_P', 1)
cdata.set('HAVE__BUILTIN_CONSTANT_P', 1)
cdata.set('HAVE_COMPUTED_GOTO', 1)
cdata.set('HAVE__STATIC_ASSERT', 1)
cdata.set('HAVE__BOOL', 1)
cdata.set('PG_USE_STDBOOL', 1)

cdata.set('USE_ASSERT_CHECKING', get_option('cassert'))

cdata.set('USE_UNNAMED_POSIX_SEMAPHORES', 1)

cdata.set('BLCKSZ', 8192, description: '''
 Size of a disk block --- this also limits the size of a tuple.  You
 can set it bigger if you need bigger tuples (although TOAST should
 reduce the need to have large tuples, since fields can be spread
 across multiple tuples).

 BLCKSZ must be a power of 2.  The maximum possible value of BLCKSZ
 is currently 2^15 (32768).  This is determined by the 15-bit widths
 of the lp_off and lp_len fields in ItemIdData (see
 include/storage/itemid.h).

 Changing BLCKSZ requires an initdb.
''')


cdata.set('XLOG_BLCKSZ', 8192)
cdata.set('RELSEG_SIZE', 131072)
cdata.set('DEF_PGPORT', 5432)
cdata.set_quoted('DEF_PGPORT_STR', '5432')
cdata.set_quoted('PG_KRB_SRVNAM', 'postgres')

printf_attributes = ['gnu_printf', '__syslog__', 'printf']
testsrc = 'extern void pgac_write(int ignore, const char *fmt,...) __attribute__((format(@0@, 2,3)));'
foreach a : printf_attributes
  if cc.compiles(testsrc.format(a), args: '-Werror', name: 'format ' + a)
    cdata.set('PG_PRINTF_ATTRIBUTE', a)
    break
  endif
endforeach

cdata.set('MEMSET_LOOP_LIMIT', 1024)

cdata.set_quoted('PACKAGE_NAME', 'PostgreSQL')
cdata.set_quoted('PACKAGE_BUGREPORT', 'pgsql-bugs@lists.postgresql.org')
cdata.set_quoted('PACKAGE_URL', 'https://www.postgresql.org/')

cdata.set_quoted('PG_VERSION', pg_version)
cdata.set_quoted('PG_VERSION_STR', 'PostgreSQL 14devel on x86_64-pc-linux-gnu, compiled by gcc-10 (Debian 10.2.1-6) 10.2.1 20210110, 64-bit')
cdata.set_quoted('PG_MAJORVERSION', pg_version_major)
cdata.set('PG_MAJORVERSION_NUM', pg_version_major)
cdata.set('PG_VERSION_NUM', 140000)
cdata.set_quoted('CONFIGURE_ARGS', '')

add_project_arguments('-msse4.2', language: 'c')
cdata.set('USE_SSE42_CRC32C', 1)

add_project_arguments('-D_GNU_SOURCE', language: 'c')


###############################################################
# Atomics
###############################################################

if get_option('atomics')
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

# FIXME: int64 reference
    {'name': 'HAVE_GCC__SYNC_INT64_CAS',
     'desc': '__sync_val_compare_and_swap(int64)',
     'test': '''
long val = 0;
__sync_val_compare_and_swap(&val, 0, 37);'''},

    {'name': 'HAVE_GCC__ATOMIC_INT32_CAS',
     'desc': ' __atomic_compare_exchange_n(int32)',
     'test': '''
int val = 0;
int expect = 0;
__atomic_compare_exchange_n(&val, &expect, 37, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);'''},

# FIXME: int64 reference
    {'name': 'HAVE_GCC__ATOMIC_INT64_CAS',
     'desc': ' __atomic_compare_exchange_n(int64)',
     'test': '''
long val = 0;
int expect = 0;
__atomic_compare_exchange_n(&val, &expect, 37, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);'''},
  ]

  foreach check : atomic_checks
    test = '''
int main(int argc, char **argv)
{
@0@
}'''.format(check['test'])

    cdata.set(check['name'],
      cc.links(test, name: check['desc']))
  endforeach

endif


###############################################################
# zlib
###############################################################

if get_option('zlib')
  zlib = dependency('zlib', required: true)
  if not cc.has_header('zlib.h', dependencies: [zlib])
    error('zlib header not found')
  endif

  if not cc.has_type('z_streamp', dependencies: [zlib],prefix: '#include <zlib.h>')
    error('zlib version is too old')
  endif

  cdata.set('HAVE_LIBZ', true)
  cdata.set('WITH_ZLIB', true)
else
  zlib = dependency('', required : false)
endif


###############################################################
# Readline Handling
#
# FIXME: editline support
###############################################################

if not get_option('without_readline')
  #readline = cc.find_library('readline')
  readline = dependency('readline')
  if readline.found()
    cdata.set('HAVE_LIBREADLINE', 1)

    cdata.set('HAVE_READLINE_HISTORY_H',
      cc.has_header('readline/history.h', dependencies: readline))
    cdata.set('HAVE_READLINE_READLINE_H',
      cc.has_header('readline/readline.h', dependencies: readline))

    check_funcs = [
      'rl_completion_matches',
      'rl_filename_completion_function',
      'rl_reset_screen_size',
      'append_history',
      'history_truncate_file',
    ]

    foreach func : check_funcs
      cdata.set('HAVE_'+func.to_upper(),
        cc.has_function(func, dependencies: readline))
    endforeach

    check_vars = [
      'rl_completion_append_character',
      'rl_completion_suppress_quote',
      'rl_filename_quote_characters',
      'rl_filename_quoting_function',
    ]

    foreach var : check_vars
      cdata.set('HAVE_'+var.to_upper(),
        cc.has_header_symbol('readline/readline.h', var, dependencies: readline))
    endforeach
  endif
else
  readline = dependency('', required : false)
endif


###############################################################
# SSL Library
###############################################################

if get_option('ssl') != 'none'
  ssl = dependency(get_option('ssl'), required: true)

  cdata.set_quoted('WITH_SSL', get_option('ssl'))

  check_funcs = [
    'X509_get_signature_nid',
    'OPENSSL_init_ssl',
    'BIO_get_data',
    'BIO_meth_new',
    'ASN1_STRING_get0_data',
  ]

  foreach func : check_funcs
    cdata.set('HAVE_'+func.to_upper(),
      cc.has_function(func, dependencies: ssl))
  endforeach

  cdata.set('USE_OPENSSL', 1,
    description: 'Define to 1 to build with OpenSSL support. (--with-ssl=openssl)')

else
  ssl = dependency('', required : false)
endif


common_functional_flags = [
  # Disable strict-aliasing rules; needed for gcc 3.3+
  '-fno-strict-aliasing',
  # Disable optimizations that assume no overflow; needed for gcc 4.3+
  '-fwrapv',
  '-fexcess-precision=standard'
]

add_project_arguments(cc.get_supported_arguments(common_functional_flags), language: 'c')

vectorize_cflags = cc.get_supported_arguments(['-ftree-vectorize'])
unroll_loops_cflags = cc.get_supported_arguments(['-funroll-loops'])


common_warning_flags = [
  '-Wmissing-prototypes',
  '-Wpointer-arith',
  '-Werror=vla',
  '-Wendif-labels',
  '-Wmissing-format-attribute',
  '-Wimplicit-fallthrough=3',
  '-Wcast-function-type',
  '-Wformat-security',
]

add_project_arguments(cc.get_supported_arguments(common_warning_flags), language: 'c')

if llvm.found()
  add_project_arguments(cpp.get_supported_arguments(common_warning_flags), language: 'cpp')
endif

# A few places with imported code get a pass on -Wdeclaration-after-statement, remember
# the result for them
if cc.has_argument('-Wdeclaration-after-statement')
  add_project_arguments('-Wdeclaration-after-statement', language: 'c')
  using_declaration_after_statement_warning = true
else
  using_declaration_after_statement_warning = false
endif


# We want to suppress a few unhelpful warnings - but gcc won't
# complain about unrecognized -Wno-foo switches, so we have to test
# for the positive form and if that works, add the negative form

negative_warning_flags = [
  'unused-command-line-argument',
  'format-truncation',
  'stringop-truncation',

  # FIXME: andres
  'cast-function-type',
  'clobbered',
  'format-truncation',
  'missing-field-initializers',
  'sign-compare',
  'unused-parameter',
]

foreach w : negative_warning_flags
  if cc.has_argument('-W'+w)
    add_project_arguments('-Wno-'+w, language: 'c')
  endif

  if llvm.found() and cpp.has_argument('-W'+w)
    add_project_arguments('-Wno-'+w, language: 'cpp')
  endif
endforeach


# List of tap tests we later generate test() invocations for. The main
# reason for doing it that way instead of having test() invocations
# everywhere is that they end up being too large. A second benefit is
# that it'd make it easier to generate data for another runner.
tap_tests = []
isolation_tests = []
regress_tests = []

test_environment = environment()
test_install_target = meson.build_root() / 'tmp_install/'
test_install = test_install_target + '/' + get_option('prefix')
test_environment.prepend('LD_LIBRARY_PATH', test_install / get_option('libdir'))
test_environment.prepend('PATH',  test_install / get_option('bindir'))
test_environment.prepend('PG_REGRESS', meson.build_root() / 'src/test/regress/pg_regress')

add_test_setup('default',
  env: test_environment,
  is_default: true)



###############################################################
# Build
###############################################################


# First visit src/include - all targets creating headers are defined
# within. That makes it easy to add the necessary dependencies for the
# subsequent build steps.

generated_headers = []

postgres_inc = include_directories('src/include')

subdir('src/include')

backend_deps = [m_dep, rt_dep, dl_dep, thread_dep]

frontend_code = declare_dependency(
  compile_args: ['-DFRONTEND'],
  include_directories: [postgres_inc],
  sources: generated_headers,
)

backend_code = declare_dependency(
  compile_args: [],
  include_directories: [postgres_inc],
  sources: generated_headers,
)

both_code = declare_dependency(
  compile_args: [],
  include_directories: [postgres_inc],
  sources: generated_headers,
)



###############################################################
# Test Infrastructure
###############################################################

test_deps = []


subdir('src')

subdir('contrib')

subdir('src/test')


###############################################################
# Test Generation
###############################################################

test('temp_install',
    meson_bin, args: meson_args + ['install', '--quiet', '--only-changed'],
    env: {'DESTDIR':test_install_target},
    priority: 100,
    is_parallel: false,
    suite: ['setup'])

test_result_dir = meson.build_root() / 'testrun'


# Run all 'pg_regress' style tests
foreach rt : regress_tests

  test_command = [
    pg_regress,
    '--temp-instance', test_result_dir / rt['name'] / 'pg_regress' / 'tmp_check',
    '--inputdir', rt['sd'],
    '--outputdir', test_result_dir / rt['name'] / 'pg_regress',
    '--bindir', '',
    '--dlpath', rt['bd'],
    '--max-concurrent-tests=20',
  ]

  if rt.has_key('regress_args')
     test_command += rt['regress_args']
  endif

  if rt.has_key('schedule')
     test_command += ['--schedule', rt['schedule'],]
  else
     test_command += rt['sql']
  endif

  test_kwargs = {
    'suite': ['pg_regress', rt['name']],
    'priority': 10,
    'timeout': 300,
    'depends': test_deps,
    'workdir': rt['sd'],
    'args': [
      meson.build_root(),
      rt['name'],
      'pg_regress',
      test_command,
    ]
  }

  # Allow test definition to override arguments
  if rt.has_key('test_kwargs')
     test_kwargs += rt['test_kwargs']
  endif

  test(rt['name'] / 'pg_regress',
    testwrap,
    kwargs: test_kwargs,
  )
endforeach


# Run all 'isolationtester' style tests
foreach it : isolation_tests

  test_command = [
    pg_isolation_regress,
    '--temp-instance', test_result_dir / it['name'] / 'isolation' / 'tmp_check',

    '--inputdir', it['sd'],
    '--outputdir', test_result_dir / it['name'] / 'isolation',
    '--bindir', '',
    '--dlpath', it['bd'],
    '--max-concurrent-tests=20',
  ]

  if it.has_key('regress_args')
     test_command += it['regress_args']
  endif

  if it.has_key('schedule')
     test_command += ['--schedule', it['schedule'],]
  else
     test_command += it['specs']
  endif

  test_kwargs = {
    'suite': ['isolation', it['name']],
    'priority': 20,
    'timeout': 300,
    'depends': test_deps,
    'workdir': it['sd'],
    'args': [
      meson.build_root(),
      it['name'],
      'isolation',
      test_command,
    ]
  }

  # Allow test definition to override arguments
  if it.has_key('test_kwargs')
     test_kwargs += it['test_kwargs']
  endif

  test(it['name'] / 'isolation',
    testwrap,
    kwargs: test_kwargs,
  )
endforeach


# See comment above tap_test = [].
# FIXME: dependencies for each test
foreach tt : tap_tests
  env = environment()
  #env.set('TESTDIR', meson.build_root())
  env.set('REGRESS_SHLIB', regress_module.full_path())

  foreach t : tt['tests']
    test(tt['name'] / t,
      testwrap,
      args: [
        meson.build_root(),
	tt['name'],
	t,
        'perl',
        '-I', meson.source_root() / 'src/test/perl',
	'-I', tt['sd'],
	tt['sd'] / t
      ],
      protocol: 'tap',
      suite: ['tap', tt['name']],
      env: env,
      depends: test_deps,
      timeout: 300,
      )
  endforeach
endforeach
