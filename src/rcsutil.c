/* RCS utility functions

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

#include "rcsbase.h"

/*
 * list of blocks allocated with ftestalloc()
 * These blocks can be freed by ffree when we're done with the current file.
 * We could put the free block inside struct alloclist, rather than a pointer
 * to the free block, but that would be less portable.
 */
struct alloclist
{
  void *alloc;
  struct alloclist *nextalloc;
};
static struct alloclist *alloced;

static void *okalloc (void *);
static void *
okalloc (void * p)
{
  if (!p)
    faterror ("out of memory");
  return p;
}

void *
testalloc (size_t size)
/* Allocate a block, testing that the allocation succeeded.  */
{
  return okalloc (malloc (size));
}

void *
testrealloc (void *ptr, size_t size)
/* Reallocate a block, testing that the allocation succeeded.  */
{
  return okalloc (realloc (ptr, size));
}

void *
fremember (void *ptr)
/* Remember PTR in 'alloced' so that it can be freed later.  Yield PTR.  */
{
  register struct alloclist *q = talloc (struct alloclist);
  q->nextalloc = alloced;
  alloced = q;
  return q->alloc = ptr;
}

void *
ftestalloc (size_t size)
/* Allocate a block, putting it in 'alloced' so it can be freed later. */
{
  return fremember (testalloc (size));
}

void
ffree (void)
/* Free all blocks allocated with ftestalloc().  */
{
  register struct alloclist *p, *q;
  for (p = alloced; p; p = q)
    {
      q = p->nextalloc;
      tfree (p->alloc);
      tfree (p);
    }
  alloced = 0;
}

void
ffree1 (register char const *f)
/* Free the block f, which was allocated by ftestalloc.  */
{
  register struct alloclist *p, **a = &alloced;

  while ((p = *a)->alloc != f)
    a = &p->nextalloc;
  *a = p->nextalloc;
  tfree (p->alloc);
  tfree (p);
}

char *
str_save (char const *s)
/* Save s in permanently allocated storage. */
{
  return strcpy (tnalloc (char, strlen (s) + 1), s);
}

char *
fstr_save (char const *s)
/* Save s in storage that will be deallocated when we're done with this file. */
{
  return strcpy (ftnalloc (char, strlen (s) + 1), s);
}

char *
cgetenv (char const *name)
/* Like getenv(), but yield a copy; getenv() can overwrite old results. */
{
  register char *p;

  return (p = getenv (name)) ? str_save (p) : p;
}

char const *
getusername (int suspicious)
/* Get the caller's login name.  Trust only getwpuid if SUSPICIOUS.  */
{
  static char *name;

  if (!name)
    {
      if (
           /* Prefer getenv() unless suspicious; it's much faster.  */
#		    if getlogin_is_secure
           (suspicious
            || (!(name = cgetenv ("LOGNAME"))
                && !(name = cgetenv ("USER")))) && !(name = getlogin ())
#		    else
           suspicious
           || (!(name = cgetenv ("LOGNAME"))
               && !(name = cgetenv ("USER")) && !(name = getlogin ()))
#		    endif
        )
        {
#if defined HAVE_GETUID && defined HAVE_GETPWUID
          struct passwd const *pw = getpwuid (ruid ());
          if (!pw)
            faterror ("no password entry for userid %lu",
                      (unsigned long) ruid ());
          name = pw->pw_name;
#else
#if has_setuid
          faterror ("setuid not supported");
#else
          faterror ("Who are you?  Please setenv LOGNAME.");
#endif
#endif
        }
      checksid (name);
    }
  return name;
}

#if defined HAVE_SIGNAL_H

/*
 *	 Signal handling
 *
 * Standard C places too many restrictions on signal handlers.
 * We obey as many of them as we can.
 * Posix places fewer restrictions, and we are Posix-compatible here.
 */

static sig_atomic_t volatile heldsignal, holdlevel;
#ifdef SA_SIGINFO
static int unsupported_SA_SIGINFO;
static siginfo_t bufsiginfo;
static siginfo_t *volatile heldsiginfo;
#endif

#if has_NFS && has_mmap && large_memory && mmap_signal
static char const *accessName;

