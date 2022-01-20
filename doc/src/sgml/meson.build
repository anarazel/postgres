alldocs = []
doc_generated = []

xmllint = find_program('xmllint', native: true, required: false)
pandoc = find_program('pandoc', native: true, required: false)
xsltproc = find_program('xsltproc', native: true, required: false)
fop = find_program('fop', native: true, required: false)


configure_file(
  input: 'version.sgml.in',
  output: 'version.sgml',
  configuration: cdata,
)

doc_generated += custom_target('features-supported.sgml',
  input: files(
    '../../../src/backend/catalog/sql_feature_packages.txt',
    '../../../src/backend/catalog/sql_features.txt'),
  output: 'features-supported.sgml',
  command: [perl, files('mk_feature_tables.pl'), 'YES', '@INPUT@'],
  build_by_default: false,
  install: false,
  capture: true)

doc_generated += custom_target('features-unsupported.sgml',
  input: files(
    '../../../src/backend/catalog/sql_feature_packages.txt',
    '../../../src/backend/catalog/sql_features.txt'),
  output: 'features-unsupported.sgml',
  command: [perl, files('mk_feature_tables.pl'), 'NO', '@INPUT@'],
  build_by_default: false,
  install: false,
  capture: true)

doc_generated += custom_target('errcodes-table.sgml',
  input: files(
    '../../../src/backend/utils/errcodes.txt'),
  output: 'errcodes-table.sgml',
  command: [perl, files('generate-errcodes-table.pl'), '@INPUT@'],
  build_by_default: false,
  install: false,
  capture: true)

# FIXME: this actually has further inputs, adding depfile support to
# generate-keywords-table.pl is probably the best way to address that
# robustly.
doc_generated += custom_target('keywords-table.sgml',
  input: files(
    '../../../src/include/parser/kwlist.h'),
  output: 'keywords-table.sgml',
  command: [perl, files('generate-keywords-table.pl'), '@CURRENT_SOURCE_DIR@'],
  build_by_default: false,
  install: false,
  capture: true)

# For everything else we need at least xmllint
if not xmllint.found()
  subdir_done()
endif

# Compute validity just once
postgres_sgml_valid = custom_target('postgres.sgml.valid',
  input: 'postgres.sgml',
  output: 'postgres.sgml.valid',
  command: [xmllint, '--noout', '--valid', '--path', '@OUTDIR@', '@INPUT@'],
  depends: doc_generated,
  build_by_default: true,
  capture: true,
)
alldocs += postgres_sgml_valid


#
# Full documentation as html, text
#
if xsltproc.found()
  xsltproc_flags = [
    '--stringparam', 'pg.version', pg_version,
    '--param', 'website.stylesheet', '1'
  ]


  # FIXME: Should use a wrapper around xsltproc --load-trace to compute a
  # depfile
  html = custom_target('html',
    input: ['stylesheet.xsl', 'postgres.sgml'],
    output: 'html',
    depends: doc_generated + [postgres_sgml_valid],
    command: [xsltproc, '--path', '@OUTDIR@', '-o', '@OUTDIR@/', xsltproc_flags, '@INPUT@'],
    build_by_default: false,
  )
  alldocs += html


  html_help = custom_target('html_help',
    input: ['stylesheet-hh.xsl', 'postgres.sgml'],
    output: 'htmlhelp',
    depends: doc_generated + [postgres_sgml_valid],
    command: [xsltproc, '--path', '@OUTDIR@', '-o', '@OUTDIR@/', xsltproc_flags, '@INPUT@'],
    build_by_default: false,
  )
  alldocs += html_help


  # single-page HTML
  postgres_html = custom_target('postgres.html',
    input: ['stylesheet-html-nochunk.xsl', 'postgres.sgml'],
    output: 'postgres.html',
    depends: doc_generated + [postgres_sgml_valid],
    command: [xsltproc, '--path', '@OUTDIR@', '-o', '@OUTPUT@', xsltproc_flags, '@INPUT@'],
    build_by_default: false,
  )
  alldocs += postgres_html

  # single-page text
  if pandoc.found()
    postgres_txt = custom_target('postgres.txt',
      input: [postgres_html],
      output: 'postgres.txt',
      depends: doc_generated + [postgres_sgml_valid],
      command: [pandoc, '-t', 'plain', '-o', '@OUTPUT@', '@INPUT@'],
      build_by_default: false,
    )
    alldocs += postgres_txt
  endif
