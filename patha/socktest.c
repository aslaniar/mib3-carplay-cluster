/* socktest.c -- reproduce the SHIM's helper connect exactly, as the CARPLAY
 * uid/gid (986:44) with the carplay::app exec label, and print the errno.
 * Usage: socktest <uid> <gid> [path]
 * Built with the house recipe (own _start, -nostdlib). */
extern int   socket(int, int, int);
extern int   connect(int, const void *, unsigned int);
extern int   setgid(unsigned);
extern int   setuid(unsigned);
extern int   write(int, const void *, unsigned long);
extern int   open(const char *, int, ...);
extern long  read(int, void *, unsigned long);
extern void  _exit(int);
extern int  *__errno_location(void);
#define ERRNO (*__errno_location())

static unsigned sl(const char *s){ unsigned n=0; while (s && s[n]) n++; return n; }
static void P(const char *s){ write(1, s, sl(s)); }
static void Pv(long v){ char b[24]; int i=0; if(v<0){write(1,"-",1);v=-v;} if(!v){write(1,"0",1);return;} while(v){b[i++]=(char)('0'+v%10);v/=10;} while(i) write(1,&b[--i],1); }
static int atoi_(const char *s){ int v=0; while (*s>='0'&&*s<='9'){ v=v*10+(*s-'0'); s++; } return v; }

int main(int argc, char **argv){
  struct { unsigned short fam; char path[108]; } sa;
  unsigned uid = (argc>1)? (unsigned)atoi_(argv[1]) : 0u;
  unsigned gid = (argc>2)? (unsigned)atoi_(argv[2]) : 0u;
  const char *p = (argc>3)? argv[3] : "/dev/shm/mib3cluster.sock";
  int i, fd, r;

  P("socktest: target uid="); Pv((long)uid); P(" gid="); Pv((long)gid); P(" path="); P(p); P("\n");
  if (gid){ r = setgid(gid); P("setgid rc="); Pv(r); if (r) { P(" errno="); Pv((long)ERRNO); } P("\n"); }
  if (uid){ r = setuid(uid); P("setuid rc="); Pv(r); if (r) { P(" errno="); Pv((long)ERRNO); } P("\n"); }

  for (i=0;i<108;i++) sa.path[i]=0;
  for (i=0;i<107 && p[i];i++) sa.path[i]=p[i];
  sa.fam = 1;
  fd = socket(1, 1, 0);
  P("socket fd="); Pv(fd); if (fd<0){ P(" errno="); Pv((long)ERRNO); P("\n"); return 1; } P("\n");
  r = connect(fd, &sa, sizeof(sa));
  P("connect rc="); Pv(r); if (r){ P(" errno="); Pv((long)ERRNO); } P("\n");
  P(r==0 ? "CONNECT OK\n" : "CONNECT FAILED\n");
  return 0;
}
int main(int, char **);
void _start(void){
  static char cb[512]; static char *av[16]; int n=0, ac=0, i=0;
  int fd = open("/proc/self/cmdline", 0);
  if (fd >= 0){ long r = read(fd, cb, sizeof(cb)-1); if (r>0) n=(int)r; }
  cb[n]=0;
  while (i<n && ac<15){ av[ac++]=&cb[i]; while (i<n && cb[i]) i++; i++; }
  av[ac]=0; _exit(main(ac,av));
}
