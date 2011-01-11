/* RCS stream editor

   Copyright (C) 2010, 2011 Thien-Thi Nguyen
   Copyright (C) 1990, 1991, 1992, 1993, 1994, 1995 Paul Eggert
   Copyright (C) 1982, 1988, 1989 Walter Tichy

   This file is part of GNU RCS.

   GNU RCS is free software: you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   GNU RCS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   See the GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/******************************************************************************
 *                       edits the input file according to a
 *                       script from stdin, generated by diff -n
 ******************************************************************************
 */

#include "base.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#ifdef HAVE_UTIME_H
#include <utime.h>
#endif
#include <fcntl.h>
#include <ctype.h>
#include "same-inode.h"
#include "unistd-safer.h"
#include "b-complain.h"
#include "b-divvy.h"
#include "b-esds.h"
#include "b-excwho.h"
#include "b-fb.h"
#include "b-feph.h"
#include "b-fro.h"
#include "b-isr.h"
#include "b-kwxout.h"

/* This is needed for dietlibc, according to Mike Mestnik.
   Hmm, shouldn't gnulib handle this?  */
#ifndef _POSIX_SYMLOOP_MAX
#define _POSIX_SYMLOOP_MAX 8
#endif

struct editstuff
{
  struct fro *fedit;
  char const *filename;
  /* Edit file stream and filename.  */

  size_t script_lno;
  /* Line number of the current edit script (for error reporting).  */

  long lcount;
  /* Edit line counter; #lines before cursor.  */
  long corr;
  /* #adds - #deletes in each edit run, used to correct ‘es->lcount’
     in case file is not rewound after applying one delta.  */

  char **line;
  size_t gap, gapsize, lim;
  /* ‘line’ contains pointers to the lines in the currently "edited" file.
     It is a 0-origin array that represents ‘lim - gapsize’ lines.
     ‘line[0 .. gap-1]’ and ‘line[gap+gapsize .. lim-1]’ hold pointers to lines.
     ‘line[gap .. gap+gapsize-1]’ contains garbage.

     Any '@'s in lines are duplicated.  Lines are terminated by '\n',
     or (for a last partial line only) by single '@'.  */

  struct sff *sff;
};

#define lockname    (BE (sff)[SFFI_LOCKDIR].filename)
#define newRCSname  (BE (sff)[SFFI_NEWDIR].filename)

struct editstuff *
make_editstuff (void)
{
  return ZLLOC (1, struct editstuff);
}

void
unmake_editstuff (struct editstuff *es)
{
  free (es->line);
  memset (es, 0, sizeof (struct editstuff));
}

static void *
okalloc (void * p)
{
  if (!p)
    oom ();
  return p;
}

static void *
testalloc (size_t size)
/* Allocate a block, testing that the allocation succeeded.  */
{
  return okalloc (malloc (size));
}

static void *
testrealloc (void *ptr, size_t size)
/* Reallocate a block, testing that the allocation succeeded.  */
{
  return okalloc (realloc (ptr, size));
}

static bool
nfs_NOENT_p (void)
{
  return has_NFS && ENOENT == errno;
}

int
un_link (char const *s)
/* Remove ‘s’, even if it is unwritable.
   Ignore ‘unlink’ ‘ENOENT’ failures; NFS generates bogus ones.  */
{
  int rv = unlink (s);

  if (!PROB (rv))
    /* Good news, don't do anything.  */
    ;
  else
    {
      if (BAD_UNLINK)
        {
          int e = errno;

          /* Forge ahead even if ‘errno == ENOENT’;
             some completely brain-damaged hosts (e.g. PCTCP 2.2)
             return ‘ENOENT’ even for existing unwritable files.  */
          if (PROB (chmod (s, S_IWUSR)))
            {
              errno = e;
              rv = -1;
            }
        }
      if (nfs_NOENT_p ())
        rv = 0;
    }
  return rv;
}

#undef SYNTAX_ERROR
#define SYNTAX_ERROR(...)  fatal_syntax (es->script_lno, __VA_ARGS__)

#define EDIT_SCRIPT_SHORT()  \
  SYNTAX_ERROR ("edit script ends prematurely")

#define EDIT_SCRIPT_OVERFLOW()  \
  SYNTAX_ERROR ("edit script refers to line past end of file")

#define movelines(s1, s2, n)  memmove (s1, s2, (n) * sizeof (char *))

static void
insertline (struct editstuff *es, unsigned long n, char *l)
/* Before line ‘n’, insert line ‘l’.  */
{
  if (es->lim - es->gapsize < n)
    EDIT_SCRIPT_OVERFLOW ();
  if (!es->gapsize)
    es->line = !es->lim
      ? testalloc (sizeof (char *) * (es->lim = es->gapsize = 1024))
      : (es->gap = es->gapsize = es->lim,
         testrealloc (es->line, sizeof (char *) * (es->lim <<= 1)));
  if (n < es->gap)
    movelines (es->line + n + es->gapsize,
               es->line + n,
               es->gap - n);
  else if (es->gap < n)
    movelines (es->line + es->gap,
               es->line + es->gap + es->gapsize,
               n - es->gap);

  es->line[n] = l;
  es->gap = n + 1;
  es->gapsize--;
}

