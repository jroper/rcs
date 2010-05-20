/* b-fro.h --- read-only file

   Copyright (C) 2010 Thien-Thi Nguyen
   Copyright (C) 1990, 1991, 1992, 1993, 1994, 1995 Paul Eggert
   Copyright (C) 1982, 1988, 1989 Walter Tichy

   This file is part of GNU RCS.

   GNU RCS is free software: you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 2 of the License, or
   (at your option) any later version.

   GNU RCS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   See the GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

struct range
{
  off_t beg;
  off_t end;
};

enum readmethod
  {
    RM_MMAP,
    RM_MEM,
    RM_STDIO
  };

struct fro
{
  int fd;
  off_t end;
  enum readmethod rm;
  char *ptr, *lim, *base;
  void (*deallocate) (struct fro *);
  FILE *stream;
};

extern struct fro *fro_open (char const *filename, char const *type,
                             struct stat *status);
extern void fro_zclose (struct fro **p);
extern void fro_close (struct fro *f);
extern off_t fro_tello (struct fro *f);
extern void fro_move (struct fro *f, off_t change);
extern bool fro_try_getbyte (int *c, struct fro *f);
extern void fro_must_getbyte (int *c, struct fro *f);
extern void fro_trundling (bool sequentialp, struct fro *f);
extern void fro_spew_partial (FILE *to, struct fro *f, struct range *r);
extern void fro_spew (struct fro *f, FILE *to);

/* Idioms.  */

#define fro_bob(f)  fro_move (f, 0)

#define STDIO_P(f)  (RM_STDIO == (f)->rm)

/* Get a char into ‘c’ from ‘f’, executing statement ‘s’ at EOF.  */
#define GETCHAR_OR(c,f,s)  do                   \
    if (fro_try_getbyte (&(c), (f)))            \
      { s; }                                    \
  while (0)

/* Like ‘GETCHAR_OR’, except EOF is an error.  */
#define GETCHAR(c,f)  fro_must_getbyte (&(c), (f))

/* b-fro.h ends here */
