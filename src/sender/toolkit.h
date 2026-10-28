/* ============================================================================
 * toolkit.h -- the boilerplate EVERY on-unit tool needs, in one place.
 *
 * WHY THIS EXISTS.  The tools in this directory are built `-nostdlib` against
 * the unit's own glibc 2.23: there is no crt and no crt1's argv setup, so each
 * one has to bootstrap itself.  Before this header, 22 of the 26 tools carried
 * an identical copy of that bootstrap and 25 carried their own integer ->
 * ASCII converter under four different names.  It was not twenty design
 * decisions; it was one recipe retyped.
 *
 * WHAT IS (AND IS NOT) HERE.  Only things that are genuinely the same in every
 * tool: the argv bootstrap, write()-based printing, and atoi.  Nothing
 * tool-specific belongs here -- a helper that needs to know what a "frame" is
 * does not live in this file.
 *
 * WHY write() AND NOT printf().  These tools log to a pipe and are frequently
 * killed mid-run (the decoder is replaced per session, the presenter is
 * signalled).  stdio buffers, so a killed tool loses its last few hundred bytes
 * -- which is exactly the log you wanted.  write() is unbuffered on purpose.
 *
 * LIBC USE.  strlen/atoi/memset/memcpy come from the unit's libc (these tools
 * link libc.so.6).  Hand-rolling them added nothing but bugs and bulk.
 *
 * USAGE
 *   #include "toolkit.h"
 *   int main(int argc, char **argv);
 *   void _start(void){ TOOL_ARGS(); }
 * ==========================================================================*/
#ifndef MIB3_TOOLKIT_H
#define MIB3_TOOLKIT_H

/* --- libc, declared by hand (no headers in a -nostdlib build) ------------- */
extern long  write(int, const void *, unsigned long);
extern long  read(int, void *, unsigned long);
extern int   open(const char *, int, ...);
extern int   close(int);
extern void  _exit(int);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern long  lseek(int, long, int);
extern unsigned long strlen(const char *);
extern int   atoi(const char *);
extern char *getenv(const char *);

#define TK_UNUSED __attribute__((unused))

/* --- printing: one implementation, stdout and stderr --------------------- */
TK_UNUSED static void Pf(int fd, const char *s){
  if (s) write(fd, s, strlen(s));
}
TK_UNUSED static void Pnf(int fd, long v){
  char b[24]; int i = 0;
  if (v < 0){ write(fd, "-", 1); v = -v; }
  if (!v){ write(fd, "0", 1); return; }
  while (v){ b[i++] = (char)('0' + v % 10); v /= 10; }
  while (i) write(fd, &b[--i], 1);
}
TK_UNUSED static void Pxf(int fd, unsigned long v){
  const char *h = "0123456789abcdef"; char b[20]; int i = 0;
  if (!v){ write(fd, "0", 1); return; }
  while (v){ b[i++] = h[v & 0xf]; v >>= 4; }
  while (i) write(fd, &b[--i], 1);
}
TK_UNUSED static void P (const char *s){ Pf(1, s); }   /* stdout */
TK_UNUSED static void Pe(const char *s){ Pf(2, s); }   /* stderr */
TK_UNUSED static void Pn(long v)        { Pnf(1, v); }
TK_UNUSED static void Pw(long v)        { Pnf(2, v); }
TK_UNUSED static void Px(unsigned long v){ Pxf(1, v); }
TK_UNUSED static void Pxw(unsigned long v){ Pxf(2, v); }

/* atoi with the tools' historical semantics: no sign, no whitespace skip.
 * (Kept explicit rather than silently changing how a port number parses.) */
TK_UNUSED static int A(const char *s){
  int v = 0;
  while (s && *s >= '0' && *s <= '9'){ v = v * 10 + (*s - '0'); s++; }
  return v;
}

/* --- the argv bootstrap --------------------------------------------------
 * -nostdlib means no crt1, so nothing has parsed argv for us.  Reading
 * /proc/self/cmdline avoids writing a start.S that pulls argc/argv off the
 * stack (which would need per-ABI assembly).  512 bytes covers any real
 * invocation; 15 args is more than any tool takes.
 * ------------------------------------------------------------------------ */
#define TOOL_ARGS()                                                          \
  static char tk_cb[1024]; static char *tk_av[16];                            \
  int tk_n = 0, tk_ac = 0, tk_i = 0;                                         \
  int tk_fd = open("/proc/self/cmdline", 0);                                 \
  if (tk_fd >= 0){                                                           \
    long tk_r = read(tk_fd, tk_cb, sizeof(tk_cb) - 1);                       \
    if (tk_r > 0) tk_n = (int)tk_r;                                          \
    close(tk_fd);                                                            \
  }                                                                          \
  tk_cb[tk_n] = 0;                                                           \
  while (tk_i < tk_n && tk_ac < 15){                                         \
    tk_av[tk_ac++] = &tk_cb[tk_i];                                           \
    while (tk_i < tk_n && tk_cb[tk_i]) tk_i++;                               \
    tk_i++;                                                                  \
  }                                                                          \
  tk_av[tk_ac] = 0;                                                          \
  _exit(main(tk_ac, tk_av));

#endif /* MIB3_TOOLKIT_H */
