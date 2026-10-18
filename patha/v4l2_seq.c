/* ============================================================================
 * v4l2_seq.c -- replicate the VENDOR's exact /dev/video32 (msm_vdec_8974)
 *               session so the hardware H.264 decoder writes decoded frames
 *               into buffers WE own.  Then we can present them ourselves.
 *
 * WHY THIS IS DIFFERENT from v4l2_dec.c (which failed): a strace of the
 * helper's own Cinemo decode bridge on the car (2026-10-17 20:28) captured the
 * vendor's complete ioctl sequence (saved: MIB3_RE/data/v4l2_vendor_ref_20261017.txt).
 * The differences that matter, all of which v4l2_dec.c got wrong:
 *
 *   1. CAPTURE is TWO-PLANE: plane[0] = the NV12 frame (sizeimage 942080 for
 *      1280x480), plane[1] = a 16384-byte EXTRADATA plane.  QBUF must carry
 *      length=2 (2 planes).  v4l2_dec queued length=1 -> the driver rejects it.
 *   2. VIDIOC_PREPARE_BUF is called on every capture buffer BEFORE STREAMON.
 *   3. Private controls the driver requires (copied byte-for-byte from the
 *      strace): 0x99202e=32784; 0x992007=0; 0x992019 = 2,6,25,29,26,27;
 *      after STREAMON: 0x99201a=2.
 *   4. The vendor ORDER: probe S_FMT 320x240, then REQBUFS(cap,1)/REQBUFS(out,1),
 *      then the real S_FMT 1280x480, REQBUFS(out,4), S_FMT cap NV12,
 *      REQBUFS(cap,20), PREPARE_BUF x20, STREAMON(CAPTURE) FIRST, then QBUF cap
 *      x20, QBUF out, STREAMON(OUTPUT).
 *   5. USERPTR everywhere (never MMAP/QUERYBUF -- QUERYBUF is unimplemented).
 *
 * STAGE 1 (this tool): prove frames come back; optionally dump frame 0 as raw
 * NV12 and as RGBA (so we can eyeball it and feed the mib3egl presenter).
 *
 * usage: v4l2_seq <file.264> [max_chunks]
 *   env: V4L2_PROBE=0   skip the 320x240 probe S_FMT
 *        V4L2_CTRL=0    skip the private S_CTRLs
 *        V4L2_PREPARE=0 skip PREPARE_BUF
 *        V4L2_CAPN=<n>  capture buffer count (default 20)
 *        V4L2_OUTN=<n>  output buffer count (default 4)
 *        V4L2_DUMP=<path>  write frame 0 as RGBA (w*h*4)
 *        V4L2_NV12=<path>  write frame 0 as raw NV12
 *        V4L2_FPS=1     pace output submissions
 *
 * Built -nostdlib against the unit's own libc, own _start (house recipe:
 * sh build.sh in this dir).
 * ==========================================================================*/
#define NULL ((void*)0)
extern int   open(const char *, int, ...);
extern long  read(int, void *, unsigned long);
extern long  write(int, const void *, unsigned long);
extern int   close(int);
extern int   ioctl(int, unsigned long, ...);
extern void  _exit(int);
extern void *mmap(void *, unsigned long, int, int, int, long);
extern char *getenv(const char *);
extern void *malloc(unsigned long);
extern unsigned int usleep(unsigned int);
extern int bind(int, const void *, unsigned int);
extern int listen(int, int);
extern int accept(int, void *, unsigned int *);
extern int chmod(const char *, unsigned);
extern int unlink(const char *);
typedef unsigned int u32; typedef unsigned long u64;

#define IOC_NONE  0u
#define IOC_WRITE 1u
#define IOC_READ  2u
#define IOC(dir,type,nr,size) (((dir)<<30)|((size)<<16)|((type)<<8)|(nr))

/* ---- structs (hand-declared; layouts match the kernel's own types) ---- */
struct v4l2_plane_pix_format { u32 sizeimage; u32 bytesperline; unsigned short reserved[6]; };
#define V4L2_MAX_PLANES 8
struct v4l2_pix_format_mplane {
  u32 width, height, pixelformat, field;
  struct v4l2_plane_pix_format plane_fmt[V4L2_MAX_PLANES];
  unsigned char num_planes, flags, reserved[10];
};
struct v4l2_format { u32 type; u32 rsvd; unsigned char body[200]; };
#define FMT_W(f)    (*(u32*)((f)->body + 0))
#define FMT_H(f)    (*(u32*)((f)->body + 4))
#define FMT_FCC(f)  (*(u32*)((f)->body + 8))
#define FMT_FLD(f)  (*(u32*)((f)->body + 12))
/* v4l2_pix_format_mplane has colorspace at +16 (the strace shows it), so
 * plane_fmt[] starts at +20 (NOT +16) and num_planes is at +180 (NOT +176).
 * Getting this wrong made every sizeimage/length 0 -> QBUF EINVAL. */
#define FMT_NPL(f)  (*((unsigned char*)((f)->body + 180)))
#define FMT_PSZ(f,i) (*(u32*)((f)->body + 20 + (i)*20 + 0))
#define FMT_PBP(f,i) (*(u32*)((f)->body + 20 + (i)*20 + 4))
struct v4l2_requestbuffers { u32 count, type, memory, reserved[2]; };
struct my_timeval { long tv_sec, tv_usec; };
struct my_timecode { u32 type, flags; unsigned char frames, seconds, minutes, hours, userbits[4]; };
struct v4l2_plane { u32 bytesused, length; union { u32 mem_offset; unsigned long userptr; int fd; } m; u32 data_offset, reserved[11]; };
struct v4l2_buffer {
  u32 index, type, bytesused, flags, field;
  struct my_timeval timestamp;
  struct my_timecode timecode;
  u32 sequence, memory;
  union { u32 offset; unsigned long userptr; void *planes; int fd; } m;
  u32 length, reserved2, reserved;
};
struct v4l2_control { u32 id; int value; };
struct v4l2_event_subscription { u32 type, id, flags, reserved[5]; };

#define V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 9
#define V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE  10
#define V4L2_MEMORY_USERPTR 2
#define V4L2_FIELD_ANY 0
#define V4L2_PIX_FMT_H264 0x34363248u  /* 'H264' */
#define V4L2_PIX_FMT_NV12 0x3231564eu  /* 'NV12' */
#define V4L2_PIX_FMT_Q128 0x38323151u  /* 'Q128' NV12 UBWC */
#define V4L2_EVENT_SOURCE_CHANGE 5

#define VIDIOC_S_FMT      IOC(3u,'V',5, sizeof(struct v4l2_format))
#define VIDIOC_G_FMT      IOC(3u,'V',4, sizeof(struct v4l2_format))
#define VIDIOC_REQBUFS    IOC(3u,'V',8, sizeof(struct v4l2_requestbuffers))
#define VIDIOC_S_CTRL     IOC(3u,'V',28,sizeof(struct v4l2_control))  /* nr 28! 27 is G_CTRL */
#define VIDIOC_QBUF       IOC(3u,'V',15,sizeof(struct v4l2_buffer))
#define VIDIOC_DQBUF      IOC(3u,'V',17,sizeof(struct v4l2_buffer))
#define VIDIOC_PREPARE_BUF IOC(3u,'V',93,sizeof(struct v4l2_buffer))
#define VIDIOC_STREAMON   IOC(1u,'V',18,4)
#define VIDIOC_STREAMOFF  IOC(1u,'V',19,4)
#define VIDIOC_SUBSCRIBE_EVENT IOC(1u,'V',90,sizeof(struct v4l2_event_subscription))
#define VIDIOC_DQEVENT    IOC(2u,'V',89,128)
#define VIDIOC_DECODER_CMD IOC(3u,'V',96,72)   /* msm_vidc: 72-byte struct */

#define ION_IOC_ALLOC 0xC0204900UL
#define ION_IOC_MAP   0xC0084902UL
#define ION_IOC_SHARE 0xC0084904UL