static void
deletelines (struct editstuff *es, unsigned long n, unsigned long nlines)
/* Delete lines ‘n’ through ‘n + nlines - 1’.  */
{
  unsigned long l = n + nlines;

  if (es->lim - es->gapsize < l || l < n)
    EDIT_SCRIPT_OVERFLOW ();
  if (l < es->gap)
    movelines (es->line + l + es->gapsize,
               es->line + l,
               es->gap - l);
  else if (es->gap < n)
    movelines (es->line + es->gap,
               es->line + es->gap + es->gapsize,
               n - es->gap);

  es->gap = n;
  es->gapsize += nlines;
}

static void
snapshotline (register FILE *f, register char *l)
{
  register int c;
  do
    {
      if ((c = *l++) == SDELIM && *l++ != SDELIM)
        return;
      aputc (c, f);
    }
  while (c != '\n');
}

static void
snapshotedit_fast (struct editstuff *es, FILE *f)
/* Copy the current state of the edits to ‘f’.  */
{
  register char **p, **lim, **l = es->line;

  for (p = l, lim = l + es->gap; p < lim;)
    snapshotline (f, *p++);
  for (p += es->gapsize, lim = l + es->lim; p < lim;)
    snapshotline (f, *p++);
}

struct finctx
{
  struct expctx ctx;
  size_t script_lno;
};

static void
finisheditline (struct finctx *finctx, char *l)
{
  struct expctx *ctx = &finctx->ctx;

  ctx->from->ptr = l;
  if (expandline (ctx) < 0)
    PFATAL ("%s:%zu: error expanding keywords while applying delta %s",
            REPO (filename), finctx->script_lno, ctx->delta->num);
}

static void
finishedit_fast (struct editstuff *es, struct delta const *delta,
                 FILE *outfile, bool done)
/* Do expansion if ‘delta’ is set, output the state of the edits to
   ‘outfile’.  But do nothing unless ‘done’ is set (which means we are
   on the last pass).  */
{
  if (done)
    {
      openfcopy (outfile);
      outfile = FLOW (res);
      if (!delta)
        snapshotedit_fast (es, outfile);
      else
        {
          register char **p, **lim, **l = es->line;
          register struct fro *fin = FLOW (from);
          char *here = fin->ptr;
          struct finctx finctx =
            {
              .ctx = EXPCTX_1OUT (outfile, fin, true, true),
              .script_lno = es->script_lno
            };

          for (p = l, lim = l + es->gap; p < lim;)
            finisheditline (&finctx, *p++);
          for (p += es->gapsize, lim = l + es->lim; p < lim;)
            finisheditline (&finctx, *p++);
          fin->ptr = here;
          FINISH_EXPCTX (&finctx.ctx);
        }
    }
}

static FILE *
fopen_update_truncate (char const *name)
/* Open a temporary NAME for output, truncating any previous contents.  */
{
  if (!STDIO_P (FLOW (from)))
    return fopen_safer (name, FOPEN_W_WORK);
  else
    {
      if (BAD_FOPEN_WPLUS && PROB (un_link (name)))
        fatal_sys (name);
      return fopen_safer (name, FOPEN_WPLUS_WORK);
    }
}

void
openfcopy (FILE *f)
{
  if (!(FLOW (res) = f))
    {
      if (!FLOW (result))
        FLOW (result) = maketemp (2);
      if (!(FLOW (res) = fopen_update_truncate (FLOW (result))))
        fatal_sys (FLOW (result));
    }
}

static void
swapeditfiles (struct editstuff *es, FILE *outfile)
/* Swap ‘FLOW (result)’ and ‘es->filename’,
   set ‘es->fedit’ to manage ‘FLOW (res)’,
   and rewind ‘es->fedit’ for reading.
   Set ‘FLOW (res)’ to ‘outfile’ if non-NULL;
   otherwise, set ‘FLOW (res)’ to be ‘FLOW (result)’
   opened for reading and writing.  */
{
  char const *tmpptr;
  FILE *stream = FLOW (res);
  struct fro *f = es->fedit;

  es->lcount = 0;
  es->corr = 0;
  if (! f)
    {
      f = es->fedit = FZLLOC (struct fro);
      f->rm = RM_STDIO;
    }
  f->stream = stream;
  f->end = ftello (stream);
  Orewind (stream);
  tmpptr = es->filename;
  es->filename = FLOW (result);
  FLOW (result) = tmpptr;
  openfcopy (outfile);
}

static void
finishedit_slow (struct editstuff *es, struct delta const *delta,
                 FILE *outfile, bool done)
