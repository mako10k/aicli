AC_PREREQ([2.71])
AC_INIT([aicli],[0.0.0],[unknown])
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])

AC_USE_SYSTEM_EXTENSIONS

AC_CONFIG_MACRO_DIR([m4])
AC_PROG_CC

# pthreads
AX_PTHREAD

# AX_PTHREAD sets PTHREAD_CFLAGS and PTHREAD_LIBS.

# libcurl (required)
PKG_CHECK_MODULES([CURL],[libcurl])

# md4c (optional; --enable-md4c/--disable-md4c)
AC_ARG_ENABLE([md4c],
  [AS_HELP_STRING([--enable-md4c], [Enable Markdown support via md4c (default=auto)])],
  [],
  [enable_md4c=auto])

have_md4c=no
have_vendored_md4c=no
MD4C_CFLAGS=""
MD4C_LIBS=""

AS_CASE([$enable_md4c],
  [yes|no|auto], [],
  [AC_MSG_ERROR([invalid value for --enable-md4c: $enable_md4c (use yes/no/auto)])])

AS_IF([test "x$enable_md4c" != "xno"], [
  AC_MSG_CHECKING([for vendored md4c])
  if test -f "${srcdir}/vendor/md4c/src/md4c.h" && test -f "${srcdir}/vendor/md4c/src/md4c.c"; then
    AC_MSG_RESULT([yes])
    have_md4c=yes
    have_vendored_md4c=yes
  else
    AC_MSG_RESULT([no])
    PKG_CHECK_MODULES([MD4C], [md4c], [have_md4c=yes], [have_md4c=no])
  fi
])

AS_IF([test "x$have_md4c" = "xyes"], [
  AC_DEFINE([HAVE_MD4C], [1], [Define to 1 if md4c support is enabled.])
], [
  AS_IF([test "x$enable_md4c" = "xyes"], [
    AC_MSG_ERROR([md4c requested but not found (vendor/md4c/src or pkg-config module "md4c")])
  ], [
    AC_MSG_NOTICE([md4c not found; Markdown support disabled])
  ])
])

AM_CONDITIONAL([HAVE_MD4C], [test "x$have_md4c" = "xyes"])
AM_CONDITIONAL([HAVE_VENDORED_MD4C], [test "x$have_vendored_md4c" = "xyes"])
AC_SUBST([MD4C_CFLAGS])
AC_SUBST([MD4C_LIBS])

# POSIX regex (for execute DSL advanced features)
AC_CHECK_HEADERS([regex.h])
AC_SEARCH_LIBS([regcomp], [regex], [], [])
AC_CHECK_FUNCS([regcomp regexec regfree regerror])

# vendored yyjson
AC_MSG_NOTICE([checking for vendored yyjson... yes])
AC_DEFINE([HAVE_YYJSON_H],[1],[Define to 1 if you have the <yyjson.h> header file.])
# yyjson: prefer vendored copy; fall back to system header
AC_MSG_CHECKING([for vendored yyjson])
if test -f "${srcdir}/vendor/yyjson/yyjson.h"; then
  AC_MSG_RESULT([yes])
  have_yyjson=yes
else
  AC_MSG_RESULT([no])
  AC_CHECK_HEADERS([yyjson.h], [have_yyjson=yes], [have_yyjson=no])
fi
AM_CONDITIONAL([HAVE_YYJSON], [test "x$have_yyjson" = "xyes"])
AS_IF([test "x$have_yyjson" = "xno"], [
  AC_MSG_WARN([yyjson.h not found: OpenAI/Brave JSON features will be disabled until provided.])
])

AC_CONFIG_FILES([
 Makefile
 src/Makefile
 include/Makefile
 tests/Makefile
])
AC_OUTPUT
