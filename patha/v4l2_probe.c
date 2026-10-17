/* ============================================================================
 * v4l2_probe.c -- what IS each /dev/video* node?  (runs on the unit)
 *
 * WHY: before building anything on /dev/video32 we must CONFIRM it is the hardware
 * H.264 decoder and not, say, the encoder MS-S uses (/dev/video33) or a camera/VFE
 * node.  VIDIOC_QUERYCAP returns the kernel driver name and the device capabilities,
 * which distinguish a memory-to-memory DECODER (OUTPUT_MPLANE + CAPTURE_MPLANE +
 * M2M) from an encoder (same shape, different driver/card) and from a capture-only
 * node.
 *
 * Built -nostdlib against the unit's own libc, own _start (the house recipe).
 * ==========================================================================*/
#define NULL ((void*)0)
extern int  open(const char *, int, ...);
extern int  close(int);
extern int  ioctl(int, unsigned long, ...);
extern int  write(int, const void *, unsigned long);
extern void _exit(int);

typedef unsigned int u32;

struct v4l2_capability {
  char driver[16];
  char card[32];
  char bus_info[32];
  u32  version;
  u32  caps;
  u32  device_caps;
  u32  reserved[3];
};
#define VIDIOC_QUERYCAP 0x80685600UL      /* _IOR('V', 0, struct v4l2_capability), size 104 */

static unsigned slen(const char *s){ unsigned n=0; while (s && s[n]) n++; return n; }
static void P(const char *s){ write(1, s, slen(s)); }
static void Pv(unsigned long v){
  char b[24]; int i=0;
  if (!v){ write(1,"0",1); return; }
  while (v){ b[i++] = (char)('0'+v%10); v/=10; }
  while (i) write(1, &b[--i], 1);
}
static void Ph(unsigned long v){
  char b[20]; int i=0; const char *h="0123456789abcdef";
  if (!v){ write(1,"0",1); return; }
  while (v){ b[i++]=h[v&0xf]; v>>=4; }
  while (i) write(1,&b[--i],1);
}

int main(int argc, char **argv);
void _start(void){
  static char cbuf[512]; static char *av[16];
  int n=0, argc=0, i=0, fd=open("/proc/self/cmdline",0);
  if (fd>=0){ long r=0; extern long read(int,void*,unsigned long);
              r = read(fd,cbuf,(unsigned long)sizeof(cbuf)-1); if(r>0) n=(int)r; close(fd); }
  cbuf[n]=0;
  while (i<n && argc<15){ av[argc++]=&cbuf[i]; while (i<n && cbuf[i]) i++; i++; }
  av[argc]=0;
  _exit(main(argc,av));
}

static const char *NODES[] = { "/dev/video0", "/dev/video32", "/dev/video33", "/dev/video35", 0 };

int main(int argc, char **argv){
  int k;
  (void)argc; (void)argv;
  for (k = 0; NODES[k]; k++){
    struct v4l2_capability c;
    int fd, rc, j;
    char *p = (char*)&c;
    for (j = 0; j < (int)sizeof(c); j++) p[j] = 0;
    P("=== "); P(NODES[k]); P(" ===\n");
    fd = open(NODES[k], 2 /*O_RDWR*/);
    if (fd < 0){ P("   open(O_RDWR) FAILED\n\n"); continue; }
    rc = ioctl(fd, VIDIOC_QUERYCAP, &c);
    if (rc != 0){ P("   VIDIOC_QUERYCAP FAILED rc="); Pv((unsigned long)(long)rc); P("\n\n"); close(fd); continue; }
    P("   driver  : "); P(c.driver);    P("\n");
    P("   card    : "); P(c.card);      P("\n");
    P("   bus     : "); P(c.bus_info);  P("\n");
    P("   caps    : 0x"); Ph(c.caps);       P("\n");
    P("   devcaps : 0x"); Ph(c.device_caps); P("\n");
    P("   -> "); 
    if (c.device_caps & 0x00004000) P("VIDEO_M2M_MPLANE ");
    if (c.device_caps & 0x00008000) P("VIDEO_M2M ");
    if (c.device_caps & 0x00001000) P("CAPTURE_MPLANE ");
    if (c.device_caps & 0x00002000) P("OUTPUT_MPLANE ");
    if (c.device_caps & 0x00000001) P("CAPTURE ");
    if (c.device_caps & 0x00000002) P("OUTPUT ");
    if (c.device_caps & 0x04000000) P("STREAMING ");
    P("\n\n");
    close(fd);
  }
  return 0;
}
