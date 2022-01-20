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


# FIXME: For cross builds, it would need a native built libpgport/pgcommon to
# build our zic. But for that we'd need to run a good chunk of the configure
# tests both natively and cross. Unclear if it's worth it.
if meson.is_cross_build()
  zic = find_program('zic', native: true, required: false)
else
  zic = executable('zic', zic_sources,
    dependencies: [frontend_code],
    kwargs: default_bin_args + {'install': false}
  )
endif

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
