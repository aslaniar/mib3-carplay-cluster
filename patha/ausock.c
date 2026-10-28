/* ausock.c -- feed Annex-B access units from a file into the cluster helper's
 * AF_UNIX socket (mib3helper.h wire format: 16-byte header + payload).
 *
 * Lets us exercise the decoder with a FILE, no phone and no shim injection --
 * a standalone decode+render test.
 *
 * usage: ausock <file.264> [socket] [fps]
 *   env: AUSOCK_UID / AUSOCK_GID  drop to these credentials before connecting
 *
 * Build: shim/tools/build.sh (aarch64, libc-only).
 */
#include "toolkit.h"

extern int   usleep(unsigned int);
extern void *mmap(void *, unsigned long, int, int, int, long);
extern int   socket(int, int, int);
extern int   connect(int, const void *, unsigned int);
extern int   setgid(unsigned);
extern int   setuid(unsigned);

int main(int argc, char **argv);
void _start(void){ TOOL_ARGS(); }

struct sockaddr_un { unsigned short sun_family; char sun_path[108]; };

int main(int argc, char **argv){
  const char *path  = (argc>1)?argv[1]:"/tmp/kit/real.264";
  const char *sockp = (argc>2)?argv[2]:"/dev/shm/mib3cluster.sock";
  int fps = (argc>3)?A(argv[3]):10; if(fps<1)fps=10;
  long fsz;
  int fd = open(path, 0);
  if (fd < 0){ P("ausock: cannot open "); P(path); P("\n"); return 2; }
  /* size via lseek.  This used to read the whole file ONE BYTE AT A TIME just
   * to count it (~2.4M read() calls for a real capture). */
  fsz = lseek(fd, 0, 2 /*SEEK_END*/);
  if (fsz <= 0){ P("ausock: empty or unseekable input\n"); close(fd); return 2; }
  lseek(fd, 0, 0 /*SEEK_SET*/);
  unsigned char *file = (unsigned char*)mmap(0, (unsigned long)fsz, 3, 0x22, -1, 0);
  if (file == (void*)-1 || !file){ P("ausock: mmap fail\n"); close(fd); return 2; }
  { long got=0; while(got<fsz){ long r=read(fd,file+got,(unsigned long)(fsz-got)); if(r<=0)break; got+=r; } fsz=got; }
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
  struct sockaddr_un sa;
  /* Optionally drop to a target uid/gid -- used to prove that a process with
   * carplay's credentials (986:44, label carplay::app) can reach the socket. */
  { char *u = getenv("AUSOCK_UID"), *g = getenv("AUSOCK_GID");
    if (g && g[0]){ P("ausock: setgid "); Pn(A(g)); P("\n"); setgid((unsigned)A(g)); }
    if (u && u[0]){ P("ausock: setuid "); Pn(A(u)); P("\n"); setuid((unsigned)A(u)); } }
  memset(&sa, 0, sizeof sa);
  sa.sun_family = 1;
  { int i; for(i=0; sockp[i] && i<107; i++) sa.sun_path[i]=sockp[i]; }
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
      { long off=0; while(off<16){ long w=write(s,hdr+off,(unsigned long)(16-off)); if(w<=0)break; off+=w; } }
      { long off=0; while(off<len){ long w=write(s,file+b0+off,(unsigned long)(len-off)); if(w<=0)break; off+=w; } }
      seq++; sent++;
      P("ausock: AU "); Pn(idx); P(" len="); Pn(len); P("\n");
      usleep((unsigned)(1000000/fps));
    }
    P("ausock: sent AUs="); Pn(sent); P("\n");
  }
  close(s);
  return 0;
}
