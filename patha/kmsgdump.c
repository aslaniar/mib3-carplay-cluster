/* kmsgdump.c -- drain the unit's kernel log to stdout for a bounded time.
 *
 * WHY: on this unit `dmesg` prints nothing and a blocking `cat /proc/kmsg`
 * returns no buffered lines, so the driver's own errors (msm_vidc) are only
 * reachable via a NON-BLOCKING read of /proc/kmsg.  This tool opens it
 * O_NONBLOCK and drains whatever is buffered for <ms> milliseconds.
 *
 * usage: kmsgdump [ms]        (default 1500)
 * Cross-build (libc only), from shim/tools/:
 *   clang --target=aarch64-linux-gnu -O2 -nostdlib -fPIE -fuse-ld=ld.lld \
 *         -Wl,-e,_start -o kmsgdump kmsgdump.c <rootfs>/lib64/libc.so.6 \
 *         <rootfs>/lib64/libdl-2.23.so
 */
typedef unsigned long usize_t;
extern int open(const char *, int, ...);
extern long read(int, void *, usize_t);
extern int write(int, const void *, usize_t);
extern int close(int);
extern int usleep(unsigned int);
extern void _exit(int);

static unsigned slen(const char *s){ unsigned n=0; while(s&&s[n])n++; return n; }
static void P(const char*s){ write(1,s,slen(s)); }
static int my_atoi(const char*s){ int v=0; while(s && *s>='0'&&*s<='9'){ v=v*10+(*s-'0'); s++; } return v; }

/* freestanding argv from /proc/self/cmdline (same pattern as the other tools) */
int main(int argc, char **argv);
void _start(void){
  static char cb[512]; static char *av[16]; int n=0, ac=0, i=0;
  int fd=open("/proc/self/cmdline",0);
  if(fd>=0){ long r=read(fd,cb,(usize_t)sizeof(cb)-1); if(r>0)n=(int)r; close(fd); }
  cb[n]=0;
  while(i<n && ac<15){ av[ac++]=&cb[i]; while(i<n&&cb[i])i++; i++; }
  av[ac]=0; _exit(main(ac,av));
}

int main(int argc, char **argv){
  const char *path = (argc>1 && argv[1][0]=='/') ? argv[1] : "/proc/kmsg";
  int ms = (argc>2) ? my_atoi(argv[2]) : ((argc>1 && argv[1][0]!='/') ? my_atoi(argv[1]) : 1500);
  if(ms<=0) ms = 1500;
  /* O_RDONLY=0 | O_NONBLOCK=0x800 */
  int fd = open(path, 0x800);
  if(fd < 0){ P("kmsgdump: open "); P(path); P(" failed\n"); return 2; }
  int elapsed = 0;
  char buf[8192];
  while(elapsed < ms){
    /* drain until the ring is empty, THEN poll — /dev/kmsg returns one record
     * per read, so a sleep-per-read would never catch up on a big ring. */
    for(;;){
      long r = read(fd, buf, sizeof(buf));
      if(r > 0){ long off=0; while(off<r){ long w=write(1, buf+off, (usize_t)(r-off)); if(w<=0)break; off+=w; } }
      else break;
    }
    usleep(20000); elapsed += 20;
  }
  close(fd);
  return 0;
}