void
readAccessFilenameBuffer (char const *filename, unsigned char const *p)
{
  unsigned char volatile t;
  accessName = filename;
  t = *p;
  accessName = 0;
}
#else
#   define accessName ((char const *) 0)
#endif

#if !has_psignal

# define psignal my_psignal
static void my_psignal (int, char const *);
static void
my_psignal (int sig, char const *s)
{
  char const *sname = "Unknown signal";
#	if has_sys_siglist && defined(NSIG)
  if ((unsigned) sig < NSIG)
    sname = sys_siglist[sig];
#	else
  switch (sig)
    {
#	       ifdef SIGHUP
    case SIGHUP:
      sname = "Hangup";
      break;
#	       endif
#	       ifdef SIGINT
    case SIGINT:
      sname = "Interrupt";
      break;
#	       endif
#	       ifdef SIGPIPE
    case SIGPIPE:
      sname = "Broken pipe";
      break;
#	       endif
#	       ifdef SIGQUIT
    case SIGQUIT:
      sname = "Quit";
      break;
#	       endif
#	       ifdef SIGTERM
    case SIGTERM:
      sname = "Terminated";
      break;
#	       endif
#	       ifdef SIGXCPU
    case SIGXCPU:
      sname = "Cputime limit exceeded";
      break;
#	       endif
#	       ifdef SIGXFSZ
    case SIGXFSZ:
      sname = "Filesize limit exceeded";
      break;
#	       endif
#	      if has_mmap && large_memory
#	       if defined(SIGBUS) && mmap_signal==SIGBUS
    case SIGBUS:
      sname = "Bus error";
      break;
#	       endif
#	       if defined(SIGSEGV) && mmap_signal==SIGSEGV
    case SIGSEGV:
      sname = "Segmentation fault";
      break;
#	       endif
#	      endif
    }
#	endif

  /* Avoid calling sprintf etc., in case they're not reentrant.  */
  {
    char const *p;
    char buf[BUFSIZ], *b = buf;
    for (p = s; *p; *b++ = *p++)
      continue;
    *b++ = ':';
    *b++ = ' ';
    for (p = sname; *p; *b++ = *p++)
      continue;
    *b++ = '\n';
    write (STDERR_FILENO, buf, b - buf);
  }
}
#endif

static void catchsig (int);
#ifdef SA_SIGINFO
static void catchsigaction (int, siginfo_t *, void *);
#endif

static void
catchsig (int s)
#ifdef SA_SIGINFO
{
  catchsigaction (s, (siginfo_t *) 0, (void *) 0);
}

static void
catchsigaction (int s, siginfo_t *i, void *c)
#endif
{
#   if sig_zaps_handler
  /* If a signal arrives before we reset the handler, we lose. */
  signal (s, SIG_IGN);
#   endif

#   ifdef SA_SIGINFO
  if (!unsupported_SA_SIGINFO)
    i = 0;
#   endif

  if (holdlevel)
    {
      heldsignal = s;
#	ifdef SA_SIGINFO
      if (i)
        {
          bufsiginfo = *i;
          heldsiginfo = &bufsiginfo;
        }
#	endif
      return;
    }

  ignoreints ();
  setrid ();
  if (!quietflag)
    {
      /* Avoid calling sprintf etc., in case they're not reentrant.  */
      char const *p;
      char buf[BUFSIZ], *b = buf;

      if (!(
#		if has_mmap && large_memory && mmap_signal
             /* Check whether this signal was planned.  */
             s == mmap_signal && accessName
#		else
             0
#		endif
          ))
        {
          char const *nRCS = "\nRCS";
#	    if defined(SA_SIGINFO) && has_si_errno && has_mmap && large_memory && mmap_signal
          if (s == mmap_signal && i && i->si_errno)
            {
              errno = i->si_errno;
              perror (nRCS++);
            }
#	    endif
#	    if defined(SA_SIGINFO) && has_psiginfo
          if (i)
            psiginfo (i, nRCS);
          else
            psignal (s, nRCS);
#	    else
          psignal (s, nRCS);
#	    endif
        }

      for (p = "RCS: "; *p; *b++ = *p++)
        continue;
#	if has_mmap && large_memory && mmap_signal
      if (s == mmap_signal)
        {
          p = accessName;
          if (!p)
            p = "Was a file changed by some other process?  ";
          else
            {
              char const *p1;
              for (p1 = p; *p1; p1++)
                continue;
              write (STDERR_FILENO, buf, b - buf);
              write (STDERR_FILENO, p, p1 - p);
              b = buf;
              p = ": Permission denied.  ";
            }
          while (*p)
            *b++ = *p++;
        }
#	endif
      for (p = "Cleaning up.\n"; *p; *b++ = *p++)
        continue;
      write (STDERR_FILENO, buf, b - buf);
    }
  exiterr ();
}

