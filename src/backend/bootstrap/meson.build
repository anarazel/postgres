backend_sources += files(
 'bootstrap.c')

bootscanner = custom_target('bootscanner',
  input: ['bootscanner.l'],
  output: ['bootscanner.c'],
  command: [flex, '-o', '@OUTPUT@', '@INPUT@'])

generated_backend_sources += custom_target('bootparse',
  input: ['bootparse.y', bootscanner[0]],
  output: ['bootparse.c'],
  command: [bison, bisonflags, '-o', '@OUTPUT@', '@INPUT0@'])
