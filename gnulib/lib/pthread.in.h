/* -*- buffer-read-only: t -*- vi: set ro: */
/* DO NOT EDIT! GENERATED AUTOMATICALLY! */
/* Implement a trivial subset of the pthreads library.

   Copyright (C) 2009, 2010 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 2.1, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.  */

/* Written by Glen Lenker.  */

#ifndef PTHREAD_H_
# define PTHREAD_H_

# include <errno.h>
# include <stdlib.h>

typedef int pthread_t;
typedef int pthread_attr_t;

static int
pthread_create (pthread_t *restrict thread,
                const pthread_attr_t *restrict attr,
                void *(*start_routine)(void*), void *restrict arg)
{
  errno = EAGAIN;
  return -1;
}

static int
pthread_join (pthread_t thread, void **value_ptr)
{
  abort ();
  return -1;
}

#endif /* PTHREAD_H_ */
