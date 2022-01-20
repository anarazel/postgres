ecpg_sources = files(
  'c_keywords.c',
  'descriptor.c',
  'ecpg.c',
  'ecpg_keywords.c',
  'keywords.c',
  'output.c',
  'parser.c',
  'type.c',
  '../ecpglib/typename.c',
  'variable.c',
)

ecpg_sources += custom_target('pgc.c',
  input: ['pgc.l'],
  output: ['pgc.c'],
  command: [flex_cmd, '@INPUT@'],
)

ecpg_files = [
    'ecpg.addons',
    'ecpg.header',
    'ecpg.tokens',
    'ecpg.trailer',
    'ecpg.type',
]

preproc_y = custom_target('preproc.y',
  input: [
    '../../../backend/parser/gram.y',
    ecpg_files,
  ],
  output: ['preproc.y'],
  command: [
    perl, files('parse.pl'),
    '--srcdir', '@CURRENT_SOURCE_DIR@',
    '--parser', '@INPUT0@',
    '--output', '@OUTPUT0@',
  ],
)

check_rules = custom_target('preproc.y.check_rules',
  input: [
    '../../../backend/parser/gram.y',
    ecpg_files,
  ],
  output: 'preproc.y.check_rules',
  command: [
    perl, files('check_rules.pl'),
    '--srcdir', '@CURRENT_SOURCE_DIR@',
    '--parser', '@INPUT0@',
    '--stamp', '@OUTPUT0@',
  ],
)

ecpg_sources += custom_target('preproc.c',
  input: [preproc_y],
  output: ['preproc.c', 'preproc.h'],
  depends: check_rules,
  command: [bison_cmd, '-d', '@INPUT@'],
)

ecpg_sources += custom_target('c_kwlist_d.h',
  input: ['c_kwlist.h'],
  output: ['c_kwlist_d.h'],
  command: [
    perl,
    '-I', '@SOURCE_ROOT@/src/tools',
    '@SOURCE_ROOT@/src/tools/gen_keywordlist.pl',
    '--output', '@OUTDIR@',
    '--varname', 'ScanCKeywords',
    '--no-case-fold', '@INPUT0@'
  ],
)

ecpg_sources += custom_target('ecpg_kwlist_d.h',
  input: ['ecpg_kwlist.h'],
  output: ['ecpg_kwlist_d.h'],
  command: [
    perl, '-I',
    '@SOURCE_ROOT@/src/tools',
    '@SOURCE_ROOT@/src/tools/gen_keywordlist.pl',
    '--output', '@OUTDIR@',
    '--varname', 'ScanECPGKeywords', '@INPUT0@',
  ]
)

ecpg_exe = executable('ecpg',
  ecpg_sources,
  include_directories : ['.', ecpg_inc, postgres_inc],
  dependencies: [frontend_shlib_code, libpq],
  kwargs: default_bin_args,
)

subdir('po', if_found: libintl)
