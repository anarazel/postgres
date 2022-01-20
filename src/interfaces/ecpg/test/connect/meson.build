
pgc_files = [
  'test1',
  'test2',
  'test3',
  'test4',
  'test5',
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