void
ignoreints (void)
{
  ++holdlevel;
}

void
restoreints (void)
{
  if (!--holdlevel && heldsignal)
#	    ifdef SA_SIGINFO
    catchsigaction (heldsignal, heldsiginfo, (void *) 0);
#	    else
    catchsig (heldsignal);
#	    endif
}

static void setup_catchsig (int const *, int);

#if has_sigaction

static void check_sig (int);
static void
check_sig (int r)
{
  if (r != 0)
    efaterror ("signal handling");
}

static void
setup_catchsig (int const *sig, int sigs)
{
  register int i, j;
  struct sigaction act;

  for (i = sigs; 0 <= --i;)
    {
      check_sig (sigaction (sig[i], (struct sigaction *) 0, &act));
      if (act.sa_handler != SIG_IGN)
        {
          act.sa_handler = catchsig;
#		ifdef SA_SIGINFO
          if (!unsupported_SA_SIGINFO)
            {
#			if has_sa_sigaction
              act.sa_sigaction = catchsigaction;
#			else
              act.sa_handler = catchsigaction;
#			endif
              act.sa_flags |= SA_SIGINFO;
            }
#		endif
          for (j = sigs; 0 <= --j;)
            check_sig (sigaddset (&act.sa_mask, sig[j]));
          if (sigaction (sig[i], &act, (struct sigaction *) 0) != 0)
            {
#		    if defined(SA_SIGINFO) && defined(ENOTSUP)
              if (errno == ENOTSUP && !unsupported_SA_SIGINFO)
                {
                  /* Turn off use of SA_SIGINFO and try again.  */
                  unsupported_SA_SIGINFO = 1;
                  i++;
                  continue;
                }
#		    endif
              check_sig (-1);
            }
        }
    }
}

#else
#if has_sigblock

static void
setup_catchsig (int const *sig, int sigs)
{
  register int i;
  int mask;

  mask = 0;
  for (i = sigs; 0 <= --i;)
    mask |= sigmask (sig[i]);
  mask = sigblock (mask);
  for (i = sigs; 0 <= --i;)
    if (signal (sig[i], catchsig) == SIG_IGN &&
        signal (sig[i], SIG_IGN) != catchsig)
      faterror ("signal catcher failure");
  sigsetmask (mask);
}

#else

static void
setup_catchsig (int const *sig, int sigs)
{
  register i;

  for (i = sigs; 0 <= --i;)
    if (signal (sig[i], SIG_IGN) != SIG_IGN &&
        signal (sig[i], catchsig) != SIG_IGN)
      faterror ("signal catcher failure");
}

#endif
#endif

static int const regsigs[] = {
# ifdef SIGHUP
  SIGHUP,
# endif
# ifdef SIGINT
  SIGINT,
# endif
# ifdef SIGPIPE
  SIGPIPE,
# endif
# ifdef SIGQUIT
  SIGQUIT,
# endif
# ifdef SIGTERM
  SIGTERM,
# endif
# ifdef SIGXCPU
  SIGXCPU,
# endif
# ifdef SIGXFSZ
  SIGXFSZ,
# endif
};

void
catchints (void)
{
  static int catching_ints;
  if (!catching_ints)
    {
      catching_ints = true;
      setup_catchsig (regsigs, (int) (sizeof (regsigs) / sizeof (*regsigs)));
    }
}

