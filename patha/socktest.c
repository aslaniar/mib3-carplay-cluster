/* socktest.c -- reproduce the SHIM's helper connect exactly, as the CARPLAY
 * uid/gid (986:44) with the carplay::app exec label, and print the errno.
 *
 * Usage: socktest <uid> <gid> [path]
 * Build: shim/tools/build.sh (aarch64, libc-only).
 *
 * Kept because the failure it diagnoses (an AF_UNIX connect that fails only
 * under carplay's credentials/label) cannot be reproduced any other way.
 */
#include "toolkit.h"

extern int   socket(int, int, int);
extern int   connect(int, const void *, unsigned int);
extern int   setgid(unsigned);
extern int   setuid(unsigned);
extern int  *__errno_location(void);
#define ERRNO (*__errno_location())

int main(int argc, char **argv);
void _start(void){ TOOL_ARGS(); }

int main(int argc, char **argv){
  struct { unsigned short fam; char path[108]; } sa;
  unsigned uid = (argc>1)? (unsigned)A(argv[1]) : 0u;
  unsigned gid = (argc>2)? (unsigned)A(argv[2]) : 0u;
  const char *p = (argc>3)? argv[3] : "/dev/shm/mib3cluster.sock";
  int i, fd, r;

  P("socktest: target uid="); Pn((long)uid); P(" gid="); Pn((long)gid); P(" path="); P(p); P("\n");
  if (gid){ r = setgid(gid); P("setgid rc="); Pn(r); if (r) { P(" errno="); Pn((long)ERRNO); } P("\n"); }
  if (uid){ r = setuid(uid); P("setuid rc="); Pn(r); if (r) { P(" errno="); Pn((long)ERRNO); } P("\n"); }

  memset(&sa, 0, sizeof sa);
  for (i=0;i<107 && p[i];i++) sa.path[i]=p[i];
  sa.fam = 1;
  fd = socket(1, 1, 0);
  P("socket fd="); Pn(fd); if (fd<0){ P(" errno="); Pn((long)ERRNO); P("\n"); return 1; } P("\n");
  r = connect(fd, &sa, sizeof(sa));
  P("connect rc="); Pn(r); if (r){ P(" errno="); Pn((long)ERRNO); } P("\n");
  P(r==0 ? "CONNECT OK\n" : "CONNECT FAILED\n");
  return 0;
}