/* ---- tiny print helpers ---- */
static unsigned slen(const char *s){ unsigned n=0; while (s && s[n]) n++; return n; }
static void P(const char *s){ write(1, s, slen(s)); }
static void Pv(long v){ char b[24]; int i=0; if(v<0){write(1,"-",1);v=-v;} if(!v){write(1,"0",1);return;} while(v){b[i++]=(char)('0'+v%10);v/=10;} while(i) write(1,&b[--i],1); }
static void Pk(const char*t,long v){ P(t); P("="); Pv(v); P("\n"); }
extern int *__errno_location(void);
#define ERRNO (*__errno_location())
static void Prc(const char *t, int rc){ P(t); P(" rc="); Pv(rc); if(rc) { P(" errno="); Pv((long)ERRNO); } P("\n"); }
static int my_atoi(const char*s){ long v=0; while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return (int)v; }
static int envnum(const char*n, int d){ char*v=getenv(n); return v?v[0]?my_atoi(v):d:d; }

int main(int argc, char **argv);
void _start(void){
  static char cbuf[1024]; static char *av[16];
  int n=0, argc=0, i=0, fd=open("/proc/self/cmdline",0);
  if (fd>=0){ long r=read(fd,cbuf,(unsigned long)sizeof(cbuf)-1); if(r>0) n=(int)r; close(fd); }
  cbuf[n]=0;
  while (i<n && argc<15){ av[argc++]=&cbuf[i]; while (i<n && cbuf[i]) i++; i++; }
  av[argc]=0;
  _exit(main(argc,av));
}

struct ion_alloc_ { unsigned long len, align; unsigned int heap_mask, flags; int handle; };
struct ion_fd_    { int handle, fd; };

#define OUTN_MAX 8
#define CAPN_MAX 24
#define OUTBUF_SZ (8u<<20)          /* per output chunk: 8 MiB (>= negotiated 7077888) */

static unsigned int g_heap = 0xffffffffu;   /* V4L2_HEAP override */
static int g_dmabuf = 0;                    /* V4L2_DMABUF=1 -> memory=DMABUF */
static int g_ptrfd = 0;                     /* V4L2_PTRFD=1  -> userptr slot carries the ion FD */
static int g_shfd  = 0;                     /* V4L2_SHFD=1   -> userptr slot carries the ion SHARE (dma_buf) fd */
static int g_fdfield = 0;                   /* V4L2_FDFIELD: 0=userptr VA, 1=m.fd(ion), 2=m.fd(share), 3=reserved[0]=fd, 4=data_offset=fd, 5=reserved[1]=fd */
static unsigned int g_ionflags = 0;         /* V4L2_IONFLAG  -> ion alloc flags */

static void *ion_alloc(int ionfd, unsigned long len, unsigned long *out_len, int *out_fd, int *out_share){
  struct ion_alloc_ a; struct ion_fd_ f; int r, i; void *p;
  for (i=0;i<(int)sizeof(a);i++) ((char*)&a)[i]=0;
  a.len = len; a.align = 4096; a.heap_mask = g_heap; a.flags = g_ionflags;
  r = ioctl(ionfd, ION_IOC_ALLOC, &a);
  if (r != 0){ P("ION alloc FAIL len="); Pv((long)len); P(" heap=0x"); { const char*hx="0123456789abcdef"; int q; for(q=7;q>=0;q--) write(1,&hx[(g_heap>>(q*4))&0xf],1);} P(" errno="); Pv((long)ERRNO); P("\n"); return 0; }
  for (i=0;i<(int)sizeof(f);i++) ((char*)&f)[i]=0;
  f.handle = a.handle;
  r = ioctl(ionfd, ION_IOC_MAP, &f);
  if (r != 0){ P("ION map FAIL errno="); Pv((long)ERRNO); P("\n"); return 0; }
  p = mmap(NULL, len, 3, 1, f.fd, 0);
  if (p == (void*)-1){ P("ION mmap FAIL\n"); return 0; }
  if (out_len) *out_len = len;
  if (out_fd) *out_fd = f.fd;
  if (out_share){ struct ion_fd_ sf; int z, shfd = -1;
    for (z=0;z<(int)sizeof(sf);z++) ((char*)&sf)[z]=0;
    sf.handle = a.handle;
    if (ioctl(ionfd, ION_IOC_SHARE, &sf) == 0) shfd = sf.fd;
    *out_share = shfd; }
  return p;
}
/* ---- RGBA output: NV12 -> RGBA (BT.601) -> the mib3egl presenter's socket ---- */
extern int socket(int, int, int);
extern int connect(int, const void *, unsigned int);
extern int setsockopt(int, int, int, const void *, unsigned int);
static unsigned long g_aparse = 0, g_afeed = 0;
static int  g_rsock = -1;
static unsigned long g_rdrop = 0;
static unsigned long g_adrop = 0;
static unsigned long g_capret = 0;   /* capture buffers returned (flush or real) */
static unsigned long g_qfail  = 0;   /* output QBUF failures */
static char g_rsockpath[108];
static char g_in_sock[108];
static unsigned char *g_rgba = 0;

static void rgba_connect(void){
  struct { unsigned short fam; char path[108]; } sa; int i;
  if (g_rsock >= 0 || !g_rsockpath[0]) return;
  for (i=0;i<108;i++) sa.path[i]=0;
  for (i=0;i<107 && g_rsockpath[i]; i++) sa.path[i]=g_rsockpath[i];
  sa.fam = 1; /* AF_UNIX */
  g_rsock = socket(1, 1, 0);
  if (g_rsock < 0) return;
  if (connect(g_rsock, &sa, sizeof(sa)) != 0){ close(g_rsock); g_rsock = -1; return; }
  /* Bound how long a frame send can block us.  Each frame is w*h*4 = 2.4 MB and
   * the presenter drains at its own pace; without a timeout the decoder stops
   * reading the shim's AU socket for >200 ms -- the shim's SO_SNDTIMEO fires,
   * it drops the AU and reconnects, and the decoder is left decoding a
   * truncated stream (flat black).  60 ms is comfortably under the shim's
   * 200 ms, and the presenter resets its reassembly on client-gone, so a
   * partial frame is harmless -- we just drop that frame. */
  { struct { long s; long us; } tv; tv.s = 0; tv.us = 60000;
    setsockopt(g_rsock, 1 /*SOL_SOCKET*/, 21 /*SO_SNDTIMEO*/, &tv, sizeof(tv)); }
}
static int send_testpat(int w, int h){
  int x, y; long o;
  static unsigned long tp = 0;
  if (g_rsock < 0) rgba_connect();
  if (g_rsock < 0) return -1;
  if (!g_rgba){ g_rgba = (unsigned char*)malloc((unsigned long)w*h*4); if (!g_rgba) return -1; }
  for (y=0;y<h;y++){
    for (x=0;x<w;x++){
      o = ((long)y*w + x)*4;
      g_rgba[o]   = (unsigned char)((x*255)/(w-1));
      g_rgba[o+1] = (unsigned char)((y*255)/(h-1));
      g_rgba[o+2] = (unsigned char)(tp & 0xff);
      g_rgba[o+3] = 255;
    }
  }
  tp += 6;
  { long total = (long)w*h*4, off = 0;
    while (off < total){
      long wr = write(g_rsock, g_rgba+off, (unsigned long)(total-off));
      if (wr <= 0){ close(g_rsock); g_rsock = -1; g_rdrop++; return -1; }
      off += wr;
    } }
  return 0;
}

/* ---- AU capture (V4L2_DUMPAUS=<path>): record exactly what the shim sent us,
 * so a truncated/dropped stream can be proven offline. ---- */
