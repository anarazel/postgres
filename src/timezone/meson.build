# files to build into backend
timezone_sources = files(
  'localtime.c',
  'pgtz.c',
  'strftime.c',
)


timezone_inc = include_directories('.')

timezone_localtime_source = files('localtime.c')

# files needed to build zic utility program
zic_sources = files(
   'zic.c'
)

# we now distribute the timezone data as a single file
tzdata = files(
  'data/tzdata.zi'
)

zic = executable('zic', zic_sources,
  link_with: pgport_static,
  dependencies: [frontend_code]
)

# FIXME: this used to be sorted - but also isn't actually used
abbrevs_txt = custom_target('abbrevs.txt',
  input: tzdata,
  output: ['abbrevs.txt'],
  command: [zic, '-P', '-b', 'fat', 'junkdir', '@INPUT@'],
  capture: true)

tzdata = custom_target('tzdata',
  input: tzdata,
  output: ['timezone'],
  command: [zic, '-d', '@OUTPUT@', '@INPUT@'],
  install: true,
  install_dir: get_option('datadir'))

subdir('tznames')
