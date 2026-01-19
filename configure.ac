AC_PREREQ([2.71])
AC_INIT([aicli],[0.0.0],[unknown])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])
AC_PROG_CC

# libcurl (required)
PKG_CHECK_MODULES([CURL],[libcurl])

# yyjson: header check (optional for now; required once JSON-based features are implemented)
AC_CHECK_HEADERS([yyjson.h], [have_yyjson=yes], [have_yyjson=no])
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
