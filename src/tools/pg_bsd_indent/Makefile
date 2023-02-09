#-------------------------------------------------------------------------
#
# src/tools/pg_bsd_indent/Makefile
#
# Copyright (c) 2017-2023, PostgreSQL Global Development Group
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_bsd_indent - indent C code nicely"
PGAPPICON = win32

subdir = src/tools/pg_bsd_indent
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	args.o \
	err.o \
	indent.o \
	io.o \
	lexi.o \
	parse.o \
	pr_comment.o

all: pg_bsd_indent

pg_bsd_indent: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_bsd_indent$(X) '$(DESTDIR)$(bindir)/pg_bsd_indent$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_bsd_indent$(X)'

clean distclean maintainer-clean:
	rm -f pg_bsd_indent$(X) $(OBJS)
	rm -f *.out *.list tests.diff

# We don't use "make check" because that would insist on building
# all of the surrounding Postgres installation.
.PHONY: test

test: pg_bsd_indent
	@rm -f tests.diff
	@cp $(srcdir)/tests/*.list .
	@for testsrc in $(srcdir)/tests/*.0; do \
		test=`basename "$$testsrc" .0`; \
		./pg_bsd_indent $$testsrc $$test.out -P$(srcdir)/tests/$$test.pro || echo FAILED >>$$test.out; \
		diff -u $$testsrc.stdout $$test.out >>tests.diff 2>&1 || true; \
	done
	@cat tests.diff
	@test '!' -s tests.diff
	@echo Tests complete.
