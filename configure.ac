AC_PREREQ([2.71])
AC_INIT([aicli],[0.0.0],[unknown])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])
AC_PROG_CC

# libcurl (required)
PKG_CHECK_MODULES([CURL],[libcurl])

# yyjson: header check (required for future JSON handling)
AC_CHECK_HEADERS([yyjson.h], [], [
  AC_MSG_ERROR([yyjson.h not found. Install yyjson development headers or vendor yyjson.h into an include path.])
])

AC_CONFIG_FILES([
 Makefile
 src/Makefile
 include/Makefile
 tests/Makefile
])
AC_OUTPUT