static int g_au_fd = -1;
static unsigned long g_au_n = 0;
static int send_rgba(const unsigned char *nv12, int w, int h){
  const unsigned char *Y = nv12;
  const unsigned char *UV = nv12 + (long)w*h;
  int i, j, cw = w>>1;
  if (g_rsock < 0) rgba_connect();
  if (g_rsock < 0) return -1;
  if (!g_rgba){ g_rgba = (unsigned char*)malloc((unsigned long)w*h*4); if (!g_rgba) return -1; }
  for (j=0;j<h;j++){
    int cj = (j>>1)*cw;
    for (i=0;i<w;i++){
      int yy = Y[(long)j*w+i];
      int ci = (cj + (i>>1))*2;
      int uu = UV[ci], vv = UV[ci+1];
      int c = yy - 16, d = uu - 128, e = vv - 128;
      int r, g, b; long o;
      if (c < 0) c = 0;
      r = (298*c + 409*e + 128) >> 8;
      g = (298*c - 100*d - 208*e + 128) >> 8;
      b = (298*c + 516*d + 128) >> 8;
      o = ((long)j*w + i)*4;
      g_rgba[o]   = (unsigned char)(r<0?0:(r>255?255:r));
      g_rgba[o+1] = (unsigned char)(g<0?0:(g>255?255:g));
      g_rgba[o+2] = (unsigned char)(b<0?0:(b>255?255:b));
      g_rgba[o+3] = 255;
    }
  }
  { long total = (long)w*h*4, off = 0;
    while (off < total){
      long wr = write(g_rsock, g_rgba+off, (unsigned long)(total-off));
      if (wr <= 0){ close(g_rsock); g_rsock = -1; return -1; }
      off += wr;
    } }
  return 0;
}
static int g_ts = 0;          /* V4L2_TS=1 -> stamp output buffers (msm may require it) */
static long g_tsctr = 0;
static void setplane(struct v4l2_plane *p, unsigned long va, int fd, int shfd, u32 len, u32 used){
  p->bytesused = used; p->length = len;
  switch (g_fdfield){
    case 1: p->m.fd = fd; break;
    case 2: p->m.fd = shfd; break;
    case 3: p->reserved[0] = (u32)fd; p->m.userptr = va; break;
    case 4: p->data_offset = (u32)fd; p->m.userptr = va; break;
    case 5: p->reserved[1] = (u32)fd; p->m.userptr = va; break;
    case 6: p->reserved[0] = (u32)shfd; p->m.userptr = va; break;
    default:
      if (g_dmabuf) p->m.fd = fd;
      else if (g_shfd) p->m.fd = shfd;
      else if (g_ptrfd) p->m.fd = fd;
      else p->m.userptr = va;
  }
}

static int sfmt(int fd, u32 type, u32 w, u32 h, u32 fcc, u32 npl, u32 psz0, u32 pbp0, u32 psz1){
  struct v4l2_format f; int i, rc;
  for (i=0;i<(int)sizeof(f);i++) ((char*)&f)[i]=0;
  f.type = type; FMT_W(&f)=w; FMT_H(&f)=h; FMT_FCC(&f)=fcc; FMT_FLD(&f)=V4L2_FIELD_ANY;
  FMT_NPL(&f)=(unsigned char)npl;
  if (psz0) FMT_PSZ(&f,0)=psz0;
  if (pbp0) FMT_PBP(&f,0)=pbp0;
  if (npl>1 && psz1) FMT_PSZ(&f,1)=psz1;
  rc = ioctl(fd, VIDIOC_S_FMT, &f);
  P("  S_FMT type="); Pv(type); P(" "); Pv(w); P("x"); Pv(h);
  P(" fcc=0x"); { const char*hx="0123456789abcdef"; int q; for(q=0;q<8;q++) write(1,&hx[(FMT_FCC(&f)>>(28-4*q))&0xf],1); }
  P(" npl="); Pv(FMT_NPL(&f)); P(" sz0="); Pv(FMT_PSZ(&f,0)); P(" bp0="); Pv(FMT_PBP(&f,0));
  P(" sz1="); Pv(FMT_PSZ(&f,1));
  Prc(" rc", rc);
  return rc;
}

/* Queue the first OUTN access units into the output buffers.  Extracted so the
 * vendor's second pass can re-prime from AU 0 after it renegotiates the capture
 * queue (the renegotiate reclaims the output buffers we primed in the parse
 * pass; with none queued the driver never returns one and we would deadlock). */
static int prime_output(int fd, u32 memtype, void **outbuf, int *outfd,
                        int *outshare, unsigned long *outlen, long *aub,
                        const unsigned char *fdata, int nau, int *feed_i, int outn){
  int primed = 0, i, z;
  for (i=0;i<outn && *feed_i<nau;i++){
    struct v4l2_buffer b; struct v4l2_plane pl; u32 used; long lo, hi, j;
    lo = aub[*feed_i]; hi = aub[*feed_i+1]; used = (u32)(hi-lo);
    if (used > OUTBUF_SZ) used = OUTBUF_SZ;
    for (j=0;j<(long)used;j++) ((unsigned char*)outbuf[i])[j] = fdata[lo+j];
    for (z=0;z<(int)sizeof(b);z++) ((char*)&b)[z]=0;
    for (z=0;z<(int)sizeof(pl);z++) ((char*)&pl)[z]=0;
    setplane(&pl, (unsigned long)outbuf[i], outfd[i], outshare[i], (u32)outlen[i], used);
    b.index=(u32)i; b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b.memory=memtype;
    b.m.planes=&pl; b.length=1; b.bytesused=used;
    if (g_ts){ b.flags = 0x4000u; b.timestamp.tv_sec = 0; b.timestamp.tv_usec = (g_tsctr++) * 33333; }
    if (ioctl(fd, VIDIOC_QBUF, &b) != 0){ P("  QBUF out fail idx="); Pv(i);
      P(" used="); Pv((long)used); P(" errno="); Pv((long)ERRNO); P("\n"); break; }
    primed++; (*feed_i)++;
  }
  return primed;
}

/* ---- the VENDOR's SECOND SETUP PASS -------------------------------------
 * data/v4l2_vendor_ref_20261017.txt lines 138-152.  After the first input
 * buffer has been parsed the driver FLUSHES the whole capture queue (every
 * buffer comes back DONE with bytesused=0) and expects the client to
 * renegotiate it: the same REQBUFS(1) that granted 20 in the first pass grants
 * only 6 afterwards -- the driver now knows the stream's real DPB size.  Only
 * after that pass does the vendor see real frames (DONE|KEYFRAME on output,
 * DONE|PFRAME on capture).
 *
 * We never did this, which is fine for a stream whose first SPS matches the
 * format we already set, and fatal for one that makes the driver re-evaluate.
 * Gated by V4L2_RESTART=1 (default off: keep the proven path the default).
 * Returns the number of live capture buffers, or -1. */
static int vendor_restart(int fd, u32 memtype, void **capbuf, int *capfd,
                          int *capshare, u32 capsz0, u32 capsz1,
                          u32 w, u32 h, u32 capfcc, int want){
  struct v4l2_requestbuffers rb; int t, rc, i, z, n, okp = 0, okq = 0;
  P("--- vendor second setup pass (renegotiate capture) ---\n");
  t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  Prc("  STREAMOFF capture", ioctl(fd, VIDIOC_STREAMOFF, &t));
  for (z=0;z<(int)sizeof(rb);z++) ((char*)&rb)[z]=0;
  rb.count=0; rb.type=(u32)t; rb.memory=memtype;
  rc = ioctl(fd, VIDIOC_REQBUFS, &rb);
  P("  REQBUFS cap(0) rc="); Pv(rc); if (rc){ P(" errno="); Pv((long)ERRNO); } P("\n");
  for (z=0;z<(int)sizeof(rb);z++) ((char*)&rb)[z]=0;
  rb.count=(u32)want; rb.type=(u32)t; rb.memory=memtype;
  rc = ioctl(fd, VIDIOC_REQBUFS, &rb);
  if (rc != 0){                        /* a flush still in flight refuses this */
    int tryn;
    for (tryn=0; tryn<4 && rc!=0; tryn++){ usleep(50000);
      for (z=0;z<(int)sizeof(rb);z++) ((char*)&rb)[z]=0;
      rb.count=(u32)want; rb.type=(u32)t; rb.memory=memtype;
      rc = ioctl(fd, VIDIOC_REQBUFS, &rb); }
  }
  P("  REQBUFS cap ask="); Pv(want); P(" rc="); Pv(rc); if (rc){ P(" errno="); Pv((long)ERRNO); }
  P(" granted="); Pv((long)rb.count); P("\n");
  n = (int)rb.count;
  if (n <= 0 || n > CAPN_MAX){ P("  !! renegotiate gave no capture buffers\n"); return -1; }
  /* the vendor re-does the capture S_FMT here (G_FMT then S_FMT) */
  sfmt(fd, (u32)t, w, h, capfcc, 1, (u32)(w*h*3/2), w, 16384);
  for (i=0;i<n;i++){
    struct v4l2_buffer b; struct v4l2_plane pl[2];
    for (z=0;z<(int)sizeof(b);z++) ((char*)&b)[z]=0;
    for (z=0;z<(int)sizeof(pl);z++) ((char*)&pl)[z]=0;
    setplane(&pl[0], (unsigned long)capbuf[i], capfd[i], capshare[i], capsz0, 0);
    setplane(&pl[1], (unsigned long)capbuf[i]+(unsigned long)capsz0, capfd[i], capshare[i], capsz1, 0);
    b.index=(u32)i; b.type=(u32)t; b.memory=memtype; b.m.planes=pl; b.length=2;
    if (ioctl(fd, VIDIOC_PREPARE_BUF, &b) == 0) okp++;
  }
  Pk("  restart PREPARE_BUF ok", okp);
  Prc("  STREAMON capture", ioctl(fd, VIDIOC_STREAMON, &t));
  { struct v4l2_control c; c.id=0x99201a; c.value=2;
    Prc("  S_CTRL 0x99201a=2", ioctl(fd, VIDIOC_S_CTRL, &c)); }
  for (i=0;i<n;i++){
    struct v4l2_buffer b; struct v4l2_plane pl[2];
    for (z=0;z<(int)sizeof(b);z++) ((char*)&b)[z]=0;
    for (z=0;z<(int)sizeof(pl);z++) ((char*)&pl)[z]=0;
    setplane(&pl[0], (unsigned long)capbuf[i], capfd[i], capshare[i], capsz0, 0);
    setplane(&pl[1], (unsigned long)capbuf[i]+(unsigned long)capsz0, capfd[i], capshare[i], capsz1, 0);
    b.index=(u32)i; b.type=(u32)t; b.memory=memtype; b.m.planes=pl; b.length=2;
    if (ioctl(fd, VIDIOC_QBUF, &b) == 0) okq++;
  }
  Pk("  restart QBUF cap ok", okq);
  { struct v4l2_control c; c.id=0x99201a; c.value=0;
    Prc("  S_CTRL 0x99201a=0", ioctl(fd, VIDIOC_S_CTRL, &c)); }
  Pk("  restart live capture buffers", (long)n);
  return n;
}

