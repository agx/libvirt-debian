# -*- buffer-read-only: t -*- vi: set ro:
# DO NOT EDIT! GENERATED AUTOMATICALLY!
# ffs.m4 serial 1
dnl Copyright (C) 2011 Free Software Foundation, Inc.
dnl This file is free software; the Free Software Foundation
dnl gives unlimited permission to copy and/or distribute it,
dnl with or without modifications, as long as this notice is preserved.

AC_DEFUN([gl_FUNC_FFS],
[
  AC_CHECK_FUNCS_ONCE([ffs])
  if test $ac_cv_func_ffs = no; then
    HAVE_FFS=0
  fi
])
