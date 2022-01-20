# These files are part of the Unicode Character Database. Download
# them on demand.

UNICODE_VERSION = '14.0.0'

unicode_data = {}
unicode_baseurl = 'https://www.unicode.org/Public/@0@/ucd/@1@'

if not wget.found()
  subdir_done()
endif

foreach f : ['UnicodeData.txt', 'EastAsianWidth.txt', 'DerivedNormalizationProps.txt', 'CompositionExclusions.txt', 'NormalizationTest.txt']
  url = unicode_baseurl.format(UNICODE_VERSION, f)
  target = custom_target(f,
    output: f,
    command: [wget, wget_flags, url],
    build_by_default: false,
  )
  unicode_data += {f: target}
endforeach


update_unicode_targets = []

update_unicode_targets += \
  custom_target('unicode_norm_table.h',
    input: [unicode_data['UnicodeData.txt'], unicode_data['CompositionExclusions.txt']],
    output: ['unicode_norm_table.h', 'unicode_norm_hashfunc.h'],
    command: [perl, files('generate-unicode_norm_table.pl'), '@OUTDIR@', '@INPUT@'],
    build_by_default: false,
  )

update_unicode_targets += \
  custom_target('unicode_combining_table.h',
    input: [unicode_data['UnicodeData.txt']],
    output: ['unicode_combining_table.h'],
    command: [perl, files('generate-unicode_combining_table.pl'), '@INPUT@'],
    build_by_default: false,
    capture: true,
  )

update_unicode_targets += \
  custom_target('unicode_east_asian_fw_table.h',
    input: [unicode_data['EastAsianWidth.txt']],
    output: ['unicode_east_asian_fw_table.h'],
    command: [perl, files('generate-unicode_east_asian_fw_table.pl'), '@INPUT@'],
    build_by_default: false,
    capture: true,
  )

update_unicode_targets += \
  custom_target('unicode_normprops_table.h',
    input: [unicode_data['DerivedNormalizationProps.txt']],
    output: ['unicode_normprops_table.h'],
    command: [perl, files('generate-unicode_normprops_table.pl'), '@INPUT@'],
    build_by_default: false,
    capture: true,
  )

norm_test_table = custom_target('norm_test_table.h',
    input: [unicode_data['NormalizationTest.txt']],
    output: ['norm_test_table.h'],
    command: [perl, files('generate-norm_test_table.pl'), '@INPUT@', '@OUTPUT@'],
    build_by_default: false,
  )

inc = include_directories('.')

norm_test = executable('norm_test',
  ['norm_test.c', norm_test_table],
  dependencies: [frontend_port_code],
  include_directories: inc,
  link_with: [pgport_static, common_static],
  build_by_default: false,
  kwargs: default_bin_args + {
    'install': false,
  }
)

if not meson.is_cross_build()
  norm_test_valid = custom_target('norm_test.valid',
    output: 'norm_test.valid',
    depends: update_unicode_targets,
    command: [norm_test],
    build_by_default: false,
    capture: true,
  )

  run_target('update-unicode',
    depends: norm_test_valid,
    command: ['cp', update_unicode_targets, '@SOURCE_ROOT@/src/include/common/']
  )
else
  run_target('update-unicode',
    depends: update_unicode_targets,
    command: ['cp', update_unicode_targets, '@SOURCE_ROOT@/src/include/common/']
  )
endif
