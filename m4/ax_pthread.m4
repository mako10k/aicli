# ==========================================================================
# https://www.gnu.org/software/autoconf-archive/ax_pthread.html
# ==========================================================================
#
# This file is vendored from Autoconf Archive.
# It is used only at configure-time to detect pthread flags.
#
# serial 31

AU_ALIAS([ACX_PTHREAD], [AX_PTHREAD])
AC_DEFUN([AX_PTHREAD],
[
AC_REQUIRE([AC_CANONICAL_HOST])
AC_REQUIRE([AC_PROG_CC])
AC_REQUIRE([AC_PROG_SED])
AC_LANG_PUSH([C])
ax_pthread_ok=no

# We used to check for pthread.h first, but this fails if pthread.h
# requires special compiler flags (e.g. on Tru64 or Sequent).
# It gets checked for in the link test anyway.

# First of all, check if the user has set any of the PTHREAD_LIBS,
# etcetera environment variables, and if threads linking works using
# them:
if test "x$PTHREAD_CFLAGS$PTHREAD_LIBS" != "x"; then
        ax_pthread_save_CC="$CC"
        ax_pthread_save_CFLAGS="$CFLAGS"
        ax_pthread_save_LIBS="$LIBS"
        AS_IF([test "x$PTHREAD_CC" != "x"],[CC="$PTHREAD_CC"])
        AS_IF([test "x$PTHREAD_CXX" != "x"],[CXX="$PTHREAD_CXX"])
        CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
        LIBS="$PTHREAD_LIBS $LIBS"
        AC_MSG_CHECKING([for pthread_join using $CC $PTHREAD_CFLAGS $PTHREAD_LIBS])
        AC_LINK_IFELSE([AC_LANG_CALL([], [pthread_join])],[ax_pthread_ok=yes])
        AC_MSG_RESULT([$ax_pthread_ok])
        if test "x$ax_pthread_ok" = "xno"; then
                PTHREAD_LIBS=""
                PTHREAD_CFLAGS=""
        fi
        CC="$ax_pthread_save_CC"
        CFLAGS="$ax_pthread_save_CFLAGS"
        LIBS="$ax_pthread_save_LIBS"
fi

# We must check for the threads library under a number of different
# names; the ordering is very important because some systems (e.g. DEC)
# have both -lpthread and -lpthreads, where one of the libraries is
# broken (non-POSIX).

# Create a list of thread flags to try. Items with a "," contain both
# C compiler flags (before ",") and linker flags (after ","). Other
# items starting with a "-" are C compiler flags, and remaining items
# are library names, except for "none" which indicates that we try
# without any flags at all, and "pthread-config" which is a program
# returning the flags for the Pth emulation library.

ax_pthread_flags="pthreads none -Kthread -pthread -pthreads -mthreads pthread --thread-safe -mt pthread-config"

case $host_os in
        freebsd*)
                ax_pthread_flags="-kthread lthread $ax_pthread_flags"
                ;;
        hpux*)
                ax_pthread_flags="-mt -pthread pthread $ax_pthread_flags"
                ;;
        openedition*)
                AC_EGREP_CPP([AX_PTHREAD_ZOS_MISSING],
                    [
#                       if !defined(_OPEN_THREADS) && !defined(_UNIX03_THREADS)
                        AX_PTHREAD_ZOS_MISSING
#                       endif
                    ],
                    [AC_MSG_WARN([IBM z/OS requires -D_OPEN_THREADS or -D_UNIX03_THREADS to enable pthreads support.])])
                ;;
        solaris*)
                ax_pthread_flags="-mt,-lpthread pthread $ax_pthread_flags"
                ;;
esac

AC_CACHE_CHECK([whether $CC is Clang],[ax_cv_PTHREAD_CLANG],[
ax_cv_PTHREAD_CLANG=no
if test "x$GCC" = "xyes"; then
AC_EGREP_CPP([AX_PTHREAD_CC_IS_CLANG],
            [/* Note: Clang 2.7 lacks __clang_[a-z]+__ */
#               if defined(__clang__) && defined(__llvm__)
                AX_PTHREAD_CC_IS_CLANG
#               endif
            ],
            [ax_cv_PTHREAD_CLANG=yes])
fi
])
ax_pthread_clang="$ax_cv_PTHREAD_CLANG"

AS_IF([test "x$GCC" = "xyes"],[ax_pthread_flags="-pthread,-lpthread -pthread -pthreads $ax_pthread_flags"])
AS_IF([test "x$ax_pthread_clang" = "xyes"],[ax_pthread_flags="-pthread,-lpthread -pthread"])

case $host_os in
        darwin* | hpux* | linux* | osf* | solaris*)
                ax_pthread_check_macro="_REENTRANT"
                ;;
        aix*)
                ax_pthread_check_macro="_THREAD_SAFE"
                ;;
        *)
                ax_pthread_check_macro="--"
                ;;
esac
AS_IF([test "x$ax_pthread_check_macro" = "x--"],[ax_pthread_check_cond=0],[ax_pthread_check_cond="!defined($ax_pthread_check_macro)"])

