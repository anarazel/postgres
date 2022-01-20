errcodes = custom_target('errcodes',
  input : files('../../backend/utils/errcodes.txt'),
  output : ['errcodes.h'],
  command : [perl, files('../../backend/utils/generate-errcodes.pl'), '@INPUT@', '@OUTPUT@']
)
generated_headers += errcodes

generated_backend_headers += custom_target('probes.d',
  input: files('../../backend/utils/probes.d'),
  output : 'probes.h',
  capture: true,
  command : [sed, '-f', files('../../backend/utils/Gen_dummy_probes.sed'), '@INPUT@']
)

fmgrtab_target = custom_target('fmgrtab',
  input: '../catalog/pg_proc.dat',
  output : ['fmgroids.h', 'fmgrprotos.h', 'fmgrtab.c'],
  command: [perl, '-I', '@SOURCE_ROOT@/src/backend/catalog/', files('../../backend/utils/Gen_fmgrtab.pl'), '--include-path=@SOURCE_ROOT@/src/include', '--output=@OUTDIR@', '@INPUT@']
)

generated_backend_headers += fmgrtab_target[0]
generated_backend_headers += fmgrtab_target[1]