int main(int argc, char **argv){
  const char *path = (argc>1)?argv[1]:"/tmp/kit/real.264";
  int maxchunk = (argc>2)?my_atoi(argv[2]):200;
  int do_probe = envnum("V4L2_PROBE", 1);
  int do_ctrl  = envnum("V4L2_CTRL", 1);
  int do_prep  = envnum("V4L2_PREPARE", 1);
  int CAPN = envnum("V4L2_CAPN", 20);
  int OUTN = envnum("V4L2_OUTN", 4);
  const char *dumprgba = getenv("V4L2_DUMP");
  const char *dumpnv12 = getenv("V4L2_NV12");
  int w = 1280, h = 480;
  int fd, ionfd, i, rc;
  u32 capsz0=0, capsz1=0, outsz=0;
  void *outbuf[OUTN_MAX], *capbuf[CAPN_MAX];
  unsigned long outlen[OUTN_MAX], caplen[CAPN_MAX];
  int outfd[OUTN_MAX], capfd[CAPN_MAX];
  int outshare[OUTN_MAX], capshare[CAPN_MAX];
  int src;
  u32 memtype = V4L2_MEMORY_USERPTR;
  u32 capfcc = envnum("V4L2_CAPFMT",0) ? V4L2_PIX_FMT_Q128 : V4L2_PIX_FMT_NV12;
  for (i=0;i<OUTN_MAX;i++){ outfd[i]=-1; outshare[i]=-1; }
  for (i=0;i<CAPN_MAX;i++){ capfd[i]=-1; capshare[i]=-1; }
  { char *hv = getenv("V4L2_HEAP");
    if (hv && hv[0]){ unsigned int r=0; int k=0;
      if (hv[0]=='0'&&(hv[1]=='x'||hv[1]=='X')){ k=2;
        for(;hv[k];k++){ int d; char c=hv[k];
          if(c>='0'&&c<='9')d=c-'0'; else if(c>='a'&&c<='f')d=c-'a'+10;
          else if(c>='A'&&c<='F')d=c-'A'+10; else break; r=r*16+(unsigned)d; } }
      else { for(;hv[k]>='0'&&hv[k]<='9';k++) r=r*10+(unsigned)(hv[k]-'0'); }
      g_heap = r; } }
  g_dmabuf = envnum("V4L2_DMABUF",0) ? 1 : 0;
  g_ptrfd  = envnum("V4L2_PTRFD",0) ? 1 : 0;
  g_shfd   = envnum("V4L2_SHFD",0) ? 1 : 0;
  g_fdfield = envnum("V4L2_FDFIELD",0);
  g_ts = envnum("V4L2_TS",1) ? 1 : 0;   /* ON by default: the vendor stamps output buffers */
  { const char *rs = getenv("V4L2_RGBA_SOCK");
    if (rs && rs[0]){ int k; for (k=0;k<107 && rs[k];k++) g_rsockpath[k]=rs[k]; g_rsockpath[k]=0; } }
  { const char *is = getenv("V4L2_IN_SOCK");
    if (is && is[0]){ int k; for (k=0;k<107 && is[k];k++) g_in_sock[k]=is[k]; g_in_sock[k]=0; } }
  { const char *da = getenv("V4L2_DUMPAUS");
    if (da && da[0]) g_au_fd = open(da, 0101 | 01000, 0644); }
  int loopfile = envnum("V4L2_LOOP",0) ? 1 : 0;
  int testpat  = envnum("V4L2_TESTPAT",0) ? 1 : 0;
  int do_restart = envnum("V4L2_RESTART",0) ? 1 : 0;
  { char *iv = getenv("V4L2_IONFLAG"); if (iv && iv[0]){ unsigned int r=0; int k=0;
      if (iv[0]=='0'&&(iv[1]=='x'||iv[1]=='X')){ k=2; for(;iv[k];k++){ int d; char c=iv[k]; if(c>='0'&&c<='9')d=c-'0'; else if(c>='a'&&c<='f')d=c-'a'+10; else if(c>='A'&&c<='F')d=c-'A'+10; else break; r=r*16+(unsigned)d; } }
      else { for(;iv[k]>='0'&&iv[k]<='9';k++) r=r*10+(unsigned)(iv[k]-'0'); }
      g_ionflags = r; } }
  if (g_dmabuf) memtype = 3 /* V4L2_MEMORY_DMABUF */;
  P("  heap=0x"); { const char*hx="0123456789abcdef"; int q; for(q=7;q>=0;q--) write(1,&hx[(g_heap>>(q*4))&0xf],1);} 
  P(" mem="); Pv((long)memtype); P("\n");

  if (CAPN > CAPN_MAX) CAPN = CAPN_MAX;
  if (OUTN > OUTN_MAX) OUTN = OUTN_MAX;

  P("v4l2_seq: file="); P(path); P("\n");

  { const char *is0 = getenv("V4L2_IN_SOCK"); int nbf = (is0 && is0[0]) ? 04000 /*O_NONBLOCK*/ : 0;
    fd = open("/dev/video32", 2 | nbf); }
  if (fd < 0){ P("FAIL open /dev/video32 errno="); Pv((long)ERRNO); P("\n"); return 1; }
  Pk("open /dev/video32 fd", fd);

  /* --- fast probe: which DECODER_CMD value does the driver accept? --- */
  if (argc>1 && argv[1][0]=='c' && argv[1][1]=='m'){
    unsigned long num = (3ul<<30)|(72ul<<16)|0x5600ul|96ul;
    int cv;
    for (cv=0; cv<=32; cv++){
      unsigned char buf[72]; int z; for(z=0;z<72;z++) buf[z]=0;
      *(u32*)buf = (u32)cv;
      rc = ioctl(fd, num, buf);
      P("  DECODER_CMD cmd="); Pv(cv); P(" rc="); Pv(rc); if (rc!=0){ P(" errno="); Pv((long)ERRNO); } P("\n");
      if (rc == 0) P("  ^^^ ACCEPTED\n");
    }
    close(fd); return 0;
  }

  /* --- subscribe to source-change events (the vendor does 9 of these) --- */
  { struct v4l2_event_subscription s; int k; u32 types[3]; types[0]=V4L2_EVENT_SOURCE_CHANGE; types[1]=8; types[2]=3;
    for (k=0;k<9;k++){ int z; u32 ty = types[k%3];
      for (z=0;z<(int)sizeof(s);z++) ((char*)&s)[z]=0;
      s.type = ty; ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &s); } }

  if (do_probe){
    /* --- vendor step 1: probe S_FMT 320x240 H264 --- */
    P("--- probe 320x240 ---\n");
    sfmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 320, 240, V4L2_PIX_FMT_H264, 1, 0, 0, 0);
  }
  if (do_ctrl){
    struct v4l2_control c; c.id = 0x99202e; c.value = 32784;
    Prc("  S_CTRL 0x99202e=32784", ioctl(fd, VIDIOC_S_CTRL, &c));
  }
  if (do_probe){
    sfmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 320, 240, V4L2_PIX_FMT_Q128, 2, 0, 0, 16384);
  }
  if (do_ctrl){
    struct v4l2_control c; int vals[6]; int k;
    c.id = 0x992007; c.value = 0; Prc("  S_CTRL 0x992007=0", ioctl(fd, VIDIOC_S_CTRL, &c));
    vals[0]=2; vals[1]=6; vals[2]=25; vals[3]=29; vals[4]=26; vals[5]=27;
    for (k=0;k<6;k++){ c.id=0x992019; c.value=vals[k];
      P("  S_CTRL 0x992019="); Pv(vals[k]); Prc(" rc", ioctl(fd, VIDIOC_S_CTRL, &c)); }
  }
  { struct v4l2_requestbuffers rb; int z;
    for (z=0;z<(int)sizeof(rb);z++) ((char*)&rb)[z]=0;
    rb.count=1; rb.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS cap(1,USERPTR) rc="); Pv(rc); P(" granted="); Pv(rb.count); P("\n");
    for (z=0;z<(int)sizeof(rb);z++) ((char*)&rb)[z]=0;
    rb.count=1; rb.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS out(1,USERPTR) rc="); Pv(rc); P(" granted="); Pv(rb.count); P("\n"); }

  /* --- vendor step: real formats at 1280x480 (sequence mirrors the strace) --- */
  sfmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, w, h, V4L2_PIX_FMT_H264, 1, 0, 0, 0);
  sfmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, w, h, V4L2_PIX_FMT_Q128, 1, 0, 0, 0);
  { struct v4l2_requestbuffers rb; int z;
    for (z=0;z<(int)sizeof(rb);z++) ((char*)&rb)[z]=0;
    rb.count=1; rb.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS cap(1a) rc="); Pv(rc); P(" granted="); Pv(rb.count); P("\n"); }
  sfmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, w, h, V4L2_PIX_FMT_H264, 1, 0, 0, 0);
  { struct v4l2_requestbuffers rb; int z;
    for (z=0;z<(int)sizeof(rb);z++) ((char*)&rb)[z]=0;
    rb.count=(u32)OUTN; rb.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS out rc="); Pv(rc); P(" granted="); Pv(rb.count); P("\n"); }
  sfmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, w, h, capfcc, 1, (u32)(w*h*3/2), (u32)w, 16384);
  { struct v4l2_requestbuffers rb; int z;
    for (z=0;z<(int)sizeof(rb);z++) ((char*)&rb)[z]=0;
    rb.count=1; rb.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS cap(1b) rc="); Pv(rc); P(" granted="); Pv(rb.count); P("\n"); }
  { struct v4l2_control c; c.id = 0x992007; c.value = 0; Prc("  S_CTRL 0x992007=0", ioctl(fd, VIDIOC_S_CTRL, &c)); }
  sfmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, w, h, capfcc, 1, (u32)(w*h*3/2), (u32)w, 16384);
  { struct v4l2_requestbuffers rb; int z;
    for (z=0;z<(int)sizeof(rb);z++) ((char*)&rb)[z]=0;
    rb.count=(u32)CAPN; rb.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS cap rc="); Pv(rc); P(" granted="); Pv(rb.count); P("\n");
    CAPN = (int)rb.count; }

  /* --- the driver forces 2 capture planes; read the negotiated sizes --- */
  { struct v4l2_format f; int z;
    for (z=0;z<(int)sizeof(f);z++) ((char*)&f)[z]=0;
    f.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_G_FMT, &f)==0){ capsz0=FMT_PSZ(&f,0); capsz1=FMT_PSZ(&f,1);
      Pk("  negotiated capsiz0", (long)capsz0); Pk("  negotiated capsiz1", (long)capsz1); Pk("  negotiated npl", FMT_NPL(&f)); } }
  if (capsz0 == 0) capsz0 = (u32)(w*h*3/2);
  if (capsz1 == 0) capsz1 = 16384;
  { struct v4l2_format f; int z;
    for (z=0;z<(int)sizeof(f);z++) ((char*)&f)[z]=0;
    f.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd, VIDIOC_G_FMT, &f)==0){ outsz=FMT_PSZ(&f,0);
      Pk("  negotiated outsz", (long)outsz); } }

  /* --- ION allocation --- */
  ionfd = open("/dev/ion", 2);
  if (ionfd < 0){ P("FAIL open /dev/ion errno="); Pv((long)ERRNO); P("\n"); return 1; }
  Pk("ion fd", ionfd);
  for (i=0;i<OUTN;i++){
    outbuf[i] = ion_alloc(ionfd, OUTBUF_SZ, &outlen[i], &outfd[i], &outshare[i]);
    if (!outbuf[i]) return 1;
  }
  /* capture buffers: one ION chunk, plane1 sits right after plane0 */
  for (i=0;i<CAPN;i++){
    capbuf[i] = ion_alloc(ionfd, (unsigned long)capsz0 + (unsigned long)capsz1, &caplen[i], &capfd[i], &capshare[i]);
    if (!capbuf[i]) return 1;
  }
  Pk("allocated out", OUTN); Pk("allocated cap", CAPN);
  { char b[64]; int k=0; const char*hx="0123456789abcdef"; unsigned long v=(unsigned long)capbuf[0];
    P("  capbuf[0]="); for(k=15;k>=0;k--) write(1,&hx[(v>>(k*4))&0xf],1);
    P(" capsz0="); Pv((long)capsz0); P(" capsz1="); Pv((long)capsz1); P("\n"); }

  /* --- PREPARE_BUF on every capture buffer (vendor step) --- */
  if (do_prep){
    int nprep = 0;
    for (i=0;i<CAPN;i++){
      struct v4l2_buffer b; struct v4l2_plane pl[2]; int z;
      for (z=0;z<(int)sizeof(b);z++) ((char*)&b)[z]=0;
      for (z=0;z<(int)sizeof(pl);z++) ((char*)&pl)[z]=0;
      setplane(&pl[0], (unsigned long)capbuf[i], capfd[i], capshare[i], capsz0, 0);
      setplane(&pl[1], (unsigned long)capbuf[i] + (unsigned long)capsz0, capfd[i], capshare[i], capsz1, 0);
      b.index=(u32)i; b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=memtype;
      b.m.planes=pl; b.length=2;
      rc = ioctl(fd, VIDIOC_PREPARE_BUF, &b);
      if (rc==0) nprep++;
    }
    Pk("  PREPARE_BUF ok", nprep);
  }

  /* --- STREAMON capture FIRST (vendor order), then the post-on control --- */
  { int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; Prc("  STREAMON capture", ioctl(fd, VIDIOC_STREAMON, &t)); }
  if (do_ctrl){ struct v4l2_control c; c.id=0x99201a; c.value=2;
    Prc("  S_CTRL 0x99201a=2", ioctl(fd, VIDIOC_S_CTRL, &c)); }

  /* --- QBUF all capture buffers (2 planes each) --- */
  { int n=0;
    for (i=0;i<CAPN;i++){
      struct v4l2_buffer b; struct v4l2_plane pl[2]; int z;
      for (z=0;z<(int)sizeof(b);z++) ((char*)&b)[z]=0;
      for (z=0;z<(int)sizeof(pl);z++) ((char*)&pl)[z]=0;
      setplane(&pl[0], (unsigned long)capbuf[i], capfd[i], capshare[i], capsz0, 0);
      setplane(&pl[1], (unsigned long)capbuf[i]+(unsigned long)capsz0, capfd[i], capshare[i], capsz1, 0);
      b.index=(u32)i; b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=memtype;
      b.m.planes=pl; b.length=2;
      rc=ioctl(fd, VIDIOC_QBUF, &b);
      if (rc==0) n++; else if (i<3){ P("  QBUF cap fail idx="); Pv(i); Prc(" rc", rc); }
    }
    Pk("  QBUF cap ok", n); }

  /* ================= ONLINE MODE: AUs from the shim's AF_UNIX socket ========
   * The shim (in carplay) connects and writes mib3helper.h frames:
   *   16-byte header {magic 0x4d334331 LE, u32 len LE, u32 seq LE, u32 pad}
   *   then `len` bytes of Annex-B access unit.
   * We decode each AU on the hw decoder and push RGBA to mib3egl's presenter. */
  if (g_in_sock[0]){
    int lfd, cfd = -1, nfree = 0, freeidx[OUTN_MAX], i2, restart_pending = 0;
    unsigned char *abuf; unsigned long alen = 0, acap = (2u<<20);
    long nframes = 0;
    struct { unsigned short fam; char path[108]; } sa;
    if (OUTN > OUTN_MAX) return 1;
    for (i2=0;i2<108;i2++) sa.path[i2]=0;
    for (i2=0;i2<107 && g_in_sock[i2];i2++) sa.path[i2]=g_in_sock[i2];
    sa.fam = 1;
    unlink(g_in_sock);
    lfd = socket(1, 1, 0);
    if (lfd < 0 || bind(lfd, &sa, sizeof(sa)) != 0 || listen(lfd, 4) != 0){ P("FAIL in-sock setup\n"); return 1; }
    chmod(g_in_sock, 0777);
    abuf = (unsigned char*)malloc(acap);
    if (!abuf) return 1;
    P("  online: listening on "); P(g_in_sock); P("\n");
    for (i2=0;i2<OUTN;i2++) freeidx[nfree++] = i2;
    { int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; Prc("  online STREAMON output", ioctl(fd, VIDIOC_STREAMON, &t)); }
    { int got_ev = 0, guard;
      for (guard=0; guard<20; guard++){
        unsigned char ev[128]; int z, r2;
        for (z=0;z<128;z++) ev[z]=0;
        r2 = ioctl(fd, VIDIOC_DQEVENT, ev);
        if (r2 == 0){ P("  online DQEVENT type="); Pv((long)(*(u32*)ev)); P("\n"); got_ev = 1; break; }
        usleep(10000);
      }
      if (got_ev){
        unsigned char d[72]; int z, r4;
        for (z=0;z<72;z++) d[z]=0; *(u32*)d = 4;
        r4 = ioctl(fd, VIDIOC_DECODER_CMD, d);
        P("  online DECODER_CMD rc="); Pv(r4); P("\n");
      } }
    if (do_restart){
      int nlive = vendor_restart(fd, memtype, capbuf, capfd, capshare,
                                 capsz0, capsz1, (u32)w, (u32)h, capfcc, CAPN);
      if (nlive > 0) Pk("  online restart live cap bufs", (long)nlive);
    }
    /* ---- the LIVE fix -----------------------------------------------------
     * Two defects were isolated by ablation on 2026-10-17 (see
     * data/ablate_results_20261017.txt):
     *   (a) the driver returns NOTHING AT ALL (0 capture buffers) until it is
     *       sent the flush command.  We used to send it only when the driver
     *       raised a DQEVENT, and on this stream it never does -> the chain was
     *       dead before it started.  The vendor sends it unconditionally after
     *       STREAMON.
     *   (b) the flush alone still yields 0 real frames: the capture port must be
     *       RE-NEGOTIATED after the decoder has ingested bitstream data
     *       (STREAMOFF -> REQBUFS(0) -> REQBUFS -> S_FMT -> PREPARE -> STREAMON
     *       -> S_CTRL 0x99201a=2 -> QBUF -> S_CTRL 0x99201a=0).  Only then does
     *       the hardware emit frames.
     * Both are done ONCE, when the first AU has been queued, matching the
     * vendor's ordering and its ~60 ms gap between STREAMON and the restart. */
    if (envnum("V4L2_ONLINE_FIX",1)) restart_pending = 1;
    for (;;){
      int did = 0;
      static int tpk = 0;
      /* TEST-PATTERN mode: prove the present half with no video source at all */
      if (testpat){
        if (++tpk >= 25){ tpk = 0; send_testpat(w, h); }
        usleep(4000);
        continue;
      }
      for (;;){
        struct v4l2_buffer b; struct v4l2_plane p1; int z3;
        for (z3=0;z3<(int)sizeof(b);z3++) ((char*)&b)[z3]=0;
        for (z3=0;z3<(int)sizeof(p1);z3++) ((char*)&p1)[z3]=0;
        b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b.memory=memtype; b.m.planes=&p1; b.length=1;
        if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) break;
        if (nfree < OUTN) freeidx[nfree++] = (int)b.index;
        did = 1;
      }
      for (;;){
        struct v4l2_buffer b; struct v4l2_plane pl[2]; int z, isreal;
        for (z=0;z<(int)sizeof(b);z++) ((char*)&b)[z]=0;
        for (z=0;z<(int)sizeof(pl);z++) ((char*)&pl)[z]=0;
        b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=memtype; b.m.planes=pl; b.length=2;
        if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) break;
        g_capret++;
        isreal = ((b.flags & 0x18) != 0) ? 1 : 0;
        if (g_capret <= 24 || isreal){
          P("  CAP n="); Pv((long)g_capret); P(" idx="); Pv((long)b.index); P(" flags=0x");
          { const char*hx="0123456789abcdef"; int q; for(q=7;q>=0;q--) write(1,&hx[(b.flags>>(q*4))&0xf],1); }
          P(" p0.used="); Pv((long)pl[0].bytesused); P(" real="); Pv((long)isreal); P("\n");
        }
        if (isreal){
          unsigned long lum = 0; int k = 0, i2;
          const unsigned char *YY = (const unsigned char*)capbuf[b.index];
          for (i2=0; i2 < w*h; i2 += 997){ lum += YY[i2]; k++; }
          lum = k ? (lum / (unsigned long)k) : 0;
          nframes++;
          if (nframes<=8 || (nframes%50)==0){
            P("  FRAME "); Pv(nframes); P(" luma="); Pv((long)lum); P(" drops="); Pv((long)g_rdrop); P(" auskip="); Pv((long)g_adrop); P("\n");
          }
          if (g_rsockpath[0]) send_rgba((const unsigned char*)capbuf[b.index], w, h);
        }
        { struct v4l2_buffer b2; struct v4l2_plane pl2[2]; int z2;
          for (z2=0;z2<(int)sizeof(b2);z2++) ((char*)&b2)[z2]=0;
          for (z2=0;z2<(int)sizeof(pl2);z2++) ((char*)&pl2)[z2]=0;
          setplane(&pl2[0], (unsigned long)capbuf[b.index], capfd[b.index], capshare[b.index], capsz0, 0);
          setplane(&pl2[1], (unsigned long)capbuf[b.index]+(unsigned long)capsz0, capfd[b.index], capshare[b.index], capsz1, 0);
          b2.index=b.index; b2.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b2.memory=memtype;
          b2.m.planes=pl2; b2.length=2;
          ioctl(fd, VIDIOC_QBUF, &b2); }
        did = 1;
      }
      if (cfd < 0){
        int c = accept(lfd, 0, 0);
        if (c >= 0){
          struct { long s; long us; } tv; tv.s = 0; tv.us = 20000;
          setsockopt(c, 1 /*SOL_SOCKET*/, 20 /*SO_RCVTIMEO*/, &tv, sizeof(tv));
          cfd = c; P("  online: shim CONNECTED\n");
          /* EVERY fresh connection needs the parse-flush + capture renegotiate:
           * a new decoder session starts from scratch (no reference frames), so
           * the fix has to re-arm here, not once per process. */
          if (envnum("V4L2_ONLINE_FIX",1)) restart_pending = 1;
        }
      }
      if (cfd >= 0){
        unsigned long space = acap - alen;
        long r = (space > 0) ? read(cfd, abuf + alen, space) : 0;
        if (r > 0){ alen += (unsigned long)r; did = 1; }
        else if (r == 0){
          P("  online: shim gone (alen="); Pv((long)alen); P(" space="); Pv((long)space);
          P(" nfree="); Pv((long)nfree); P(")\n");
          close(cfd); cfd = -1; alen = 0;
        }
      }
      { unsigned long off = 0;
        while (alen - off >= 16){
          unsigned magic = 0; unsigned long ln = 0; int k;
          for (k=0;k<4;k++) magic |= (unsigned)abuf[off+k] << (k*8);
          for (k=0;k<4;k++) ln |= (unsigned long)abuf[off+4+k] << (k*8);
          if (magic != 0x4d334331u || ln == 0 || ln > (16u<<20)){ off++; continue; }
          if (alen - off < 16 + ln) break;
          g_aparse++;
          if (g_au_fd >= 0 && g_au_n < 6000){ write(g_au_fd, abuf+off+16, ln); g_au_n++; }
          /* Never STALL on a full decoder queue: if no output buffer is free,
           * drop this AU and keep draining the socket.  Stalling left alen
           * growing to ~200 KB, which shrank our reads until the socket
           * back-pressured into the shim's 200 ms send timeout (the storm).
           * Dropping is safe here because the shim now stamps every AU with
           * the parameter sets and re-requests a keyframe on (re)connect. */
          if (nfree <= 0){ off += 16 + ln; g_adrop++; continue; }
          { int idx = freeidx[--nfree];
            struct v4l2_buffer b3; struct v4l2_plane pl3; int z4; unsigned long j, n2;
            n2 = (ln < OUTBUF_SZ) ? ln : OUTBUF_SZ;
            for (j=0;j<n2;j++) ((unsigned char*)outbuf[idx])[j] = abuf[off+16+j];
            for (z4=0;z4<(int)sizeof(b3);z4++) ((char*)&b3)[z4]=0;
            for (z4=0;z4<(int)sizeof(pl3);z4++) ((char*)&pl3)[z4]=0;
            setplane(&pl3, (unsigned long)outbuf[idx], outfd[idx], outshare[idx], (u32)outlen[idx], (u32)n2);
            b3.index=(u32)idx; b3.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b3.memory=memtype;
            b3.m.planes=&pl3; b3.length=1; b3.bytesused=(u32)n2;
            if (g_ts){ b3.flags = 0x4000u; b3.timestamp.tv_sec = 0; b3.timestamp.tv_usec = (g_tsctr++) * 33333; }
            if (ioctl(fd, VIDIOC_QBUF, &b3) == 0) g_afeed++;
            else { g_qfail++; freeidx[nfree++] = idx;    /* hand the buffer back */
              if (g_qfail <= 6){ P("  QBUF out FAIL idx="); Pv(idx); P(" len="); Pv((long)n2);
                                 P(" errno="); Pv((long)ERRNO); P("\n"); } } }
          off += 16 + ln;
        }
        if (off){ unsigned long k2; for (k2=0;k2<alen-off;k2++) abuf[k2]=abuf[off+k2]; alen -= off; }
      }
      if (restart_pending && g_afeed > 0){
        unsigned char d[72]; int z5, r5;
        restart_pending = 0;
        usleep(60000);                 /* let the driver ingest the first AU */
        for (z5=0;z5<72;z5++) d[z5]=0; *(u32*)d = 4;
        r5 = ioctl(fd, VIDIOC_DECODER_CMD, d);
        P("  online: first AU queued -> DECODER_CMD rc="); Pv(r5); P("\n");
        /* The driver answers the flush by handing back EVERY capture buffer
         * EMPTY (its "parse pass").  The vendor drains that completely BEFORE
         * it renegotiates -- and so must we: renegotiating with the flush still
         * in flight makes REQBUFS fail EINVAL, which destroys the capture queue
         * and kills the session (measured live 2026-10-17). */
        { int n = 0, spins;
          for (spins=0; spins<60; spins++){
            int got = 0;
            for (;;){
              struct v4l2_buffer b; struct v4l2_plane pl[2]; int z6;
              for (z6=0;z6<(int)sizeof(b);z6++) ((char*)&b)[z6]=0;
              for (z6=0;z6<(int)sizeof(pl);z6++) ((char*)&pl)[z6]=0;
              b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=memtype;
              b.m.planes=pl; b.length=2;
              if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) break;
              n++; got = 1;
            }
            if (n > 0 && !got) break;
            usleep(10000);
          }
          Pk("  online: drained capture flush", (long)n); }
        vendor_restart(fd, memtype, capbuf, capfd, capshare, capsz0, capsz1,
                       (u32)w, (u32)h, capfcc, CAPN);
      }
      if (!did) usleep(2000);
      { static unsigned long st = 0;
        if ((++st % 400) == 0){
          P("  STATUS parse="); Pv((long)g_aparse); P(" feed="); Pv((long)g_afeed);
          P(" qfail="); Pv((long)g_qfail); P(" capret="); Pv((long)g_capret);
          P(" skip="); Pv((long)g_adrop); P(" nfree="); Pv((long)nfree);
          P(" frames="); Pv((long)nframes); P(" alen="); Pv((long)alen); P("\n");
        } }
    }
    { int t; t=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; ioctl(fd, VIDIOC_STREAMOFF, &t);
             t=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  ioctl(fd, VIDIOC_STREAMOFF, &t); }
    close(fd);
    return 0;
  }

  int chunks = 0, feed_i = 0;
  unsigned char *fdata = 0; long fsz = 0; static long aub[131072]; int nau = 0;
  /* --- load the bitstream and split it into access units: the vendor feeds
   * ONE AU PER OUTPUT BUFFER, not one big file chunk. --- */
  {
    src = open(path, 0);
    if (src < 0){ P("FAIL open input errno="); Pv((long)ERRNO); P("\n"); return 1; }
    fdata = (unsigned char*)malloc(16u<<20);
    if (!fdata){ P("FAIL malloc\n"); return 1; }
    { long got=0; for(;;){ long r = read(src, fdata+got, (16u<<20)-got); if (r<=0) break; got += r; if (got >= (long)(16u<<20)) break; } fsz = got; }
    close(src);
    if (envnum("V4L2_WHOLEFILE",0)){
      aub[nau++] = 0; aub[nau] = fsz;
    } else {
      long k=0; int cur=0;
      while (k+3 < fsz && nau < 131071){
        if (fdata[k]==0 && fdata[k+1]==0 && fdata[k+2]==1){
          int t = fdata[k+3] & 0x1f; int begin = 0;
          if (t==7 || t==9) begin = 1; else if ((t==1 || t==5) && cur) begin = 1;
          if (begin){ aub[nau++] = k; cur = 0; }
          if (t==1 || t==5) cur = 1;
          k += 3; continue;
        }
        k++;
      }
      if (nau == 0) aub[nau++] = 0;
      aub[nau] = fsz;
    }
    Pk("  AUs", nau);
    /* prime the output queue with the first AUs */
    { int primed = prime_output(fd, memtype, outbuf, outfd, outshare, outlen,
                                aub, fdata, nau, &feed_i, OUTN);
      chunks = primed;
      Pk("  primed output", primed); }
  }
  { int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; Prc("  STREAMON output", ioctl(fd, VIDIOC_STREAMON, &t)); }
  /* The vendor, right after STREAMON(output), gets a SOURCE_CHANGE event and
   * answers it with a flush.  We only flush when an event actually arrives
   * (V4L2_DECCMD_ALWAYS=1 forces it).  A blind early flush returns all capture
   * buffers empty and muddies the picture. */
  { int got_ev = 0, guard;
    for (guard=0; guard<20; guard++){
      unsigned char ev[128]; int z, r2;
      for (z=0;z<128;z++) ev[z]=0;
      r2 = ioctl(fd, VIDIOC_DQEVENT, ev);
      if (r2 == 0){ P("  DQEVENT type="); Pv((long)(*(u32*)ev)); P("\n"); got_ev = 1; break; }
      usleep(10000);
    }
    if (got_ev || envnum("V4L2_DECCMD_ALWAYS",0)){
      unsigned char d[72]; int z, r4;
      for (z=0;z<72;z++) d[z]=0; *(u32*)d = 4;
      r4 = ioctl(fd, VIDIOC_DECODER_CMD, d);
      P("  DECODER_CMD cmd=4 rc="); Pv(r4); if (r4!=0){ P(" errno="); Pv((long)ERRNO); } P("\n");
    } else P("  (no DQEVENT -> no early flush)\n"); }

  /* V4L2_RESTART=1: replay the vendor's SECOND setup pass.  Its first pass
   * only parses headers (the driver flushes every capture buffer back empty);
   * the real decode happens only after the capture queue is renegotiated --
   * see data/v4l2_vendor_ref_20261017.txt lines 138-152. */
  if (do_restart){
    int drain = 0, got = 0;
    P("--- restart: absorbing the parse pass ---\n");
    for (;;){
      struct v4l2_buffer b; struct v4l2_plane pl[2]; int z;
      for (z=0;z<(int)sizeof(b);z++) ((char*)&b)[z]=0;
      for (z=0;z<(int)sizeof(pl);z++) ((char*)&pl)[z]=0;
      b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=memtype; b.m.planes=pl; b.length=2;
      if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) break;
      drain++;
      { struct v4l2_buffer b2; struct v4l2_plane pl2[2]; int z2;
        for (z2=0;z2<(int)sizeof(b2);z2++) ((char*)&b2)[z2]=0;
        for (z2=0;z2<(int)sizeof(pl2);z2++) ((char*)&pl2)[z2]=0;
        setplane(&pl2[0], (unsigned long)capbuf[b.index], capfd[b.index], capshare[b.index], capsz0, 0);
        setplane(&pl2[1], (unsigned long)capbuf[b.index]+(unsigned long)capsz0, capfd[b.index], capshare[b.index], capsz1, 0);
        b2.index=b.index; b2.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b2.memory=memtype;
        b2.m.planes=pl2; b2.length=2;
        ioctl(fd, VIDIOC_QBUF, &b2); }
    }
    Pk("  restart: absorbed capture flush", drain);
    for (;;){          /* the parse pass's primed output buffers come back too */
      struct v4l2_buffer b; struct v4l2_plane p1; int z3;
      for (z3=0;z3<(int)sizeof(b);z3++) ((char*)&b)[z3]=0;
      for (z3=0;z3<(int)sizeof(p1);z3++) ((char*)&p1)[z3]=0;
      b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b.memory=memtype; b.m.planes=&p1; b.length=1;
      if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) break;
      got++;
    }
    Pk("  restart: reclaimed output bufs", got);
    if (vendor_restart(fd, memtype, capbuf, capfd, capshare, capsz0, capsz1,
                       (u32)w, (u32)h, capfcc, CAPN) > 0){
      /* the new session has no reference frames: start again at AU 0 */
      feed_i = 0; chunks = 0;
      { int primed = prime_output(fd, memtype, outbuf, outfd, outshare, outlen,
                                  aub, fdata, nau, &feed_i, OUTN);
        chunks = primed;
        Pk("  restart: re-primed output", primed); }
    }
  }

  /* --- decode loop: feed every AU, drain capture, count only REAL frames -- */
  { int ncap=0, real=0, nout=0, loops=0, dumped=0, idle=0, flushed=0;
    for (loops=0; loops<20000; loops++){
      int did = 0;
      /* drain every ready capture buffer */
      for (;;){
        struct v4l2_buffer b; struct v4l2_plane pl[2]; int z, isreal;
        for (z=0;z<(int)sizeof(b);z++) ((char*)&b)[z]=0;
        for (z=0;z<(int)sizeof(pl);z++) ((char*)&pl)[z]=0;
        b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=memtype; b.m.planes=pl; b.length=2;
        rc = ioctl(fd, VIDIOC_DQBUF, &b);
        if (rc != 0) break;
        ncap++; did=1;
        isreal = ((b.flags & 0x18) != 0) ? 1 : 0;   /* KEYFRAME|PFRAME == decoded data (msm reports bytesused=0 even for real frames) */
        if (isreal) real++;
        if (ncap<=8 || (real>0 && real<=6) || (ncap%50)==0 || isreal){
          P("  CAP n="); Pv(ncap); P(" idx="); Pv((long)b.index); P(" flags=0x");
          { const char*hx="0123456789abcdef"; int q; for(q=7;q>=0;q--) write(1,&hx[(b.flags>>(q*4))&0xf],1); }
          P(" p0.used="); Pv((long)pl[0].bytesused); P(" real="); Pv((long)isreal); P("\n");
        }
        if (isreal){
          if (dumpnv12 && real <= 12){
            char pth[64]; int k=0, m, v; char numb[12];
            pth[k++]='/'; pth[k++]='t'; pth[k++]='m'; pth[k++]='p'; pth[k++]='/'; pth[k++]='f'; pth[k++]='r';
            m=0; v=real; if(!v) numb[m++]='0'; while(v){ numb[m++]=(char)('0'+v%10); v/=10; } while(m) pth[k++]=numb[--m];
            pth[k++]='.'; pth[k++]='n'; pth[k++]='v'; pth[k++]='1'; pth[k++]='2'; pth[k]=0;
            { int of=open(pth, 0101, 0644); if (of>=0){ write(of, capbuf[b.index], (unsigned long)capsz0); close(of); } }
            if (!dumped){ P("  wrote REAL NV12 frames\n"); dumped=1; }
          }
          if (g_rsockpath[0]) send_rgba((const unsigned char*)capbuf[b.index], w, h);
        }
        { struct v4l2_buffer b2; struct v4l2_plane pl2[2]; int z2;
          for (z2=0;z2<(int)sizeof(b2);z2++) ((char*)&b2)[z2]=0;
          for (z2=0;z2<(int)sizeof(pl2);z2++) ((char*)&pl2)[z2]=0;
          setplane(&pl2[0], (unsigned long)capbuf[b.index], capfd[b.index], capshare[b.index], capsz0, 0);
          setplane(&pl2[1], (unsigned long)capbuf[b.index]+(unsigned long)capsz0, capfd[b.index], capshare[b.index], capsz1, 0);
          b2.index=b.index; b2.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b2.memory=memtype;
          b2.m.planes=pl2; b2.length=2;
          ioctl(fd, VIDIOC_QBUF, &b2); }
      }
      /* reclaim spent output buffers and feed more */
      for (;;){
        struct v4l2_buffer b; struct v4l2_plane p1; int z3;
        for (z3=0;z3<(int)sizeof(b);z3++) ((char*)&b)[z3]=0;
        for (z3=0;z3<(int)sizeof(p1);z3++) ((char*)&p1)[z3]=0;
        b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b.memory=memtype; b.m.planes=&p1; b.length=1;
        rc = ioctl(fd, VIDIOC_DQBUF, &b);
        if (rc != 0) break;
        nout++; did=1;
        if (feed_i < nau){
          u32 used; long lo, hi, j;
          struct v4l2_buffer b3; struct v4l2_plane pl3; int z4;
          lo = aub[feed_i]; hi = aub[feed_i+1]; used = (u32)(hi-lo);
          if (used > OUTBUF_SZ) used = OUTBUF_SZ;
          for (j=0;j<(long)used;j++) ((unsigned char*)outbuf[b.index])[j] = fdata[lo+j];
          feed_i++; chunks++;
          if (loopfile && feed_i >= nau){ feed_i = 0; }
          for (z4=0;z4<(int)sizeof(b3);z4++) ((char*)&b3)[z4]=0;
          for (z4=0;z4<(int)sizeof(pl3);z4++) ((char*)&pl3)[z4]=0;
          setplane(&pl3, (unsigned long)outbuf[b.index], outfd[b.index], outshare[b.index], (u32)outlen[b.index], used);
          b3.index=b.index; b3.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b3.memory=memtype;
          b3.m.planes=&pl3; b3.length=1; b3.bytesused=used;
          if (g_ts){ b3.flags = 0x4000u; b3.timestamp.tv_sec = 0; b3.timestamp.tv_usec = (g_tsctr++) * 33333; }
          ioctl(fd, VIDIOC_QBUF, &b3);
        }
      }
      /* once every AU has been submitted, flush once to drain the decoder */
      if (!flushed && feed_i >= nau){
        unsigned char d[72]; int z; int r5;
        for (z=0;z<72;z++) d[z]=0; *(u32*)d = 4;
        r5 = ioctl(fd, VIDIOC_DECODER_CMD, d);
        P("  FLUSH (drain) rc="); Pv(r5); if (r5!=0){ P(" errno="); Pv((long)ERRNO); } P("\n");
        flushed = 1;
      }
      if (!did){ idle++; usleep(5000); } else idle = 0;
      if (flushed && idle > 400) break;
      if (idle > 3000) break;
    }
    P("---- result ----\n");
    Pk("output consumed", nout);
    Pk("capture returns", ncap);
    Pk("REAL FRAMES", real);
    P(real>0 ? "STAGE1: HARDWARE DECODE WORKS\n" : "STAGE1: no real frames decoded\n");
  }
  { int t; t=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; ioctl(fd, VIDIOC_STREAMOFF, &t);
           t=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  ioctl(fd, VIDIOC_STREAMOFF, &t); }
  close(fd); close(src);
  return 0;
}