if test "x$ax_pthread_ok" = "xno"; then
for ax_pthread_try_flag in $ax_pthread_flags; do
        case $ax_pthread_try_flag in
                none)
                        AC_MSG_CHECKING([whether pthreads work without any flags])
                        PTHREAD_CFLAGS=""
                        PTHREAD_LIBS=""
                        ;;
                *,*)
                        PTHREAD_CFLAGS=`echo $ax_pthread_try_flag | sed "s/^\(.*\),\(.*\)$/\1/"`
                        PTHREAD_LIBS=`echo $ax_pthread_try_flag | sed "s/^\(.*\),\(.*\)$/\2/"`
                        AC_MSG_CHECKING([whether pthreads work with "$PTHREAD_CFLAGS" and "$PTHREAD_LIBS"]) 
                        ;;
                -*)
                        AC_MSG_CHECKING([whether pthreads work with $ax_pthread_try_flag])
                        PTHREAD_CFLAGS="$ax_pthread_try_flag"
                        PTHREAD_LIBS=""
                        ;;
                pthread-config)
                        AC_CHECK_PROG([ax_pthread_config], [pthread-config], [yes], [no])
                        AS_IF([test "x$ax_pthread_config" = "xno"], [continue])
                        PTHREAD_CFLAGS="`pthread-config --cflags`"
                        PTHREAD_LIBS="`pthread-config --ldflags` `pthread-config --libs`"
                        ;;
                *)
                        AC_MSG_CHECKING([for the pthreads library -l$ax_pthread_try_flag])
                        PTHREAD_CFLAGS=""
                        PTHREAD_LIBS="-l$ax_pthread_try_flag"
                        ;;
        esac

        ax_pthread_save_CFLAGS="$CFLAGS"
        ax_pthread_save_LIBS="$LIBS"
        CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
        LIBS="$PTHREAD_LIBS $LIBS"

        AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <pthread.h>
#                       if $ax_pthread_check_cond
#                        error "$ax_pthread_check_macro must be defined"
#                       endif
                        static void *some_global = NULL;
                        static void routine(void *a) { some_global = a; }
                        static void *start_routine(void *a) { return a; }]],
                       [[pthread_t th; pthread_attr_t attr;
                        pthread_create(&th, 0, start_routine, 0);
                        pthread_join(th, 0);
                        pthread_attr_init(&attr);
                        pthread_cleanup_push(routine, 0);
                        pthread_cleanup_pop(0);]])],
                       [ax_pthread_ok=yes],
                       [ax_pthread_ok=no])

        CFLAGS="$ax_pthread_save_CFLAGS"
        LIBS="$ax_pthread_save_LIBS"

        AC_MSG_RESULT([$ax_pthread_ok])
        AS_IF([test "x$ax_pthread_ok" = "xyes"], [break])

        PTHREAD_LIBS=""
        PTHREAD_CFLAGS=""

done
fi

# Various other checks:
if test "x$ax_pthread_ok" = "xyes"; then
        ax_pthread_save_CFLAGS="$CFLAGS"
        ax_pthread_save_LIBS="$LIBS"
        CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
        LIBS="$PTHREAD_LIBS $LIBS"

        AC_CACHE_CHECK([for joinable pthread attribute],[ax_cv_PTHREAD_JOINABLE_ATTR],[
            ax_cv_PTHREAD_JOINABLE_ATTR=unknown
            for ax_pthread_attr in PTHREAD_CREATE_JOINABLE PTHREAD_CREATE_UNDETACHED; do
                AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <pthread.h>],[int attr = $ax_pthread_attr; return attr;])],
                               [ax_cv_PTHREAD_JOINABLE_ATTR=$ax_pthread_attr; break], [])
            done
        ])

        AS_IF([test "x$ax_cv_PTHREAD_JOINABLE_ATTR" != "xunknown" && \
               test "x$ax_cv_PTHREAD_JOINABLE_ATTR" != "xPTHREAD_CREATE_JOINABLE"],
              [AC_DEFINE_UNQUOTED([PTHREAD_CREATE_JOINABLE],[$ax_cv_PTHREAD_JOINABLE_ATTR],[Define if joinable constant has a non-standard name on your system.])])

        CFLAGS="$ax_pthread_save_CFLAGS"
        LIBS="$ax_pthread_save_LIBS"

        if test "x$GCC" != "xyes"; then
            case $host_os in
            aix*)
                AC_CHECK_PROGS([PTHREAD_CC],[${CC}_r],[$CC])
                AS_IF([test "x${CXX}" != "x"],[AC_CHECK_PROGS([PTHREAD_CXX],[${CXX}_r],[$CXX])])
                ;;
            esac
        fi
fi

test -n "$PTHREAD_CC" || PTHREAD_CC="$CC"

test -n "$PTHREAD_CXX" || PTHREAD_CXX="$CXX"

AC_SUBST([PTHREAD_LIBS])
AC_SUBST([PTHREAD_CFLAGS])
AC_SUBST([PTHREAD_CC])
AC_SUBST([PTHREAD_CXX])

if test "x$ax_pthread_ok" = "xyes"; then
        ifelse([$1],,[AC_DEFINE([HAVE_PTHREAD],[1],[Define if you have POSIX threads libraries and header files.])],[$1])
        :
else
        ax_pthread_ok=no
        $2
fi
AC_LANG_POP
])
