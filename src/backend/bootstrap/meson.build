backend_sources += files(
 'bootstrap.c')

bootscanner = custom_target('bootscanner',
  input: ['bootscanner.l'],
  output: ['bootscanner.c'],
  command: [flex_cmd, '@INPUT@'],
)
generated_sources += bootscanner

generated_backend_sources += custom_target('bootparse',
  input: ['bootparse.y', bootscanner[0]],
  output: ['bootparse.c'],
  command: [bison_cmd, '@INPUT0@'],
)