/* Copy the rest of the edit file and close it (if it exists).
   If ‘delta’, perform keyword substitution at the same time.
   If ‘done’ is set, we are finishing the last pass.  */
{
  register struct fro *fe;
  register FILE *fc;

  fe = es->fedit;
  if (fe)
    {
      fc = FLOW (res);
      if (delta)
        {
          struct expctx ctx = EXPCTX_1OUT (fc, fe, false, true);

          while (1 < expandline (&ctx))
            ;
          FINISH_EXPCTX (&ctx);
        }
      else
        {
          VERBATIM (fe, ftello (fe->stream));
          fro_spew (fe, fc);
        }
      fro_close (fe);
    }
  if (!done)
    swapeditfiles (es, outfile);
}

static void
snapshotedit_slow (struct editstuff *es, FILE *f)
/* Copy the current state of the edits to ‘f’.  */
{
  finishedit_slow (es, NULL, NULL, false);
  fro_spew (es->fedit, f);
  fro_bob (es->fedit);
}

void
finishedit (struct editstuff *es, struct delta const *delta,
            FILE *outfile, bool done)
{
  (!STDIO_P (FLOW (from))
   ? finishedit_fast
   : finishedit_slow) (es, delta, outfile, done);
}

void
snapshotedit (struct editstuff *es, FILE *f)
{
  (!STDIO_P (FLOW (from))
   ? snapshotedit_fast
   : snapshotedit_slow) (es, f);
}

static void
copylines (struct editstuff *es, register long upto, struct delta const *delta)
/* Copy input lines ‘es->lcount+1..upto’ from ‘es->fedit’ to ‘FLOW (res)’.
   If ‘delta’, keyword expansion is done simultaneously.
   ‘es->lcount’ is updated.  Rewinds a file only if necessary.  */
{
  if (!STDIO_P (FLOW (from)))
    es->lcount = upto;
  else
    {
      int c;
      register FILE *fc;
      register struct fro *fe;

      if (upto < es->lcount)
        {
          /* Swap files.  */
          finishedit_slow (es, NULL, NULL, false);
          /* Assumes edit only during last pass, from the beginning.  */
        }
      fe = es->fedit;
      fc = FLOW (res);
      if (es->lcount < upto)
        {
          struct expctx ctx = EXPCTX_1OUT (fc, fe, false, true);

          if (delta)
            do
              {
                if (expandline (&ctx) <= 1)
                  EDIT_SCRIPT_OVERFLOW ();
              }
            while (++es->lcount < upto);
          else
            {
              do
                {
                  do
                    {
                      GETCHAR_OR (c, fe, EDIT_SCRIPT_OVERFLOW ());
                      aputc (c, fc);
                    }
                  while (c != '\n');
                }
              while (++es->lcount < upto);
            }
          FINISH_EXPCTX (&ctx);
        }
    }
}

void
copystring (struct editstuff *es, struct atat *atat)
/* Copy a string terminated with a single ‘SDELIM’ from ‘FLOW (from)’ to
   ‘FLOW (res)’, replacing all double ‘SDELIM’ with a single ‘SDELIM’.  If
   ‘FLOW (to)’ is non-NULL, the string also copied unchanged to ‘FLOW (to)’.
   ‘es->lcount’ is incremented by the number of lines copied.  Assumption:
   next character read is first string character.  */
{
  atat_display (FLOW (res), atat, false);
  if (FLOW (to))
    atat_put (FLOW (to), atat);
  es->lcount += atat->line_count;
}

void
enterstring (struct editstuff *es, struct atat *atat)
/* Like ‘copystring’, except the string is
   put into the ‘edit’ data structure.  */
{
  if (STDIO_P (FLOW (from)))
    {
      char const *filename;

      es->filename = NULL;
      es->fedit = NULL;
      es->lcount = es->corr = 0;
      FLOW (result) = filename = maketemp (1);
      if (!(FLOW (res) = fopen_update_truncate (filename)))
        fatal_sys (filename);
      copystring (es, atat);
    }
  else
    {
      int c;
      register FILE *frew;
      register long e, oe;
      register bool amidline, oamidline;
      register char *optr;
      register struct fro *fin;

      e = 0;
      es->gap = 0;
      es->gapsize = es->lim;
      fin = FLOW (from);
      fro_trundling (false, fin);
      frew = FLOW (to);
      GETCHAR (c, fin);
      if (frew)
        afputc (c, frew);
      amidline = false;
      for (;;)
        {
          optr = fin->ptr;
          TEECHAR ();
          oamidline = amidline;
          oe = e;
          switch (c)
            {
            case '\n':
              ++e;
              amidline = false;
              break;
            case SDELIM:
              TEECHAR ();
              if (c != SDELIM)
                {
                  /* End of string.  */
                  es->lcount = e + amidline;
                  es->corr = 0;
                  return;
                }
              /* fall into */
            default:
              amidline = true;
              break;
            }
          if (!oamidline)
            insertline (es, oe, optr);
        }
    }
}

void
editstring (struct editstuff *es, struct atat const *script,
            struct delta const *delta)
