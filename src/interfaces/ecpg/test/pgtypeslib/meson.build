pgc_files = [
  'dt_test',
  'dt_test2',
  'num_test',
  'num_test2',
  'nan_test',
]

foreach pgc_file: pgc_files
  exe_input = custom_target(pgc_file,
    input: '@0@.pgc'.format(pgc_file),
    output: '@BASENAME@.c',
    command: ecpg_preproc_test_command_start +
      ecpg_preproc_test_command_end
  )

  ecpg_test_dependencies += executable(pgc_file,
    exe_input,
    kwargs: ecpg_test_exec_kw,
  )
endforeach
