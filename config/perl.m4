# config/perl.m4


# PGAC_PATH_PERL
# --------------
AC_DEFUN([PGAC_PATH_PERL],
[PGAC_PATH_PROGS(PERL, perl)
AC_ARG_VAR(PERL, [Perl program])dnl

if test "$PERL"; then
  pgac_perl_version=`$PERL -v 2>/dev/null | sed -n ['s/This is perl.*v[a-z ]*\([0-9]\.[0-9][0-9.]*\).*$/\1/p']`
  AC_MSG_NOTICE([using perl $pgac_perl_version])
  if echo "$pgac_perl_version" | sed ['s/[.a-z_]/ /g'] | \
    $AWK '{ if ([$]1 == 5 && ([$]2 >= 14)) exit 1; else exit 0;}'
  then
    AC_MSG_WARN([
*** The installed version of Perl, $PERL, is too old to use with PostgreSQL.
*** Perl version 5.14 or later is required, but this is $pgac_perl_version.])
    PERL=""
  fi
fi

if test -z "$PERL"; then
  AC_MSG_WARN([
*** Without Perl you will not be able to build PostgreSQL from Git.
*** You can obtain Perl from any CPAN mirror site.
*** (If you are using the official distribution of PostgreSQL then you do not
*** need to worry about this, because the Perl output is pre-generated.)])
fi
])# PGAC_PATH_PERL


# PGAC_CHECK_PERL_CONFIG(NAME)
# ----------------------------
AC_DEFUN([PGAC_CHECK_PERL_CONFIG],
[AC_REQUIRE([PGAC_PATH_PERL])
AC_MSG_CHECKING([for Perl $1])
perl_$1=`$PERL -MConfig -e 'print $Config{$1}'`
test "$PORTNAME" = "win32" && perl_$1=`echo $perl_$1 | sed 's,\\\\,/,g'`
AC_SUBST(perl_$1)dnl
AC_MSG_RESULT([$perl_$1])])


# PGAC_CHECK_PERL_CONFIGS(NAMES)
# ------------------------------
AC_DEFUN([PGAC_CHECK_PERL_CONFIGS],
[m4_foreach([pgac_item], [$1], [PGAC_CHECK_PERL_CONFIG(pgac_item)])])


# PGAC_CHECK_PERL_EMBED_CCFLAGS
# -----------------------------
# We selectively extract stuff from $Config{ccflags}.  For debugging purposes,
# let's have the configure output report the raw ccflags value as well as the
# set of flags we chose to adopt.  We don't really need anything except -D
# switches, and other sorts of compiler switches can actively break things if
# Perl was compiled with a different compiler.  Moreover, although Perl likes
# to put stuff like -D_LARGEFILE_SOURCE and -D_FILE_OFFSET_BITS=64 here, it
# would be fatal to try to compile PL/Perl to a different libc ABI than core
# Postgres uses.  The available information says that most symbols that affect
# Perl's own ABI begin with letters, so it's almost sufficient to adopt -D
# switches for symbols not beginning with underscore.  Some exceptions are the
# Windows-specific -D_USE_32BIT_TIME_T and -D__MINGW_USE_VC2005_COMPAT; see
# Mkvcbuild.pm for details.  We absorb the former when Perl reports it.  Perl
# never reports the latter, and we don't attempt to deduce when it's needed.
# Consequently, we don't support using MinGW to link to MSVC-built Perl.  As
# of 2017, all supported ActivePerl and Strawberry Perl are MinGW-built.  If
# that changes or an MSVC-built Perl distribution becomes prominent, we can
# revisit this limitation.
#
# FIXME FIXME FIXME: Referenced Mkvcbuild.pm comment:
#
# Windows offers several 32-bit ABIs.  Perl is sensitive to
# sizeof(time_t), one of the ABI dimensions.  To get 32-bit time_t,
# use "cl -D_USE_32BIT_TIME_T" or plain "gcc".  For 64-bit time_t, use
# "gcc -D__MINGW_USE_VC2005_COMPAT" or plain "cl".  Before MSVC 2005,
# plain "cl" chose 32-bit time_t.  PostgreSQL doesn't support building
# with pre-MSVC-2005 compilers, but it does support linking to Perl
# built with such a compiler.  MSVC-built Perl 5.13.4 and later report
# -D_USE_32BIT_TIME_T in $Config{ccflags} if applicable, but
# MinGW-built Perl never reports -D_USE_32BIT_TIME_T despite typically
# needing it.  Ignore the $Config{ccflags} opinion about
# -D_USE_32BIT_TIME_T, and use a runtime test to deduce the ABI Perl
# expects.  Specifically, test use of PL_modglobal, which maps to a
# PerlInterpreter field whose position depends on sizeof(time_t).
#
AC_DEFUN([PGAC_CHECK_PERL_EMBED_CCFLAGS],
[AC_REQUIRE([PGAC_PATH_PERL])
AC_MSG_CHECKING([for CFLAGS recommended by Perl])
perl_ccflags=`$PERL -MConfig -e ['print $Config{ccflags}']`
AC_MSG_RESULT([$perl_ccflags])
AC_MSG_CHECKING([for CFLAGS to compile embedded Perl])
perl_embed_ccflags=`$PERL -MConfig -e ['foreach $f (split(" ",$Config{ccflags})) {print $f, " " if ($f =~ /^-D[^_]/ || $f =~ /^-D_USE_32BIT_TIME_T/)}']`
AC_SUBST(perl_embed_ccflags)dnl
AC_MSG_RESULT([$perl_embed_ccflags])
])# PGAC_CHECK_PERL_EMBED_CCFLAGS


# PGAC_CHECK_PERL_EMBED_LDFLAGS
# -----------------------------
# We are after Embed's ldopts, but without the subset mentioned in
# Config's ccdlflags and ldflags.  (Those are the choices of those who
# built the Perl installation, which are not necessarily appropriate
# for building PostgreSQL.)
AC_DEFUN([PGAC_CHECK_PERL_EMBED_LDFLAGS],
[AC_REQUIRE([PGAC_PATH_PERL])
AC_MSG_CHECKING(for flags to link embedded Perl)
if test "$PORTNAME" = "win32" ; then
	perl_lib=`basename $perl_archlibexp/CORE/perl[[5-9]]*.lib .lib`
	if test -e "$perl_archlibexp/CORE/$perl_lib.lib"; then
		perl_embed_ldflags="-L$perl_archlibexp/CORE -l$perl_lib"
	else
		perl_lib=`basename $perl_archlibexp/CORE/libperl[[5-9]]*.a .a | sed 's/^lib//'`
		if test -e "$perl_archlibexp/CORE/lib$perl_lib.a"; then
			perl_embed_ldflags="-L$perl_archlibexp/CORE -l$perl_lib"
		fi
	fi
else
	pgac_tmp1=`$PERL -MExtUtils::Embed -e ldopts`
	pgac_tmp2=`$PERL -MConfig -e 'print "$Config{ccdlflags} $Config{ldflags}"'`
	perl_embed_ldflags=`echo X"$pgac_tmp1" | sed -e "s/^X//" -e "s%$pgac_tmp2%%"`
fi
AC_SUBST(perl_embed_ldflags)dnl
if test -z "$perl_embed_ldflags" ; then
	AC_MSG_RESULT(no)
	AC_MSG_ERROR([could not determine flags for linking embedded Perl.
This probably means that ExtUtils::Embed or ExtUtils::MakeMaker is not
installed.])
else
	AC_MSG_RESULT([$perl_embed_ldflags])
fi
])# PGAC_CHECK_PERL_EMBED_LDFLAGS