/* Read an edit script from ‘FLOW (from)’ and apply it to the edit file.
   | -- ‘(STDIO_P (FLOW (from)))’ --
   | The result is written to ‘FLOW (res)’.
   | If ‘delta’, keyword expansion is performed simultaneously.
   | If running out of lines in ‘es->fedit’,
   | ‘es->fedit’ and ‘FLOW (res)’ are swapped.
   | ‘es->filename’ is the name of the file that goes with ‘es->fedit’.
   If ‘FLOW (to)’ is set, the edit script is also copied verbatim
   to ‘FLOW (to)’.  Assumes that all these files are open.
   ‘FLOW (result)’ is the name of the file that goes with ‘FLOW (res)’.
   Assumes the next input character from ‘FLOW (from)’ is the first
   character of the edit script.  */
{
  int ed;                               /* editor command */
  int c;
  register FILE *frew;
  register FILE *f = NULL;
  long line_lim = LONG_MAX;
  register struct fro *fe;
  register long i;
  register struct fro *fin;
  register long j = 0;
  struct diffcmd dc;

  es->script_lno = script->lno;
  es->lcount += es->corr;
  es->corr = 0;                         /* correct line number */
  frew = FLOW (to);
  fin = FLOW (from);
  GETCHAR (c, fin);
  if (frew)
    afputc (c, frew);
  initdiffcmd (&dc);
  while (0 <= (ed = getdiffcmd (fin, true, frew, &dc)))
    if (STDIO_P (fin)
        && line_lim <= dc.line1)
      EDIT_SCRIPT_OVERFLOW ();
    else if (!ed)
      {
        copylines (es, dc.line1 - 1, delta);
        /* Skip over unwanted lines.  */
        i = dc.nlines;
        es->corr -= i;
        es->lcount += i;
        if (!STDIO_P (fin))
          deletelines (es, es->lcount + es->corr, i);
        else
          {
            fe = es->fedit;
            do
              {
                /* Skip next line.  */
                do GETCHAR_OR (c, fe,
                               {
                                 if (i != 1)
                                   EDIT_SCRIPT_OVERFLOW ();
                                 line_lim = dc.dafter;
                                 goto done;
                               });
                while (c != '\n');
              done:
                ;
              }
            while (--i);
          }
        es->script_lno++;
      }
    else
      {
        /* Copy lines without deleting any.  */
        copylines (es, dc.line1, delta);
        i = dc.nlines;
        if (!STDIO_P (fin))
          j = es->lcount + es->corr;
        es->corr += i;
        if (STDIO_P (fin)
            && (f = FLOW (res),
                delta))
          {
            struct expctx ctx = EXPCTX (f, frew, fin, true, true);

            do
              {
                switch (expandline (&ctx))
                  {
                  case 0:
                  case 1:
                    if (i == 1)
                      return;
                    /* fall into */
                  case -1:
                    EDIT_SCRIPT_SHORT ();
                  }
              }
            while (--i);
            FINISH_EXPCTX (&ctx);
          }
        else
          {
            do
              {
                if (!STDIO_P (fin))
                  insertline (es, j++, fin->ptr);
                for (;;)
                  {
                    TEECHAR ();
                    if (c == SDELIM)
                      {
                        TEECHAR ();
                        if (c != SDELIM)
                          {
                            if (--i)
                              EDIT_SCRIPT_SHORT ();
                            return;
                          }
                      }
                    if (STDIO_P (fin))
                      aputc (c, f);
                    if (c == '\n')
                      break;
                  }
              }
            while (--i);
          }
        es->script_lno += 1 + dc.nlines;
      }
}

static int
naturalize (struct maybe *m, bool *symbolicp)
/* If ‘fn’ is a symbolic link, resolve it to the name that it points to.
   If unsuccessful, set errno and return -1.
   If it points to an existing file, return 1.
   Otherwise, set ‘errno’ to ‘ENOENT’ and return 0.
   On return ‘*symbolicp’ set means the filename was a symlink, and
   ‘fn’ points to the resolved filename (no change if not a symlink).  */
{
  int e;
  ssize_t r;
  int linkcount = _POSIX_SYMLOOP_MAX;
  struct cbuf *fn = &m->tentative;
  struct divvy *space = m->space;
  size_t len = SIZEABLE_FILENAME_LEN;
  char *chased = alloc (space, "chased", len);
  char const *orig = fn->string;

  while ((r = readlink (fn->string, chased, len)) != -1)
    if (len == (size_t) r)
      {
        len <<= 1;
        chased = alloc (space, "chased", len);
      }
    else if (!linkcount--)
      {
        errno = ELOOP;
        return -1;
      }
    else
      {
        /* Blech, ‘readlink’ does not NUL-terminate.  */
        chased[r] = '\0';
        if (ABSFNAME (chased))
          {
            fn->string = chased;
            fn->size = r;
          }
        else
          {
            char const *s = fn->string;

            accumulate_range (space, s, basefilename (s));
            accs (space, chased);
            fn->string = finish_string (space, &fn->size);
          }
      }
  e = errno;
  *symbolicp = fn->string != orig;
  errno = e;
  switch (e)
    {
    case EINVAL:
      return 1;
    case ENOENT:
      return 0;
    default:
      return -1;
    }
}