#if has_mmap && large_memory && mmap_signal

    /*
     * If you mmap an NFS file, and someone on another client removes the last
     * link to that file, and you later reference an uncached part of that file,
     * you'll get a SIGBUS or SIGSEGV (depending on the operating system).
     * Catch the signal and report the problem to the user.
     * Unfortunately, there's no portable way to differentiate between this
     * problem and actual bugs in the program.
     * This NFS problem is rare, thank goodness.
     *
     * This can also occur if someone truncates the file, even without NFS.
     */

static int const mmapsigs[] = { mmap_signal };

void
catchmmapints (void)
{
  static int catching_mmap_ints;
  if (!catching_mmap_ints)
    {
      catching_mmap_ints = true;
      setup_catchsig (mmapsigs,
                      (int) (sizeof (mmapsigs) / sizeof (*mmapsigs)));
    }
}
#endif

#endif /* defined HAVE_SIGNAL_H */

void
fastcopy (register RILE *inf, FILE *outf)
/* Function: copies the remainder of file inf to outf.
 */
{
#if large_memory
#	if maps_memory
  awrite ((char const *) inf->ptr, (size_t) (inf->lim - inf->ptr), outf);
  inf->ptr = inf->lim;
#	else
  for (;;)
    {
      awrite ((char const *) inf->ptr, (size_t) (inf->readlim - inf->ptr),
              outf);
      inf->ptr = inf->readlim;
      if (inf->ptr == inf->lim)
        break;
      Igetmore (inf);
    }
#	endif
#else
  char buf[BUFSIZ * 8];
  register size_t rcount;

  /*now read the rest of the file in blocks */
  while (!feof (inf))
    {
      if (!(rcount = fread (buf, sizeof (*buf), sizeof (buf), inf)))
        {
          testIerror (inf);
          return;
        }
      awrite (buf, (size_t) rcount, outf);
    }
#endif
}

#ifndef SSIZE_MAX
 /* This does not work in #ifs, but it's good enough for us.  */
 /* Underestimating SSIZE_MAX may slow us down, but it won't break us.  */
#	define SSIZE_MAX ((unsigned)-1 >> 1)
#endif

void
awrite (char const *buf, size_t chars, FILE *f)
{
  /* Posix 1003.1-1990 ssize_t hack */
  while (SSIZE_MAX < chars)
    {
      if (fwrite (buf, sizeof (*buf), SSIZE_MAX, f) != SSIZE_MAX)
        Oerror ();
      buf += SSIZE_MAX;
      chars -= SSIZE_MAX;
    }

  if (fwrite (buf, sizeof (*buf), chars, f) != chars)
    Oerror ();
}

/* dup a file descriptor; the result must not be stdin, stdout, or stderr.  */
static int dupSafer (int);
static int
dupSafer (int fd)
{
#	ifdef F_DUPFD
  return fcntl (fd, F_DUPFD, STDERR_FILENO + 1);
#	else
  int e, f, i, used = 0;
  while (STDIN_FILENO <= (f = dup (fd)) && f <= STDERR_FILENO)
    used |= 1 << f;
  e = errno;
  for (i = STDIN_FILENO; i <= STDERR_FILENO; i++)
    if (used & (1 << i))
      close (i);
  errno = e;
  return f;
#	endif
}

/* Renumber a file descriptor so that it's not stdin, stdout, or stderr.  */
int
fdSafer (int fd)
{
  if (STDIN_FILENO <= fd && fd <= STDERR_FILENO)
    {
      int f = dupSafer (fd);
      int e = errno;
      close (fd);
      errno = e;
      fd = f;
    }
  return fd;
}

/* Like fopen, except the result is never stdin, stdout, or stderr.  */
FILE *
fopenSafer (char const *filename, char const *type)
{
  FILE *stream = fopen (filename, type);
  if (stream)
    {
      int fd = fileno (stream);
      if (STDIN_FILENO <= fd && fd <= STDERR_FILENO)
        {
          int f = dupSafer (fd);
          if (f < 0)
            {
              int e = errno;
              fclose (stream);
              errno = e;
              return 0;
            }
          if (fclose (stream) != 0)
            {
              int e = errno;
              close (f);
              errno = e;
              return 0;
            }
          stream = fdopen (f, type);
        }
    }
  return stream;
}

