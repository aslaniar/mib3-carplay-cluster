/* ausock.c -- feed Annex-B access units from a file into the cluster helper's
 * AF_UNIX socket (mib3helper.h wire format: 16-byte header + payload).
 *
 * Lets us exercise the helper's Cinemo decode bridge with a FILE, no phone and
 * no shim injection -- a standalone decode+render test.
 *
 * usage: ausock <file.264> [socket] [fps]
 * Cross-build (libc only), from shim/tools/:
 *   clang --target=aarch64-linux-gnu -O2 -nostdlib -fPIE -fuse-ld=ld.lld \
 *         -Wl,-e,_start -o ausock ausock.c <rootfs>/lib64/libc.so.6 \
 *         <rootfs>/lib64/libdl-2.23.so
 */
typedef unsigned long usize_t;
extern int open(const char *, int, ...);
extern long read(int, void *, usize_t);
extern long write(int, const void *, usize_t);
extern int close(int);
extern void _exit(int);
extern int usleep(unsigned int);
extern void *mmap(void *, usize_t, int, int, int, long);
extern int socket(int, int, int);
extern int connect(int, const void *, unsigned int);
extern int getenv_placeholder(void);
extern char *getenv(const char *);
extern int setgid(unsigned);
extern int setuid(unsigned);

static unsigned slen(const char *s){ unsigned n=0; while(s&&s[n])n++; return n; }
static void P(const char *s){ write(1,s,slen(s)); }
static void Pn(long v){ char b[24]; int i=0; if(v<0){write(1,"-",1);v=-v;} if(!v){write(1,"0",1);return;} while(v){b[i++]=(char)('0'+v%10);v/=10;} while(i)write(1,&b[--i],1); }
static int my_atoi(const char *s){ int v=0; while(s&&*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return v; }

int main(int argc, char **argv);
void _start(void){
  static char cb[512]; static char *av[16]; int n=0, ac=0, i=0;
  int fd=open("/proc/self/cmdline",0);
  if(fd>=0){ long r=read(fd,cb,(usize_t)sizeof(cb)-1); if(r>0)n=(int)r; close(fd); }
  cb[n]=0;
  while(i<n && ac<15){ av[ac++]=&cb[i]; while(i<n&&cb[i])i++; i++; }
  av[ac]=0; _exit(main(ac,av));
}

struct sockaddr_un { unsigned short sun_family; char sun_path[108]; };

int main(int argc, char **argv){
  const char *path  = (argc>1)?argv[1]:"/tmp/kit/real.264";
  const char *sockp = (argc>2)?argv[2]:"/dev/shm/mib3cluster.sock";
  int fps = (argc>3)?my_atoi(argv[3]):10; if(fps<1)fps=10;
  int fd = open(path, 0);
  if (fd < 0){ P("ausock: cannot open "); P(path); P("\n"); return 2; }
  long fsz = 0; { char tmp[1]; long r; while((r=read(fd,tmp,1))==1) fsz++; }  /* poor-man's size */
  close(fd); fd = open(path, 0);
  unsigned char *file = (unsigned char*)mmap(0, (usize_t)fsz, 3, 0x22, -1, 0);
  if (file == (void*)-1 || !file){ P("ausock: mmap fail\n"); return 2; }
  { long got=0; while(got<fsz){ long r=read(fd,file+got,(usize_t)(fsz-got)); if(r<=0)break; got+=r; } fsz=got; }
  close(fd);
  P("ausock: file="); P(path); P(" bytes="); Pn(fsz); P("\n");

  /* split into access units: new AU at SPS/AUD, or at a slice once one is open */
  long starts[4096]; int nst=0, cur=0; long k=0;
  while(k+3<fsz && nst<4095){
    if(file[k]==0 && file[k+1]==0 && file[k+2]==1){
      int t=file[k+3]&0x1f; int begin=0;
      if(t==7||t==9) begin=1; else if((t==1||t==5) && cur) begin=1;
      if(begin){ starts[nst++]=k; cur=0; }
      if(t==1||t==5) cur=1;
      k+=3; continue;
    }
    k++;
  }
  if(nst==0){ starts[nst++]=0; } starts[nst]=fsz;
  P("ausock: AUs="); Pn(nst); P("\n");

  int s = socket(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0);
  struct sockaddr_un sa; int i;
  /* Optionally drop to a target uid/gid -- used to prove that a process with
   * carplay's credentials (986:44, label carplay::app) can reach the socket. */
  { char *u = getenv("AUSOCK_UID"), *g = getenv("AUSOCK_GID");
    if (g && g[0]){ P("ausock: setgid "); Pn(my_atoi(g)); P("\n"); setgid((unsigned)my_atoi(g)); }
    if (u && u[0]){ P("ausock: setuid "); Pn(my_atoi(u)); P("\n"); setuid((unsigned)my_atoi(u)); } }
  for(i=0;i<(int)sizeof(sa);i++) ((char*)&sa)[i]=0;
  sa.sun_family = 1;
  for(i=0; sockp[i] && i<107; i++) sa.sun_path[i]=sockp[i];
  if (connect(s, &sa, (unsigned int)sizeof(sa)) != 0){ P("ausock: connect failed\n"); return 3; }
  P("ausock: connected\n");

  { int idx; unsigned long seq=0; int sent=0;
    for(idx=0; idx<nst; idx++){
      long b0=starts[idx], b1=starts[idx+1]; long len=b1-b0; long j;
      unsigned char hdr[16];
      if(len<=0) continue;
      hdr[0]=0x31; hdr[1]=0x43; hdr[2]=0x33; hdr[3]=0x4d;               /* magic 0x4d334331 LE */
      hdr[4]=(len)&0xff; hdr[5]=(len>>8)&0xff; hdr[6]=(len>>16)&0xff; hdr[7]=(len>>24)&0xff;
      for(j=0;j<8;j++) hdr[8+j]=(unsigned char)((seq>>(j*8))&0xff);
      { long off=0; while(off<16){ long w=write(s,hdr+off,(usize_t)(16-off)); if(w<=0)break; off+=w; } }
      { long off=0; while(off<len){ long w=write(s,file+b0+off,(usize_t)(len-off)); if(w<=0)break; off+=w; } }
      seq++; sent++;
      P("ausock: AU "); Pn(idx); P(" len="); Pn(len); P("\n");
      usleep((unsigned)(1000000/fps));
    }
    P("ausock: sent AUs="); Pn(sent); P("\n");
  }
  close(s);
  return 0;
}