struct fro *
rcswriteopen (struct maybe *m)
/* Create the lock file corresponding to ‘m->tentative’.  Then try to
   open ‘m->tentative’ for reading and return its ‘fro*’ descriptor.
   Put its status into ‘*(m->status)’ too.
   ‘m->mustread’ is true if the file must already exist, too.
   If all goes well, discard any previously acquired locks,
   and set ‘REPO (fd_lock)’ to the file descriptor of the RCS lockfile.  */
{
  struct cbuf *RCSbuf = &m->tentative;
  register char *tp;
  register char const *sp, *repofn, *x;
  struct fro *f;
  size_t len;
  int e, exists, fdesc, fdescSafer, r;
  bool waslocked;
  struct stat statbuf;
  bool symbolicp = false;
  char *lfn;                            /* lock filename */
  struct sff *sff = BE (sff);

  waslocked = 0 <= REPO (fd_lock);
  exists = naturalize (m, &symbolicp);
  if (exists < (m->mustread | waslocked))
    /* There's an unusual problem with the RCS file; or the RCS file
       doesn't exist, and we must read or we already have a lock
       elsewhere.  */
    return NULL;

  repofn = RCSbuf->string;
  accs (m->space, RCSbuf->string);
  lfn = finish_string (m->space, &len);
  sp = basefilename (lfn);
  x = rcssuffix (repofn);
  if (!x && symbolicp)
    {
      PERR ("symbolic link to non RCS file `%s'", repofn);
      errno = EINVAL;
      return NULL;
    }
  if (*sp == *x)
    {
      PERR ("RCS filename `%s' incompatible with suffix `%s'", sp, x);
      errno = EINVAL;
      return NULL;
    }
  /* Create a lock filename that is a function of the RCS filename.  */
  if (*x)
    {
      /* The suffix is nonempty.  The lock filename is the first char
         of of the suffix, followed by the RCS filename with last char
         removed.  E.g.:
         | foo,v  -- RCS filename with suffix ,v
         | ,foo,  -- lock filename
      */
      tp = lfn + len;
      *tp-- = '\0';
      while (sp < tp)
        {
          *tp = tp[-1];
          tp--;
        }
      *tp = *x;
    }
  else
    {
      /* The suffix is empty.  The lock filename is the RCS filename
         with last char replaced by '_'.  */
      tp = lfn + len - 1;
      if ('_' == *tp)
        {
          PERR ("RCS filename `%s' ends with `%c'", repofn, *tp);
          errno = EINVAL;
          return NULL;
        }
      *tp = '_';
    }

  sff[waslocked].filename = intern (SINGLE, lfn, len);

  f = NULL;

  /* good news:
     ‘open (f, O_CREAT|O_EXCL|O_TRUNC|..., OPEN_CREAT_READONLY)’
     is atomic according to POSIX 1003.1-1990.

     bad news:
     NFS ignores O_EXCL and doesn't comply with POSIX 1003.1-1990.

     good news:
     ‘(O_TRUNC,OPEN_CREAT_READONLY)’ normally guarantees atomicity
     even with NFS.

     bad news:
     If you're root, ‘(O_TRUNC,OPEN_CREAT_READONLY)’ doesn't guarantee atomicity.

     good news:
     Root-over-the-wire NFS access is rare for security reasons.
     This bug has never been reported in practice with RCS.
     So we don't worry about this bug.

     An even rarer NFS bug can occur when clients retry requests.
     This can happen in the usual case of NFS over UDP.
     Suppose client A releases a lock by renaming ",f," to "f,v" at
     about the same time that client B obtains a lock by creating ",f,",
     and suppose A's first rename request is delayed, so A reissues it.
     The sequence of events might be:
     - A sends rename (",f,", "f,v").
     - B sends create (",f,").
     - A sends retry of rename (",f,", "f,v").
     - server receives, does, and acknowledges A's first ‘rename’.
     - A receives acknowledgment, and its RCS program exits.
     - server receives, does, and acknowledges B's ‘create’.
     - server receives, does, and acknowledges A's retry of ‘rename’.
     This not only wrongly deletes B's lock, it removes the RCS file!
     Most NFS implementations have idempotency caches that usually prevent
     this scenario, but such caches are finite and can be overrun.

     This problem afflicts not only RCS, which uses ‘open’ and ‘rename’
     to get and release locks; it also afflicts the traditional
     Unix method of using ‘link’ and ‘unlink’ to get and release locks,
     and the less traditional method of using ‘mkdir’ and ‘rmdir’.
     There is no easy workaround.

     Any new method based on ‘lockf’ seemingly would be incompatible with
     the old methods; besides, ‘lockf’ is notoriously buggy under NFS.
     Since this problem afflicts scads of Unix programs, but is so rare
     that nobody seems to be worried about it, we won't worry either.  */

#define create(f) open (f, OPEN_O_BINARY | OPEN_O_LOCK                \
                        | OPEN_O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, \
                        OPEN_CREAT_READONLY)

  ISR_DO (CATCHINTS);
  IGNOREINTS ();

  /* Create a lock file for an RCS file.  This should be atomic,
     i.e.  if two processes try it simultaneously, at most one
     should succeed.  */
  seteid ();
  fdesc = create (lfn);
  /* Do it now; ‘setrid’ might use stderr.  */
  fdescSafer = fd_safer (fdesc);
  e = errno;
  setrid ();

  if (0 <= fdesc)
    sff[SFFI_LOCKDIR].disposition = effective;

  if (PROB (fdescSafer))
    {
      if (e == EACCES && !PROB (stat (lfn, &statbuf)))
        /* The RCS file is busy.  */
        e = EEXIST;
    }
  else
    {
      e = ENOENT;
      if (exists)
        {
          f = fro_open (repofn, FOPEN_RB, m->status);
          e = errno;
          if (f && waslocked)
            {
              /* Discard the previous lock in favor of this one.  */
              ORCSclose ();
              seteid ();
              r = un_link (lockname);
              e = errno;
              setrid ();
              errno = e;
              if (PROB (r))
                fatal_sys (lockname);
              sff[SFFI_LOCKDIR].filename = lfn;
            }
        }
      REPO (fd_lock) = fdescSafer;
    }

  RESTOREINTS ();

  errno = e;
  return f;
}