#ifdef F_DUPFD
#	undef dup
#	define dup(fd) fcntl(fd, F_DUPFD, 0)
#endif

#if has_fork || has_spawn

static int movefd (int, int);
static int
movefd (int old, int new)
{
  if (old < 0 || old == new)
    return old;
#	ifdef F_DUPFD
  new = fcntl (old, F_DUPFD, new);
#	else
  new = dup2 (old, new);
#	endif
  return close (old) == 0 ? new : -1;
}

static int fdreopen (int, char const *, int);
static int
fdreopen (int fd, char const *file, int flags)
{
  int newfd;
  close (fd);
  newfd =
#if !open_can_creat
    flags & O_CREAT ? creat (file, S_IRUSR | S_IWUSR) :
#endif
    open (file, flags, S_IRUSR | S_IWUSR);
  return movefd (newfd, fd);
}

#if has_spawn
static void redirect (int, int);
static void
redirect (int old, int new)
/*
* Move file descriptor OLD to NEW.
* If OLD is -1, do nothing.
* If OLD is -2, just close NEW.
*/
{
  if ((old != -1 && close (new) != 0) || (0 <= old && movefd (old, new) < 0))
    efaterror ("spawn I/O redirection");
}
#endif

#else /* !has_fork && !has_spawn */

static void bufargcat (struct buf *, int, char const *);
static void
bufargcat (register struct buf *b, int c, register char const *s)
/* Append to B a copy of C, plus a quoted copy of S.  */
{
  register char *p;
  register char const *t;
  size_t bl, sl;

  for (t = s, sl = 0; *t;)
    sl += 3 * (*t++ == '\'') + 1;
  bl = strlen (b->string);
  bufrealloc (b, bl + sl + 4);
  p = b->string + bl;
  *p++ = c;
  *p++ = '\'';
  while (*s)
    {
      if (*s == '\'')
        {
          *p++ = '\'';
          *p++ = '\\';
          *p++ = '\'';
        }
      *p++ = *s++;
    }
  *p++ = '\'';
  *p = 0;
}

#endif

#if !has_spawn && has_fork
/*
* Output the string S to stderr, without touching any I/O buffers.
* This is useful if you are a child process, whose buffers are usually wrong.
* Exit immediately if the write does not completely succeed.
*/
static void write_stderr (char const *);
static void
write_stderr (char const *s)
{
  size_t slen = strlen (s);
  if (write (STDERR_FILENO, s, slen) != slen)
    _exit (EXIT_TROUBLE);
}
#endif

