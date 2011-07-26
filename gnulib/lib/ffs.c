/* -*- buffer-read-only: t -*- vi: set ro: */
/* DO NOT EDIT! GENERATED AUTOMATICALLY! */
/* ffs.c -- find the first set bit in a word.
   Copyright (C) 2011 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* Written by Eric Blake.  */

#include <strings.h>

int
ffs (int i)
{
#if __GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 4)
  return __builtin_ffs (i);
#else
  /* http://graphics.stanford.edu/~seander/bithacks.html#ZerosOnRightMultLookup
     gives this deBruijn constant for a branch-less computation, although
     that table counted trailing zeros rather than bit position.  */
  static unsigned int table[] = {
    1, 2, 29, 3, 30, 15, 25, 4, 31, 23, 21, 16, 26, 18, 5, 9,
    32, 28, 14, 24, 22, 20, 17, 8, 27, 13, 19, 7, 12, 6, 11, 10
  };
  unsigned int u = i;
  unsigned int bit = u & -u;
  return table[(bit * 0x077cb531U) >> 27] - !i;
#endif
}