int
chnamemod (FILE ** fromp, char const *from, char const *to,
           int set_mode, mode_t mode, time_t mtime)
/* Rename a file (with stream pointer ‘*fromp’) from ‘from’ to ‘to’.
   ‘from’ already exists.
   If ‘0 < set_mode’, change the mode to ‘mode’, before renaming if possible.
   If ‘mtime’ is not -1, change its mtime to ‘mtime’ before renaming.
   Close and clear ‘*fromp’ before renaming it.
   Unlink ‘to’ if it already exists.
   Return -1 on error (setting ‘errno’), 0 otherwise.   */
{
  mode_t mode_while_renaming = mode;
  int fchmod_set_mode = 0;

#if BAD_A_RENAME || bad_NFS_rename
  struct stat st;
  if (bad_NFS_rename || (BAD_A_RENAME && set_mode <= 0))
    {
      if (PROB (fstat (fileno (*fromp), &st)))
        return -1;
      if (BAD_A_RENAME && set_mode <= 0)
        mode = st.st_mode;
    }
#endif  /* BAD_A_RENAME || bad_NFS_rename */

#if BAD_A_RENAME
  /* There's a short window of inconsistency
     during which the lock file is writable.  */
  mode_while_renaming = mode | S_IWUSR;
  if (mode != mode_while_renaming)
    set_mode = 1;
#endif  /* BAD_A_RENAME */

  if (0 < set_mode
      && !PROB (change_mode (fileno (*fromp), mode_while_renaming)))
    fchmod_set_mode = set_mode;

  /* On some systems, we must close before chmod.  */
  Ozclose (fromp);
  if (fchmod_set_mode < set_mode && PROB (chmod (from, mode_while_renaming)))
    return -1;

  if (PROB (setmtime (from, mtime)))
    return -1;

#if BAD_B_RENAME
  /* There's a short window of inconsistency
     during which ‘to’ does not exist.  */
  if (PROB (un_link (to)) && errno != ENOENT)
    return -1;
#endif  /* BAD_B_RENAME */

  if (PROB (rename (from, to)) && !nfs_NOENT_p ())
    return -1;

#if bad_NFS_rename
  {
    /* Check whether the rename falsely reported success.
       A race condition can occur between the rename and the stat.  */
    struct stat tostat;

    if (PROB (stat (to, &tostat)))
      return -1;
    if (!SAME_INODE (st, tostat))
      {
        errno = EIO;
        return -1;
      }
  }
#endif  /* bad_NFS_rename */

#if BAD_A_RENAME
  if (0 < set_mode && PROB (chmod (to, mode)))
    return -1;
#endif

  return 0;
}

int
setmtime (char const *file, time_t mtime)
/* Set ‘file’ last modified time to ‘mtime’ (return utime(2) rv),
   but do nothing (return 0) if ‘mtime’ is -1.  */
{
  struct utimbuf amtime;

  if (mtime == -1)
    return 0;
  amtime.actime = BE (now);
  amtime.modtime = mtime;
  return utime (file, &amtime);
}

int
findlock (bool delete, struct delta **target)
/* Find the first lock held by caller and return a pointer
   to the locked delta; also, remove the lock if ‘delete’.
   If one lock, put it into ‘*target’.
   Return 0 for no locks, 1 for one, 2 for two or more.  */
{
  struct rcslock const *rl;
  struct link box, *found;
  char const *me = getcaller ();

  if (! (box.next = GROK (locks))
      || ! (found = lock_login_memq (&box, me)))
    return 0;
  if (lock_login_memq (found->next, me))
    {
      RERR ("multiple revisions locked by %s; please specify one", me);
      return 2;
    }
  rl = found->next->entry;
  *target = rl->delta;
  if (delete)
    lock_drop (&box, found);
  return 1;
}

