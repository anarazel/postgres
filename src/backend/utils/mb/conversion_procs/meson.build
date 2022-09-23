encodings = {
  'cyrillic_and_mic': {
    'sources': ['cyrillic_and_mic/cyrillic_and_mic.c'],
    'description': 'cyrillic <-> mic text conversions',
  },
  'euc2004_sjis2004': {
    'sources': ['euc2004_sjis2004/euc2004_sjis2004.c'],
    'description': 'euc2004 <-> sjis2004 text conversions',
  },
  'euc_cn_and_mic': {
    'sources': ['euc_cn_and_mic/euc_cn_and_mic.c'],
    'description': 'euc_cn <-> mic text conversions',
  },
  'euc_jp_and_sjis': {
    'sources': ['euc_jp_and_sjis/euc_jp_and_sjis.c'],
    'description': 'euc_jp <-> sjis text conversions',
  },
  'euc_kr_and_mic': {
    'sources': ['euc_kr_and_mic/euc_kr_and_mic.c'],
    'description': 'euc_kr <-> mic text conversions',
  },
  'euc_tw_and_big5': {
    'sources': [
      'euc_tw_and_big5/euc_tw_and_big5.c',
      'euc_tw_and_big5/big5.c',
    ],
    'description': 'euc_tw <-> big5 text conversions',
  },
  'latin2_and_win1250': {
    'sources': ['latin2_and_win1250/latin2_and_win1250.c'],
    'description': 'latin2 <-> win1250 text conversions',
  },
  'latin_and_mic': {
    'sources': ['latin_and_mic/latin_and_mic.c'],
    'description': 'latin <-> mic text conversions',
  },
  'utf8_and_big5': {
    'sources': ['utf8_and_big5/utf8_and_big5.c'],
    'description': 'utf8 <-> big5 text conversions',
  },
  'utf8_and_cyrillic': {
    'sources': ['utf8_and_cyrillic/utf8_and_cyrillic.c'],
    'description': 'utf8 <-> cyrillic text conversions',
  },
  'utf8_and_euc2004': {
    'sources': ['utf8_and_euc2004/utf8_and_euc2004.c'],
    'description': 'utf8 <-> euc2004 text conversions',
  },
  'utf8_and_euc_cn': {
    'sources': ['utf8_and_euc_cn/utf8_and_euc_cn.c'],
    'description': 'utf8 <-> euc_cn text conversions',
  },
  'utf8_and_euc_jp': {
    'sources': ['utf8_and_euc_jp/utf8_and_euc_jp.c'],
    'description': 'utf8 <-> euc_jp text conversions',
  },
  'utf8_and_euc_kr': {
    'sources': ['utf8_and_euc_kr/utf8_and_euc_kr.c'],
    'description': 'utf8 <-> euc_kr text conversions',
  },
  'utf8_and_euc_tw': {
    'sources': ['utf8_and_euc_tw/utf8_and_euc_tw.c'],
    'description': 'utf8 <-> euc_tw text conversions',
  },
  'utf8_and_gb18030': {
    'sources': ['utf8_and_gb18030/utf8_and_gb18030.c'],
    'description': 'utf8 <-> gb18030 text conversions',
  },
  'utf8_and_gbk': {
    'sources': ['utf8_and_gbk/utf8_and_gbk.c'],
    'description': 'utf8 <-> gbk text conversions',
  },
  'utf8_and_iso8859': {
    'sources': ['utf8_and_iso8859/utf8_and_iso8859.c'],
    'description': 'utf8 <-> iso8859 text conversions',
  },
  'utf8_and_iso8859_1': {
    'sources': ['utf8_and_iso8859_1/utf8_and_iso8859_1.c'],
    'description': 'utf8 <-> iso8859_1 text conversions',
  },
  'utf8_and_johab': {
    'sources': ['utf8_and_johab/utf8_and_johab.c'],
    'description': 'utf8 <-> johab text conversions',
  },
  'utf8_and_sjis': {
    'sources': ['utf8_and_sjis/utf8_and_sjis.c'],
    'description': 'utf8 <-> sjis text conversions',
  },
  'utf8_and_sjis2004': {
    'sources': ['utf8_and_sjis2004/utf8_and_sjis2004.c'],
    'description': 'utf8 <-> sjis2004 text conversions',
  },
  'utf8_and_uhc': {
    'sources': ['utf8_and_uhc/utf8_and_uhc.c'],
    'description': 'utf8 <-> uhc text conversions',
  },
  'utf8_and_win': {
    'sources': ['utf8_and_win/utf8_and_win.c'],
    'description': 'utf8 <-> win text conversions',
  },
}

foreach encoding, values: encodings
  sources = values['sources']
  description = values['description']

  source_files = files()
  foreach source: sources
    source_files += files(source)
  endforeach

  if host_system == 'windows'
    source_files += rc_lib_gen.process(win32ver_rc, extra_args: [
      '--NAME', encoding,
      '--FILEDESC', description,])
  endif

  backend_targets += shared_module(encoding,
    source_files,
    kwargs: pg_mod_args,
  )
endforeach