/*
* Run a command.
* infd, if not -1, is the input file descriptor.
* outname, if nonzero, is the name of the output file.
* args[1..] form the command to be run; args[0] might be modified.
*/
int
runv (int infd, char const *outname, char const **args)
{
  int wstatus;

#if bad_wait_if_SIGCHLD_ignored
  static int fixed_SIGCHLD;
  if (!fixed_SIGCHLD)
    {
      fixed_SIGCHLD = true;
#	    ifndef SIGCHLD
#	    define SIGCHLD SIGCLD
#	    endif
      signal (SIGCHLD, SIG_DFL);
    }
#endif

  oflush ();
  eflush ();
  {
#if has_spawn
    int in, out;
    char const *file;

    in = -1;
    if (infd != -1 && infd != STDIN_FILENO)
      {
        if ((in = dup (STDIN_FILENO)) < 0)
          {
            if (errno != EBADF)
              efaterror ("spawn input setup");
            in = -2;
          }
        else
          {
#		ifdef F_DUPFD
            if (close (STDIN_FILENO) != 0)
              efaterror ("spawn input close");
#		endif
          }
        if (
#		ifdef F_DUPFD
             fcntl (infd, F_DUPFD, STDIN_FILENO) != STDIN_FILENO
#		else
             dup2 (infd, STDIN_FILENO) != STDIN_FILENO
#		endif
          )
          efaterror ("spawn input redirection");
      }

    out = -1;
    if (outname)
      {
        if ((out = dup (STDOUT_FILENO)) < 0)
          {
            if (errno != EBADF)
              efaterror ("spawn output setup");
            out = -2;
          }
        if (fdreopen (STDOUT_FILENO, outname,
                      O_CREAT | O_TRUNC | O_WRONLY) < 0)
          efaterror (outname);
      }

    wstatus = spawn_RCS (0, args[1], (char **) (args + 1));
#	ifdef RCS_SHELL
    if (wstatus == -1 && errno == ENOEXEC)
      {
        args[0] = RCS_SHELL;
        wstatus = spawnv (0, args[0], (char **) args);
      }
#	endif
    redirect (in, STDIN_FILENO);
    redirect (out, STDOUT_FILENO);
#else
#if has_fork
    pid_t pid;
    if (!(pid = vfork ()))
      {
        char const *notfound;
        if (infd != -1 && infd != STDIN_FILENO && (
#		    ifdef F_DUPFD
                                                    (close (STDIN_FILENO),
                                                     fcntl (infd, F_DUPFD,
                                                            STDIN_FILENO) !=
                                                     STDIN_FILENO)
#		    else
                                                    dup2 (infd,
                                                          STDIN_FILENO) !=
                                                    STDIN_FILENO
#		    endif
            ))
          {
            /* Avoid perror since it may misuse buffers.  */
            write_stderr (args[1]);
            write_stderr (": I/O redirection failed\n");
            _exit (EXIT_TROUBLE);
          }

        if (outname)
          if (fdreopen (STDOUT_FILENO, outname,
                        O_CREAT | O_TRUNC | O_WRONLY) < 0)
            {
              /* Avoid perror since it may misuse buffers.  */
              write_stderr (args[1]);
              write_stderr (": ");
              write_stderr (outname);
              write_stderr (": cannot create\n");
              _exit (EXIT_TROUBLE);
            }
        exec_RCS (args[1], (char **) (args + 1));
        notfound = args[1];
#		ifdef RCS_SHELL
        if (errno == ENOEXEC)
          {
            args[0] = notfound = RCS_SHELL;
            execv (args[0], (char **) args);
          }
#		endif

        /* Avoid perror since it may misuse buffers.  */
        write_stderr (notfound);
        write_stderr (": not found\n");
        _exit (EXIT_TROUBLE);
      }
    if (pid < 0)
      efaterror ("fork");
#	if has_waitpid
    if (waitpid (pid, &wstatus, 0) < 0)
      efaterror ("waitpid");
#	else
    {
      pid_t w;
      do
        {
          if ((w = wait (&wstatus)) < 0)
            efaterror ("wait");
        }
      while (w != pid);
    }
#	endif
#else
    static struct buf b;
    char const *p;

    /* Use system().  On many hosts system() discards signals.  Yuck!  */
    p = args + 1;
    bufscpy (&b, *p);
    while (*++p)
      bufargcat (&b, ' ', *p);
    if (infd != -1 && infd != STDIN_FILENO)
      {
        char redirection[32];
        sprintf (redirection, "<&%d", infd);
        bufscat (&b, redirection);
      }
    if (outname)
      bufargcat (&b, '>', outname);
    wstatus = system (b.string);
#endif
#endif
  }
  if (!WIFEXITED (wstatus))
    {
      if (WIFSIGNALED (wstatus))
        {
          psignal (WTERMSIG (wstatus), args[1]);
          fatcleanup (1);
        }
      faterror ("%s failed for unknown reason", args[1]);
    }
  return WEXITSTATUS (wstatus);
}

#define CARGSMAX 20
/*
* Run a command.
* infd, if not -1, is the input file descriptor.
* outname, if nonzero, is the name of the output file.
* The remaining arguments specify the command and its arguments.
*/
int
run (int infd, char const *outname, ...)
{
  va_list ap;
  char const *rgargs[CARGSMAX];
  register int i;
  va_start (ap, outname);
  for (i = 1; (rgargs[i++] = va_arg (ap, char const *));)
    if (CARGSMAX <= i)
      faterror ("too many command arguments");
  va_end (ap);
  return runv (infd, outname, rgargs);
}