int
addsymbol (char const *num, char const *name, bool rebind)
/* Associate with revision ‘num’ the new symbolic ‘name’.
   If ‘name’ already exists and ‘rebind’ is set, associate ‘name’
   with ‘num’; otherwise, print an error message and return false;
   Return -1 if unsuccessful, 0 if no change, 1 if change.  */
{
  struct link box, *tp;
  struct symdef *d;

#define MAKEDEF()                               \
  d = FALLOC (struct symdef);                   \
  d->meaningful = name;                         \
  d->underlying = num

  box.next = GROK (symbols);
  for (tp = &box; tp->next; tp = tp->next)
    {
      struct symdef const *dk = tp->next->entry;

      if (STR_SAME (name, dk->meaningful))
        {
          if (STR_SAME (dk->underlying, num))
            return 0;
          else if (rebind)
            {
              MAKEDEF ();
              tp->next = prepend (d, tp->next->next, SINGLE);
              goto ok;
            }
          else
            {
              RERR ("symbolic name %s already bound to %s",
                    name, dk->underlying);
              return -1;
            }
        }
    }
  MAKEDEF ();
  box.next = prepend (d, box.next, SINGLE);
 ok:
  GROK (symbols) = box.next;
  return 1;
}

bool
checkaccesslist (void)
/* Return true if caller is the superuser, the owner of the
   file, the access list is empty, or caller is on the access list.
   Otherwise, print an error message and return false.  */
{
  struct link *ls = GROK (access);

  if (!ls || stat_mine_p (&REPO (stat)) || caller_login_p ("root"))
    return true;

  for (; ls; ls = ls->next)
    if (caller_login_p (ls->entry))
      return true;

  RERR ("user %s not on the access list", getcaller ());
  return false;
}

int
dorewrite (bool lockflag, int changed)
/* Do nothing if not ‘lockflag’.
   Prepare to rewrite an RCS file if ‘changed’ is positive.
   Stop rewriting if ‘changed’ is zero, because there won't be any changes.
   Fail if ‘changed’ is negative.
   Return 0 on success, -1 on failure.  */
{
  int r = 0, e;

  if (lockflag)
    {
      if (changed)
        {
          FILE *frew;

          if (changed < 0)
            return -1;
          putadmin ();
          frew = FLOW (rewr);
          puttree (REPO (tip), frew);
          aprintf (frew, "\n\n%s\n", TINYKS (desc));
          FLOW (to) = frew;
        }
      else
        {
#if BAD_CREAT0
          int nr = !!FLOW (rewr), ne = 0;
#endif
          ORCSclose ();
          seteid ();
          IGNOREINTS ();
#if BAD_CREAT0
          if (nr)
            {
              nr = un_link (newRCSname);
              ne = errno;
              keepdirtemp (newRCSname);
            }
#endif
          r = un_link (lockname);
          e = errno;
          keepdirtemp (lockname);
          RESTOREINTS ();
          setrid ();
          if (PROB (r))
            syserror (e, lockname);
#if BAD_CREAT0
          if (nr != 0)                  /* avoid ‘PROB’; ‘nr’ may be >0 */
            {
              syserror (ne, newRCSname);
              r = -1;
            }
#endif
        }
      }
  return r;
}

int
donerewrite (int changed, time_t newRCStime)
/* Finish rewriting an RCS file if ‘changed’ is nonzero.
   Set its mode if ‘changed’ is positive.
   Set its modification time to ‘newRCStime’ unless it is -1.
   Return 0 on success, -1 on failure.  */
{
  int r = 0, e = 0;
#if BAD_CREAT0
  int lr, le;
#endif

  if (changed && !FLOW (erroneousp))
    {
      struct stat *repo_stat = &REPO (stat);
      char const *repo_filename = REPO (filename);
      struct fro *from = FLOW (from);
      FILE *frew = FLOW (rewr);

      if (from)
        {
          fro_spew (from, frew);
          fro_zclose (&FLOW (from));
        }
      if (1 < repo_stat->st_nlink)
        RWARN ("breaking hard link");
      aflush (frew);
      seteid ();
      IGNOREINTS ();
      r = chnamemod (&FLOW (rewr), newRCSname, repo_filename, changed,
                     repo_stat->st_mode & (mode_t) ~(S_IWUSR | S_IWGRP | S_IWOTH),
                     newRCStime);
      frew = FLOW (rewr);
      e = errno;
      keepdirtemp (newRCSname);
#if BAD_CREAT0
      lr = un_link (lockname);
      le = errno;
      keepdirtemp (lockname);
#endif
      RESTOREINTS ();
      setrid ();
      if (PROB (r))
        {
          syserror (e, repo_filename);
          PERR ("saved in %s", newRCSname);
        }
#if BAD_CREAT0
      if (PROB (lr))
        {
          syserror (le, lockname);
          r = -1;
        }
#endif
    }
  return r;
}

