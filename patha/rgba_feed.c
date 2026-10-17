#define NULL ((void*)0)
/* ============================================================================
 * rgba_feed.c -- push raw RGBA frames into a running presenter's --rgba-sock.
 *
 *   *** tiny utility; runs ON THE UNIT (aarch64) and on the DESK bench. ***
 *
 * The EGL presenter (mib3egl.c) OWNS ivi surface 3000 and serves an AF_UNIX
 * socket; any DECODER writes w*h*4 RGBA frames to it back-to-back.  On the unit
 * that decoder is the vendor one (Cinemo/OMX); on the desk it is
 * tools/h264ipcm.py.  This tool is the third option -- feed a *known* RGBA blob
 * (e.g. an offline-decoded frame) so the delivery half can be exercised with no
 * decoder at all:
 *
 *   rgba_feed <sock> <rgbafile> <w> <h> [--loop] [--fps N] [--count N]
 *
 * It is used by harness/run_delivery.sh to prove the decode-to-texture delivery
 * chain offline: h264ipcm.py --out frame.rgba  ->  rgba_feed  ->  mib3egl tex.
 * ==========================================================================*/
typedef unsigned int  u32;
typedef unsigned long usize;

extern int    open(const char *, int, ...);
extern long   read(int, void *, unsigned long);
extern int    close(int);
extern void   _exit(int);
extern int    write(int, const void *, unsigned long);
extern int    usleep(unsigned int);
extern void  *malloc(unsigned long);
extern int    socket(int, int, int);
extern int    connect(int, const void *, unsigned int);
static int streq(const char *a, const char *b){ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void put(const char *s){ usize n=0; while (s[n]) n++; write(2, s, n); }
static void putnum(long v){ char b[24]; int i=0; if(v<0){write(2,"-",1);v=-v;} if(!v){write(2,"0",1);return;} while(v){b[i++]=(char)('0'+v%10);v/=10;} while(i) write(2,&b[--i],1); }
static void ok(const char*m){ put("[feed] "); put(m); put("\n"); }
static void num(const char*m,long v){ put("[feed] "); put(m); put("="); putnum(v); put("\n"); }
static int my_atoi(const char*s){ long v=0; int neg=0; if(*s=='-'){neg=1;s++;} while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return (int)(neg?-v:v); }
static int read_full(int fd, void *buf, unsigned long n){
  unsigned long got=0; while (got<n){ long r=read(fd,(char*)buf+got,n-got); if(r<=0) return 0; got+=(unsigned long)r; } return 1;
}
static int write_full(int fd, const void *buf, unsigned long n){
  unsigned long put_=0; while (put_<n){ long r=write(fd,(const char*)buf+put_,n-put_); if(r<=0) return 0; put_+=(unsigned long)r; } return 1;
}

int main(int argc, char **argv);
void _start(void){
  static char cbuf[1024]; static char *av[48], *nav[48];
  int n=0, argc=0, i=0, fd=open("/proc/self/cmdline",0);
  int skip=1, k;
  if (fd>=0){ long r=read(fd,cbuf,(unsigned long)sizeof(cbuf)-1); if(r>0) n=(int)r; close(fd); }
  cbuf[n]=0;
  while (i<n && argc<47){ av[argc++]=&cbuf[i]; while (i<n && cbuf[i]) i++; i++; }
  av[argc]=0;
  { const char *s=av[0]; for (const char *p=av[0]; *p; p++) if (*p=='/') s=p+1;
    if (s[0]=='l' && s[1]=='d' && (s[2]=='-'||s[2]=='.')){
      int j=1;
      while (j<argc){ const char *t=av[j];
        if (streq(t,"--library-path")||streq(t,"--preload")||streq(t,"--audit")||streq(t,"--argv0")||streq(t,"--inhibit-cache")){ j+=2; continue; }
        if (t[0]=='-'){ j+=1; continue; }
        break; }
      skip=j; } }
  for (k=0; skip+k<argc && k<47; k++) nav[k]=av[skip+k];
  nav[k]=0;
  _exit(main(k, nav));
}

int main(int argc, char **argv){
  const char *sock=0, *path=0;
  int w=1280, h=480, npos=0, loop=0, count=0, fps=0, i;
  int fd, lfd; struct { unsigned short fam; char path[108]; } sa;
  unsigned char *buf; unsigned long need, nread; long r;
  for (i=1;i<argc;i++){ const char *a=argv[i];
    if      (streq(a,"--loop")) loop=1;
    else if (streq(a,"--fps") && i+1<argc) fps=my_atoi(argv[++i]);
    else if (streq(a,"--count") && i+1<argc) count=my_atoi(argv[++i]);
    else if (a[0]=='-'){ }
    else { switch(npos++){ case 0: sock=a; break; case 1: path=a; break; case 2: w=my_atoi(a); break; case 3: h=my_atoi(a); break; default: break; } }
  }
  if (!sock || !path){ ok("usage: rgba_feed <sock> <rgbafile> <w> <h> [--loop] [--fps N] [--count N]"); return 2; }
  need = (unsigned long)w * (unsigned long)h * 4UL;
  fd = open(path, 0);
  if (fd < 0){ ok("FAIL open rgbafile"); return 2; }
  buf = malloc(need);
  if (!buf || !read_full(fd, buf, need)){ ok("FAIL read rgbafile (need w*h*4 bytes)"); return 2; }
  close(fd); num("frame bytes", (long)need);
  for (i=0;i<108;i++) sa.path[i]=0;
  for (i=0;i<107 && sock[i];i++) sa.path[i]=sock[i];
  sa.fam=1;
  lfd = socket(1,1,0);
  if (lfd<0){ ok("FAIL socket"); return 3; }
  { int tries=0; while (connect(lfd,&sa,sizeof(sa))!=0){ if (++tries>50){ ok("FAIL connect (presenter not listening?)"); return 3; } usleep(100000); } }
  ok("connected to presenter rgba-sock");
  nread = 0;
  for (;;){
    if (!write_full(lfd, buf, need)){ ok("presenter closed the socket"); break; }
    nread++;
    if (count && (int)nread >= count) break;
    if (!loop && !count && nread >= 1) break;
    if (fps > 0) usleep((unsigned int)(1000000/fps));
  }
  num("frames sent", (long)nread);
  r = 0; (void)r;
  return 0;
}