int RCSversion;

void
setRCSversion (char const *str)
{
  static int oldversion;

  register char const *s = str + 2;

  if (*s)
    {
      int v = VERSION_DEFAULT;

      if (oldversion)
        redefined ('V');
      oldversion = true;
      v = 0;
      while (isdigit (*s))
        v = 10 * v + *s++ - '0';
      if (*s)
        error ("%s isn't a number", str);
      else if (v < VERSION_min || VERSION_max < v)
        error ("%s out of range %d..%d", str, VERSION_min, VERSION_max);

      RCSversion = VERSION (v);
    }
  else
    {
      printf ("%s%s", cmdid, COMMAND_VERSION);
      exit (0);
    }
}

int
getRCSINIT (int argc, char **argv, char ***newargv)
{
  register char *p, *q, **pp;
  size_t n;

  if (!(q = cgetenv ("RCSINIT")))
    *newargv = argv;
  else
    {
      n = argc + 2;
      /*
       * Count spaces in RCSINIT to allocate a new arg vector.
       * This is an upper bound, but it's OK even if too large.
       */
      for (p = q;;)
        {
          switch (*p++)
            {
            default:
              continue;

            case ' ':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
            case '\v':
              n++;
              continue;

            case '\0':
              break;
            }
          break;
        }
      *newargv = pp = tnalloc (char *, n);
      *pp++ = *argv++;          /* copy program name */
      for (p = q;;)
        {
          for (;;)
            {
              switch (*q)
                {
                case '\0':
                  goto copyrest;

                case ' ':
                case '\b':
                case '\f':
                case '\n':
                case '\r':
                case '\t':
                case '\v':
                  q++;
                  continue;
                }
              break;
            }
          *pp++ = p;
          ++argc;
          for (;;)
            {
              switch ((*p++ = *q++))
                {
                case '\0':
                  goto copyrest;

                case '\\':
                  if (!*q)
                    goto copyrest;
                  p[-1] = *q++;
                  continue;

                default:
                  continue;

                case ' ':
                case '\b':
                case '\f':
                case '\n':
                case '\r':
                case '\t':
                case '\v':
                  break;
                }
              break;
            }
          p[-1] = '\0';
        }
    copyrest:
      while ((*pp++ = *argv++))
        continue;
    }
  return argc;
}

#define cacheid(E) static uid_t i; static int s; if (!s){ s=1; i=(E); } return i

#if defined HAVE_GETUID
uid_t
ruid (void)
{
  cacheid (getuid ());
}
#endif
#if has_setuid
uid_t
euid (void)
{
  cacheid (geteuid ());
}
#endif

#if has_setuid

/*
 * Setuid execution really works only with Posix 1003.1a Draft 5 seteuid(),
 * because it lets us switch back and forth between arbitrary users.
 * If seteuid() doesn't work, we fall back on setuid(),
 * which works if saved setuid is supported,
 * unless the real or effective user is root.
 * This area is such a mess that we always check switches at runtime.
 */

static void
set_uid_to (uid_t u)
/* Become user u.  */
{
  static int looping;

  if (euid () == ruid ())
    return;
#if (has_fork||has_spawn) && DIFF_ABSOLUTE
#	if has_setreuid
  if (setreuid (u == euid ()? ruid () : euid (), u) != 0)
    efaterror ("setuid");
#	else
  if (seteuid (u) != 0)
    efaterror ("setuid");
#	endif
#endif
  if (geteuid () != u)
    {
      if (looping)
        return;
      looping = true;
      faterror ("root setuid not supported" + (u ? 5 : 0));
    }
}

static int stick_with_euid;

void
/* Ignore all calls to seteid() and setrid().  */
nosetid (void)
{
  stick_with_euid = true;
}

void
seteid (void)
/* Become effective user.  */
{
  if (!stick_with_euid)
    set_uid_to (euid ());
}

void
setrid (void)
/* Become real user.  */
{
  if (!stick_with_euid)
    set_uid_to (ruid ());
}
#endif

time_t
now (void)
{
  static time_t t;
  if (!t && time (&t) == -1)
    efaterror ("time");
  return t;
}
