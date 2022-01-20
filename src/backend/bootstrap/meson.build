backend_sources += files(
 'bootstrap.c')

bootscanner = custom_target('bootscanner',
  input: 'bootscanner.l',
  output: 'bootscanner.c',
  command: flex_cmd,
)
generated_backend_sources += bootscanner

bootparse = custom_target('bootparse',
  input: 'bootparse.y',
  kwargs: bison_kw,
)
generated_backend_sources += bootparse.to_list()
