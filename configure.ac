AC_PREREQ([2.71])
AC_INIT([aicli],[0.0.0],[unknown])
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])

AC_CONFIG_MACRO_DIR([m4])
AC_PROG_CC

# pthreads
AX_PTHREAD

# AX_PTHREAD sets PTHREAD_CFLAGS and PTHREAD_LIBS.

# libcurl (required)
PKG_CHECK_MODULES([CURL],[libcurl])

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