endif


#
# INSTALL in html, text
#
if xsltproc.found()
  install_xml = custom_target('INSTALL.xml',
    input: ['standalone-profile.xsl', 'standalone-install.xml'],
    output: 'INSTALL.xml',
    depends: doc_generated + [postgres_sgml_valid],
    command: [xsltproc, '--path', '@OUTDIR@', '-o', '@OUTPUT@', xsltproc_flags, '--xinclude', '@INPUT@'],
    build_by_default: false,
  )
  install_html = custom_target('INSTALL.html',
    input: ['stylesheet-text.xsl', install_xml],
    output: 'INSTALL.html',
    depends: doc_generated + [postgres_sgml_valid],
    command: [xsltproc, '--path', '@OUTDIR@', '-o', '@OUTPUT@', xsltproc_flags, '@INPUT@'],
    build_by_default: false,
  )
  alldocs += install_html

  if pandoc.found()
    # XXX: Makefile does an iconv translit here, but unclear why?
    install = custom_target('INSTALL',
      input: [install_html],
      output: 'INSTALL',
      depends: doc_generated + [postgres_sgml_valid],
      command: [pandoc, '-t', 'plain', '-o', '@OUTPUT@', '@INPUT@'],
      build_by_default: false,
    )
    alldocs += postgres_txt
  endif

endif


#
# Man pages
#
if xsltproc.found()
  # FIXME: implement / consider sqlmansectnum logic
  man = custom_target('man',
    input: ['stylesheet-man.xsl', 'postgres.sgml'],
    output: ['man1', 'man3', 'man7'],
    depends: doc_generated + [postgres_sgml_valid],
    command: [xsltproc, '--path', '@OUTDIR@', '-o', '@OUTDIR@/', xsltproc_flags, '@INPUT@'],
    build_by_default: false,
  )
endif


#
# Full documentation as PDF
#
if fop.found() and xsltproc.found()
  xsltproc_fo_flags = xsltproc_flags + ['--stringparam', 'img.src.path', meson.current_source_dir() + '/']

  foreach format, detail: {'A4': 'A4', 'US': 'USletter'}
    postgres_x_fo_f = 'postgres-@0@.fo'.format(format)
    postgres_x_pdf_f = 'postgres-@0@.pdf'.format(format)

    postgres_x_fo = custom_target(postgres_x_fo_f,
      input: ['stylesheet-fo.xsl', 'postgres.sgml'],
      output: [postgres_x_fo_f],
      depends: doc_generated + [postgres_sgml_valid],
      command: [xsltproc, '--path', '@OUTDIR@/', xsltproc_fo_flags,
                '--stringparam', 'paper.type', detail,
                '-o', '@OUTPUT@', '@INPUT@'],
      build_by_default: false,
    )

    postgres_x_pdf = custom_target(postgres_x_pdf_f,
      input: [postgres_x_fo],
      output: [postgres_x_pdf_f],
      command: [fop, '-fo', '@INPUT@', '-pdf', '@OUTPUT@'],
      build_by_default: false,
    )
    alldocs += postgres_x_pdf
  endforeach
endif


#
# epub
#

# This was previously implemented using dbtoepub - but that doesn't seem to
# support running in build != source directory (i.e. VPATH builds already
# weren't supported).
if pandoc.found() and xsltproc.found()
  # XXX: Wasn't able to make pandoc successfully resolve entities
  # XXX: Perhaps we should just make all targets use this, to avoid repeatedly
  # building whole thing? It's comparatively fast though.
  postgres_full_xml = custom_target('postgres-full.xml',
    input: ['resolv.xsl', 'postgres.sgml'],
    output: ['postgres-full.xml'],
    depends: doc_generated + [postgres_sgml_valid],
    command: [xsltproc, '--path', '@OUTDIR@/', xsltproc_flags,
              '-o', '@OUTPUT@', '@INPUT@'],
    build_by_default: false,
  )

  postgres_epub = custom_target('postgres.epub',
    input: [postgres_full_xml],
    output: 'postgres.epub',
    command: [pandoc, '-f', 'docbook', '-t', 'epub', '-o', '@OUTPUT@', '--resource-path=@CURRENT_SOURCE_DIR@',
              '@INPUT@'],
    build_by_default: false,
  )
  alldocs += postgres_epub
endif


alias_target('alldocs', alldocs)