void
ORCSclose (void)
{
  int repo_fd_lock = REPO (fd_lock);

  if (0 <= repo_fd_lock)
    {
      if (PROB (close (repo_fd_lock)))
        fatal_sys (lockname);
      REPO (fd_lock) = -1;
    }
  Ozclose (&FLOW (rewr));
}

void
ORCSerror (void)
/* Like ‘ORCSclose’, except we are cleaning up after an interrupt or
   fatal error.  Do not report errors, since this may loop.  This is
   needed only because some brain-damaged hosts (e.g. OS/2) cannot
   unlink files that are open, and some nearly-POSIX hosts (e.g. NFS)
   work better if the files are closed first.  This isn't a completely
   reliable away to work around brain-damaged hosts, because of the gap
   between actual file opening and setting ‘FLOW (rewr)’ etc., but it's
   better than nothing.  */
{
  int repo_fd_lock = REPO (fd_lock);

  if (0 <= repo_fd_lock)
    close (repo_fd_lock);
  if (FLOW (rewr))
    /* Avoid ‘fclose’, since stdio may not be reentrant.  */
    close (fileno (FLOW (rewr)));
}

void
unexpected_EOF (void)
{
  RFATAL ("unexpected EOF in diff output");
}

void
initdiffcmd (register struct diffcmd *dc)
/* Initialize ‘*dc’ suitably for ‘getdiffcmd’.  */
{
  dc->adprev = 0;
  dc->dafter = 0;
}

static void
badDiffOutput (char const *buf)
{
  RFATAL ("bad diff output line: %s", buf);
}

static void
diffLineNumberTooLarge (char const *buf)
{
  RFATAL ("diff line number too large: %s", buf);
}

int
getdiffcmd (struct fro *finfile, bool delimiter, FILE *foutfile,
            struct diffcmd *dc)
/* Get an editing command output by "diff -n" from ‘finfile’.  The input
   is delimited by ‘SDELIM’ if ‘delimiter’ is set, EOF otherwise.  Copy
   a clean version of the command to ‘foutfile’ (if non-NULL).  Return 0
   for 'd', 1 for 'a', and -1 for EOF.  Store the command's line number
   and length into ‘dc->line1’ and ‘dc->nlines’.  Keep ‘dc->adprev’ and
   ‘dc->dafter’ up to date.  */
{
  int c;
  register FILE *fout;
  register char *p;
  register struct fro *fin;
  long line1, nlines, t;
  char buf[BUFSIZ];

  fin = finfile;
  fout = foutfile;
  GETCHAR_OR (c, fin,
              {
                if (delimiter)
                  unexpected_EOF ();
                return -1;
              });
  if (delimiter)
    {
      if (c == SDELIM)
        {
          GETCHAR (c, fin);
          if (c == SDELIM)
            {
              buf[0] = c;
              buf[1] = 0;
              badDiffOutput (buf);
            }
          if (fout)
            aprintf (fout, "%c%c", SDELIM, c);
          return -1;
        }
    }
  p = buf;
  do
    {
      if (buf + BUFSIZ - 2 <= p)
        {
          RFATAL ("diff output command line too long");
        }
      *p++ = c;
      GETCHAR_OR (c, fin, unexpected_EOF ());
    }
  while (c != '\n');
  *p = '\0';
  for (p = buf + 1; (c = *p++) == ' ';)
    continue;
  line1 = 0;
  while (isdigit (c))
    {
      if (LONG_MAX / 10 < line1
          || (t = line1 * 10, (line1 = t + (c - '0')) < t))
        diffLineNumberTooLarge (buf);
      c = *p++;
    }
  while (c == ' ')
    c = *p++;
  nlines = 0;
  while (isdigit (c))
    {
      if (LONG_MAX / 10 < nlines
          || (t = nlines * 10, (nlines = t + (c - '0')) < t))
        diffLineNumberTooLarge (buf);
      c = *p++;
    }
  if (c == '\r')
    c = *p++;
  if (c || !nlines)
    {
      badDiffOutput (buf);
    }
  if (line1 + nlines < line1)
    diffLineNumberTooLarge (buf);
  switch (buf[0])
    {
    case 'a':
      if (line1 < dc->adprev)
        {
          RFATAL ("backward insertion in diff output: %s", buf);
        }
      dc->adprev = line1 + 1;
      break;
    case 'd':
      if (line1 < dc->adprev || line1 < dc->dafter)
        {
          RFATAL ("backward deletion in diff output: %s", buf);
        }
      dc->adprev = line1;
      dc->dafter = line1 + nlines;
      break;
    default:
      badDiffOutput (buf);
    }
  if (fout)
    {
      aprintf (fout, "%s\n", buf);
    }
  dc->line1 = line1;
  dc->nlines = nlines;
  return buf[0] == 'a';
}

/* rcsedit.c ends here */
