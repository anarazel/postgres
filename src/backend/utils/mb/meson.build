backend_sources += files(
  'conv.c',
  'mbutils.c',
  'stringinfo_mb.c',
  'wstrcmp.c',
  'wstrncmp.c',
)


encodings = [
  'cyrillic_and_mic',
  'euc2004_sjis2004',
  'euc_cn_and_mic',
  'euc_jp_and_sjis',
  'euc_kr_and_mic',
  'euc_tw_and_big5',
  'latin2_and_win1250',
  'latin_and_mic',
  'utf8_and_big5',
  'utf8_and_cyrillic',
  'utf8_and_euc2004',
  'utf8_and_euc_cn',
  'utf8_and_euc_jp',
  'utf8_and_euc_kr',
  'utf8_and_euc_tw',
  'utf8_and_gb18030',
  'utf8_and_gbk',
  'utf8_and_iso8859',
  'utf8_and_iso8859_1',
  'utf8_and_johab',
  'utf8_and_sjis',
  'utf8_and_sjis2004',
  'utf8_and_uhc',
  'utf8_and_win',
]

foreach encoding : encodings
  shared_module(encoding,
    'conversion_procs' / encoding / (encoding + '.c'),
    dependencies: [backend_code],
    name_prefix: '',
    install: true)
endforeach
