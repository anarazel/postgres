# Copyright (c) 2022-2023, PostgreSQL Global Development Group

pg_bsd_indent_sources = files(
  'args.c',
  'err.c',
  'indent.c',
  'io.c',
  'lexi.c',
  'parse.c',
  'pr_comment.c',
)

if host_system == 'windows'
  pg_bsd_indent_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_bsd_indent',
    '--FILEDESC', 'pg_bsd_indent - indent C code nicely'])
endif

pg_bsd_indent = executable('pg_bsd_indent',
  pg_bsd_indent_sources,
  dependencies: [frontend_code],
  include_directories: include_directories('.'),
  kwargs: default_bin_args,
)
bin_targets += pg_bsd_indent
