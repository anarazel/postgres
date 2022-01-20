pgc_files = [
  'charfuncs',
  'dec_test',
  'describe',
  'rfmtdate',
  'rfmtlong',
  'rnull',
  'sqlda',
  'test_informix',
  'test_informix2',
]

pgc_extra_flags = {
  'rnull': ['-r', 'no_indicator',],
}

foreach pgc_file: pgc_files
  exe_input = custom_target(pgc_file,
    input: '@0@.pgc'.format(pgc_file),
    output: '@BASENAME@.c',
    command: ecpg_preproc_test_command_start +
      ['-C', 'INFORMIX',] +
      pgc_extra_flags.get(pgc_file, []) +
      ecpg_preproc_test_command_end
  )

  ecpg_test_dependencies += executable(pgc_file,
    exe_input,
    kwargs: ecpg_test_exec_kw,
  )
endforeach
