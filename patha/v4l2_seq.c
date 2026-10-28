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
 *        V4L2_NV12=<path>     arm the NV12 dump window (any value; "0" arms it)
 *        V4L2_NV12_AT=<n>     frame index the window starts at (default 0)
 *        V4L2_RGBA_SOCK=<path>  feed decoded RGBA to a presenter
 *        V4L2_IN_SOCK=<path>    online mode: read AUs from this socket
 *        V4L2_FPS=1     pace output submissions
 *   (There is no V4L2_DUMP: it was read and discarded, and has been removed.)
 *
 * Built -nostdlib against the unit's own libc, own _start (house recipe:
 * sh build.sh in this dir).
 * ==========================================================================*/
#define NULL ((void*)0)
#include "toolkit.h"
extern int   open(const char *, int, ...);
extern long  read(int, void *, unsigned long);
extern long  write(int, const void *, unsigned long);
extern long  send(int, const void *, unsigned long, int);
extern int   close(int);
extern int   ioctl(int, unsigned long, ...);
extern void  _exit(int);
extern void *mmap(void *, unsigned long, int, int, int, long);
extern char *getenv(const char *);
extern void *malloc(unsigned long);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern unsigned int usleep(unsigned int);
extern int bind(int, const void *, unsigned int);
extern int listen(int, int);
extern int accept(int, void *, unsigned int *);
extern int chmod(const char *, unsigned);
extern int unlink(const char *);
extern int getsockopt(int, int, int, void *, unsigned int *);
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
static void Pk(const char*t,long v){ P(t); P("="); Pn(v); P("\n"); }
extern int *__errno_location(void);
#define ERRNO (*__errno_location())
static void Prc(const char *t, int rc){ P(t); P(" rc="); Pn(rc); if(rc) { P(" errno="); Pn((long)ERRNO); } P("\n"); }
static int my_atoi(const char*s){ long v=0; while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return (int)v; }
static int envnum(const char*n, int d){ char*v=getenv(n); return v?v[0]?my_atoi(v):d:d; }

int main(int argc, char **argv);
void _start(void){ TOOL_ARGS(); }

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
  struct ion_alloc_ a; struct ion_fd_ f; int r; void *p;
  memset(&a,0,sizeof a);
  a.len = len; a.align = 4096; a.heap_mask = g_heap; a.flags = g_ionflags;
  r = ioctl(ionfd, ION_IOC_ALLOC, &a);
  if (r != 0){ P("ION alloc FAIL len="); Pn((long)len); P(" heap=0x"); { const char*hx="0123456789abcdef"; int q; for(q=7;q>=0;q--) write(1,&hx[(g_heap>>(q*4))&0xf],1);} P(" errno="); Pn((long)ERRNO); P("\n"); return 0; }
  memset(&f,0,sizeof f);
  f.handle = a.handle;
  r = ioctl(ionfd, ION_IOC_MAP, &f);
  if (r != 0){ P("ION map FAIL errno="); Pn((long)ERRNO); P("\n"); return 0; }
  p = mmap(NULL, len, 3, 1, f.fd, 0);
  if (p == (void*)-1){ P("ION mmap FAIL\n"); return 0; }
  if (out_len) *out_len = len;
  if (out_fd) *out_fd = f.fd;
  if (out_share){ struct ion_fd_ sf; int shfd = -1;
    memset(&sf,0,sizeof sf);
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
extern long time(long *);
struct my_tv2 { long s, us; };
extern int gettimeofday(struct my_tv2 *, void *);
static long g_send_ms = 0;                     /* ms: convert+write for the last frame */
static long t_out=0, t_cap=0, t_rd=0, t_feed=0, n_loop=0, g_tmark=0;   /* loop timing buckets */
static long now_ms(void){ struct my_tv2 t; gettimeofday(&t, 0); return t.s*1000 + t.us/1000; }
static long g_conns = 0;             /* helper connections accepted this process */
static long g_last_gone = 0;         /* time() the shim last disconnected */
static unsigned long g_kdrop = 0;    /* dropped: no keyframe / no sequence header yet */
static unsigned long g_qdrop = 0;    /* dropped: output queue full */
static char g_rsockpath[108];
static char g_in_sock[108];
static unsigned char *g_rgba = 0;
/* colour conversion for the RGBA feed: the phone's SPS says full range + BT.601,
 * so those are the defaults; both are overridable for a different stream. */
static int g_range_full = 1;   /* V4L2_RANGE_FULL=0 -> limited 16-235 */
static int g_matrix709  = 0;   /* V4L2_MATRIX_709=1 -> BT.709 coefficients */

/* ==========================================================================
 * 2026-10-20 (fps lane): THE TWO THINGS THAT WERE MISSING -- a HONEST phone
 * frame-rate measurement, and a drop policy that can be relaxed.
 *
 * (1) MEASUREMENT.  The cluster chain has three independent rates and only two
 *     of them were ever measured.  `parse` counts RECORDS ON THE AU SOCKET, and
 *     the shim writes TWO per phone frame -- the SPS/PPS record it stamps ahead
 *     of every AU (`sink_feed(g_cl_cfg_annexb, ...)` in mib3shim.c) and the AU
 *     itself.  So parse/2 is an artefact of the shim's framing, NOT a
 *     measurement of the phone (and parse/frames was once misread as a loss
 *     ratio).  A record that CARRIES A SLICE is exactly one phone frame: that is
 *     what `ausz` counts.  With it come the three numbers that decide WHY the
 *     phone is at the rate it is: the arrival-gap histogram (bursts vs a steady
 *     clock), the bytes/s (a bitrate budget is a flat bytes/s while the frame
 *     count moves), and the changed/same split of the DECODED pictures (a scene
 *     that is not changing is a sender that is not being asked to draw).
 *
 *     Plus `rgap`: the longest interval in which this loop did not call read()
 *     on the AU socket at all.  That matters because the PHONE drops frames when
 *     its own outgoing queue accumulates more than 67 ms of media duration
 *     (vdsink_ShouldDropFrame, CMTimeMake(67,1000) -- refs/rgi ALTScreen frame
 *     rate doc).  If our rgap ever approaches 67 ms, our loss becomes the
 *     phone's loss and no upstream knob can fix it.
 *
 * (2) POLICY.  `qlim` is how many RGBA frames may be outstanding to the
 *     presenter.  1 = today's behaviour exactly (drop unless the queue is
 *     empty).  2 = tolerate one frame in flight, which is what a BURST needs:
 *     the decode loop drains every ready capture buffer, so two frames can be
 *     decoded inside one presenter tick and the second used to be thrown away
 *     for a reason that had nothing to do with being unable to deliver it.
 *     The whole-frame invariant is UNCHANGED at every qlim: we only ever send a
 *     complete frame with MSG_DONTWAIT and drop it whole if the send is short.
 *     `qh0/qh1/qh2` count the queue depth seen at send time, so the policy can
 *     be judged without re-running with a different binary.
 * ======================================================================== */
static int g_qlim = 1;         /* V4L2_RGBA_QLIM   frames allowed in flight (1..4) */
static int g_stats = 1;        /* V4L2_STATS=0     silence the windowed RATE line  */
static int g_idle_us = 2000;   /* V4L2_IDLE_US     sleep when the loop did nothing */
static int g_besteffort = 0;   /* V4L2_BESTEFFORT=1 decode without a keyframe (OLD, black) */
static int g_live_ms = 250;    /* V4L2_LIVE_MS     liveness heartbeat, 0=off        */
static long g_framebytes = 0;  /* w*h*4, so rgba_connect can size SO_SNDBUF      */
static long g_sndbuf = 0;      /* SO_SNDBUF actually granted (read back, not asked for) */
static unsigned long g_rslice = 0;   /* AU records carrying a slice == PHONE FRAMES */
static unsigned long g_abytes = 0;   /* bitstream bytes of those records            */
static unsigned long g_amax   = 0;   /* biggest single AU                           */
static long g_ams = 0;               /* arrival ms of the last slice AU             */
static unsigned long g_gap[5];       /* arrival gaps, ms: <20 20-40 40-80 80-160 160+ */
static unsigned long g_sent = 0;     /* frames actually handed to the presenter      */
static unsigned long g_qh[3];        /* queue depth at send: 0 | <1 frame | >=1 frame */
static long g_rgap = 0, g_rast = 0;  /* longest interval between read() calls (ms)   */
static unsigned long g_changed = 0, g_same = 0;   /* decoded pictures: new vs repeat   */
static unsigned long g_sig = 0; static int g_have_sig = 0;
static long g_st_ms = 0; static unsigned long g_st_slice = 0;

/* a cheap signature of a decoded picture: enough to answer "did anything move?"
 * without touching the frame-rate path.  Sampled over the Y plane only. */
static unsigned long nv12_sig(const unsigned char *nv12, int w, int h){
  unsigned long s = 2166136261u; long n = (long)w*h, i;
  for (i = 0; i < n; i += 1024){ s = (s ^ (unsigned long)nv12[i]) * 16777619u; }
  return s;
}

/* One line per window.  Absolute totals: fps_probe.sh differences two samples,
 * which is why the numbers here are counters and not rates. */
static void stats_maybe(long nframes){
  long now;
  if (!g_stats) return;
  now = now_ms();
  if (!g_st_ms){ g_st_ms = now; g_st_slice = g_rslice; return; }
  if ((g_rslice - g_st_slice) < 128 && (now - g_st_ms) < 4000) return;
  g_st_ms = now; g_st_slice = g_rslice;
  P("  RATE ausz="); Pn((long)g_rslice);
  P(" abytes="); Pn((long)g_abytes);
  P(" amax="); Pn((long)g_amax);
  P(" gaps="); Pn((long)g_gap[0]); P(","); Pn((long)g_gap[1]); P(",");
  Pn((long)g_gap[2]); P(","); Pn((long)g_gap[3]); P(","); Pn((long)g_gap[4]);
  P(" sent="); Pn((long)g_sent);
  P(" drop="); Pn((long)g_rdrop);
  P(" qh0="); Pn((long)g_qh[0]);
  P(" qh1="); Pn((long)g_qh[1]);
  P(" qh2="); Pn((long)g_qh[2]);
  P(" rgap="); Pn(g_rgap);
  P(" changed="); Pn((long)g_changed);
  P(" same="); Pn((long)g_same);
  P(" nframes="); Pn(nframes);
  P("\n");
}

static void rgba_connect(void){
  struct { unsigned short fam; char path[108]; } sa; int i;
  if (g_rsock >= 0 || !g_rsockpath[0]) return;
  memset(sa.path,0,108);
  for (i=0;i<107 && g_rsockpath[i]; i++) sa.path[i]=g_rsockpath[i];
  sa.fam = 1; /* AF_UNIX */
  g_rsock = socket(1, 1, 0);
  if (g_rsock < 0) return;
  if (connect(g_rsock, &sa, sizeof(sa)) != 0){ close(g_rsock); g_rsock = -1; return; }
  /* The 60 ms SO_SNDTIMEO below is now INERT (the frame send is non-blocking --
   * see send_rgba), but it is kept so a future edit that re-introduces a blocking
   * write cannot stall the decode loop for more than 60 ms.  The 4 MB SO_SNDBUF
   * is what lets a whole 2.4 MB frame go out in one send(). */
  { struct { long s; long us; } tv; tv.s = 0; tv.us = 60000;
    setsockopt(g_rsock, 1 /*SOL_SOCKET*/, 21 /*SO_SNDTIMEO*/, &tv, sizeof(tv)); }
  /* A 2.4 MB RGBA frame over the default 208 KB socket buffer cannot be written
   * without blocking: the presenter only drains the socket once per render tick
   * (30 fps) => 208 KB*30/s = 6.2 MB/s => a 2.6 fps ceiling for 2.4 MB frames.
   * The decoder then blocks mid-send, stops reading the AU socket, the shim's
   * 200 ms timeout fires, AUs are dropped, the P-chain breaks and the cluster
   * garbles (measured 2026-10-18: 9 frames with the presenter vs 102 without).
   * Queue a whole frame instead, so presentation can NEVER throttle decoding. */
  { int sndbuf = 4 << 20;
    setsockopt(g_rsock, 1 /*SOL_SOCKET*/, 7 /*SO_SNDBUF*/, &sndbuf, sizeof(sndbuf)); }
  /* SIZE IT FOR THE POLICY, THEN READ IT BACK.
   *
   * The kernel clamps SO_SNDBUF to 2 * net.core.wmem_max.  On this unit
   * wmem_max was 208 KB (212992), so the 4 MB request became 425984 B and a
   * whole 2.4 MB frame could NEVER fit: every send() was short, the decoder
   * closed the socket by design, the presenter discarded its partial frame and
   * re-accepted -- a flat colour on the cluster while parse/feed/frames all
   * climbed (LOCKED_GOOD_2026-10-19 S3, 1477 connect/disconnect cycles).
   * `bring_up.sh` raises the CEILING; this read-back is how a log shows that it
   * did, and the WARN is the one line that would have explained that whole run
   * in a second.  With qlim > 1 the buffer must hold qlim frames, not one. */
  { long want = (long)(g_qlim > 0 ? g_qlim : 1)
              * (g_framebytes > 0 ? g_framebytes : (long)(1280*480*4));
    if (want < (4L << 20)) want = 4L << 20;
    if (want > (64L << 20)) want = 64L << 20;
    { int sb = (int)want;
      setsockopt(g_rsock, 1 /*SOL_SOCKET*/, 7 /*SO_SNDBUF*/, &sb, sizeof(sb)); }
    { int got = 0; unsigned int gl = (unsigned int)sizeof(got);
      if (getsockopt(g_rsock, 1, 7, &got, &gl) == 0){
        g_sndbuf = (long)got;      /* the DROP THRESHOLD is derived from this */
        P("  rgba sock sndbuf="); Pn((long)got); P(" want="); Pn(want);
        P(" qlim="); Pn((long)(g_qlim > 0 ? g_qlim : 1));
        P(" frame="); Pn((g_framebytes > 0 ? g_framebytes : (long)(1280*480*4))); P("\n");
        if ((long)got < want){
          P("  !! rgba sndbuf CLAMPED by net.core.wmem_max -- raise the CEILING:\n");
          P("  !!   echo 8388608 > /proc/sys/net/core/wmem_max   (bring_up.sh does it)\n");
          if ((long)got < (g_framebytes > 0 ? g_framebytes : (long)(1280*480*4)))
            P("  !! sndbuf < ONE FRAME: every send will be short. This is the flat-colour bug.\n");
        }
      } }
  }
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
  /* Same non-blocking, whole-frame discipline as send_rgba: the test pattern is
   * a diagnostic, but it must not introduce a blocking write loop either. */
  { long total = (long)w*h*4; long t0 = now_ms();
    { int q = 0;
      if (ioctl(g_rsock, 0x5411UL /*SIOCOUTQ*/, &q) == 0 && q > 0){ g_rdrop++; return -1; } }
    { long wr = send(g_rsock, g_rgba, (unsigned long)total, 0x40 /*MSG_DONTWAIT*/ | 0x4000 /*MSG_NOSIGNAL*/);
      if (wr != total){ close(g_rsock); g_rsock = -1; g_rdrop++; g_send_ms = now_ms()-t0; return -1; }
      g_send_ms = now_ms()-t0; } }
  return 0;
}

/* ---- AU capture (V4L2_DUMPAUS=<path>): record exactly what the shim sent us,
 * so a truncated/dropped stream can be proven offline.
 * NOTE (verified 2026-10-26): this writes EVERY record in arrival order -- the
 * SPS/PPS records the shim stamps before each AU land in the file TOO, so the
 * capture is a directly decodable Annex-B elementary stream (the earlier note
 * that said otherwise had it backwards). ---- */
static int g_au_fd = -1;
static unsigned long g_au_n = 0;

/* ---- REAL NV12 dumps (V4L2_NV12=1), with the AT-offset (2026-10-26) ---------
 * THE DEFECT THIS FIXES: the NV12 dump existed ONLY in the FILE-mode decode
 * loop.  The car runs the ONLINE loop, where V4L2_NV12 was a silent no-op -- so
 * the lane's planned artifact measurement ("start the decoder with
 * V4L2_NV12=1 and read /tmp/fr*.nv12") could not have produced a single byte
 * on any session so far.  It now runs in BOTH loops.
 * V4L2_NV12_AT=<N>: skip the first N real frames, then dump 12.  The artifacts
 * appear at GUIDANCE START (hundreds of frames in), and the old "first 12"
 * window only ever covered the clean connect.  Default 0 = the old window. */
static long g_nv12_at = 0;          /* set once at startup when V4L2_NV12 is on */
static void dump_nv12_frame(long frame_no, const unsigned char *buf, u32 capsz0){
  char pth[64]; int k=0, m, v; char numb[12];
  pth[k++]='/'; pth[k++]='t'; pth[k++]='m'; pth[k++]='p'; pth[k++]='/'; pth[k++]='f'; pth[k++]='r';
  m=0; v=(int)frame_no; if(!v) numb[m++]='0'; while(v){ numb[m++]=(char)('0'+v%10); v/=10; } while(m) pth[k++]=numb[--m];
  pth[k++]='.'; pth[k++]='n'; pth[k++]='v'; pth[k++]='1'; pth[k++]='2'; pth[k]=0;
  { int of=open(pth, 0101 /*O_WRONLY|O_CREAT*/, 0644);
    if (of>=0){ write(of, buf, (unsigned long)capsz0); close(of); } }
}
static int send_rgba(const unsigned char *nv12, int w, int h){
  const unsigned char *Y = nv12;
  const unsigned char *UV = nv12 + (long)w*h;
  int i, j;
  /* ---- RANGE AND MATRIX (fixed 2026-10-20) --------------------------------
   * The phone's own SPS says `video_full_range_flag = 1` and
   * `matrix_coefficients = 6` (BT.601).  This conversion used to hard-code
   * LIMITED range -- (Y-16)*1.164 -- so on a full-range signal every Y above
   * ~235 clipped to pure white: on a light-mode map (mostly near-white) the
   * cluster looked BLOWN OUT, and dark detail was crushed by the -16.
   * Measured on the car 2026-10-20 against the captured SPS, not guessed.
   *   V4L2_RANGE_FULL=0   limited range (16-235)   -- for a stream that says so
   *   V4L2_MATRIX_709=1   BT.709 coefficients     -- for an HD/709 stream
   * The DEFAULTS match this phone: full range, BT.601. */
  const int full = g_range_full;
  const int kY  = full ? 256 : 298;             /* luma scale                    */
  const int kYo = full ? 0   : 16;              /* luma offset (limited: 16)     */
  const int kR  = g_matrix709 ? (full ? 403 : 459) : (full ? 359 : 409);
  const int kG1 = g_matrix709 ? (full ? 48  : 55 ) : (full ? 88  : 100);
  const int kG2 = g_matrix709 ? (full ? 120 : 136) : (full ? 183 : 208);
  const int kB  = g_matrix709 ? (full ? 475 : 541) : (full ? 454 : 516);
  if (g_rsock < 0) rgba_connect();
  if (g_rsock < 0) return -1;
  if (!g_rgba){ g_rgba = (unsigned char*)malloc((unsigned long)w*h*4); if (!g_rgba) return -1; }
  /* FAST PATH: two pixels per chroma sample, packed 32-bit stores.
   * The original per-pixel version cost ~32 ms/frame (measured 2026-10-18),
   * which throttled the whole pipeline to ~4 fps. */
  { unsigned int *out = (unsigned int*)g_rgba;
    for (j = 0; j < h; j++){
      const unsigned char *yrow = Y + (long)j*w;
      const unsigned char *urow = UV + (long)(j>>1)*w;
      /* Rows are emitted TOP-FIRST (row 0 = top of the image).  That is the
       * natural contract for an RGBA frame; orientation is the RENDERER's job,
       * so the presenter's quad maps screen-top to v=0 (see QUAD in mib3egl.c).
       * Do not flip here. */
      unsigned long o = (unsigned long)j*w;
      for (i = 0; i < w; i += 2){
        int d = (int)urow[i] - 128, e = (int)urow[i+1] - 128;
        int dr = kR*e + 128, dg = -kG1*d - kG2*e + 128, db = kB*d + 128;
        int k, cy;
        for (k = 0; k < 2; k++){
          int r, g, b;
          cy = (int)yrow[i+k] - kYo; if (cy < 0) cy = 0; cy *= kY;
          r = (cy + dr) >> 8; g = (cy + dg) >> 8; b = (cy + db) >> 8;
          r = r < 0 ? 0 : (r > 255 ? 255 : r);
          g = g < 0 ? 0 : (g > 255 ? 255 : g);
          b = b < 0 ? 0 : (b > 255 ? 255 : b);
          out[o+i+k] = (unsigned int)r | ((unsigned int)g << 8)
                     | ((unsigned int)b << 16) | 0xff000000u;
        }
      }
    }
  }
  { long total = (long)w*h*4; long t0 = now_ms();
    /* DROP-IF-BEHIND, THEN NON-BLOCKING WHOLE-FRAME.
     *
     * SIOCOUTQ pre-check: only send when the presenter has drained the previous
     * frame (nothing queued).  That makes a PARTIAL frame IMPOSSIBLE -- with an
     * empty queue a full 2.4 MB frame fits in one send(), so we never write half
     * a frame for the presenter to (mis)reassemble.  A skipped frame is stale on
     * screen, never black, and never a torn frame.
     *
     * The non-blocking send() is the belt: even if the ioctl ever lied, a full
     * buffer makes send() return short/EAGAIN (never block), and we drop the
     * whole frame and close so the presenter resets its reassembly.
     *
     * WHY THIS MATTERS (2026-10-19): the OLD blocking write() stalled the decode
     * loop up to 60 ms/frame when the presenter fell behind (measured 198-400 ms
     * on the car).  While blocked, the loop stops reading the shim's AU socket,
     * the shim's 200 ms SO_SNDTIMEO fires, AUs are dropped, the P-chain breaks
     * and the cluster garbles.  A skipped frame is stale; a stalled decoder is a
     * dead pipeline.  MSG_NOSIGNAL keeps a dead presenter from raising SIGPIPE
     * (the decoder is its own process, but the belt is free). */
    { int q = 0;
      long frame = (long)w*h*4;
      long ql    = (long)(g_qlim > 0 ? g_qlim : 1);
      /* SIOCOUTQ IS NOT A PAYLOAD COUNT.  Measured on the bench (run_present
       * PART Q): after exactly ONE 2 457 600-byte frame was queued, SIOCOUTQ
       * read 2 497 536 -- 1.6 % MORE than the payload, because the kernel
       * accounts skb overhead.  So `q > (qlim-1)*frame` silently makes qlim=2
       * drop every time a single frame is in flight, i.e. qlim=2 would ship as
       * a no-op.  A slack term, 8x the measured overhead and far below one
       * frame, is what makes the knob mean what it says.  For qlim=1 the limit
       * stays exactly 0 -- the shipped policy is untouched. */
      long slack = frame >> 3;
      long lim   = (ql - 1) * frame + (ql > 1 ? slack : 0);
      /* *** THE THRESHOLD MUST FIT THE BUFFER (2026-10-25) ***
       *
       * `lim` used to be a pure multiple of the frame size, with no reference to
       * how much the socket can actually hold.  Measured on the car: sndbuf
       * 9 830 400 (four frames) while qlim=2 capped the queue at 1.125 frames,
       * so EVERY evaluation at depth 2 was discarded -- qh0 642 + qh1 1614 +
       * qh2 1012 = 3268 decoded frames, drops 1012 = the whole depth-2 bucket,
       * i.e. 31% of the picture thrown away while the buffer sat two thirds
       * empty.  A frame dropped here is not backpressure, it is a lost picture:
       * the surface simply shows an older frame for longer.
       *
       * So: the policy may only drop when the socket genuinely cannot take
       * another whole frame.  qlim stays the LATENCY cap (how many frames we are
       * willing to have in flight); this line makes it physically honest. */
      if (g_sndbuf > 0){
        long cap = g_sndbuf - frame - slack;
        if (cap < 0) cap = 0;
        if (lim > cap) lim = cap;
      }
      if (ioctl(g_rsock, 0x5411UL /*SIOCOUTQ*/, &q) == 0){
        /* Record the depth BEFORE deciding: qh0/qh1/qh2 measure the queue, not
         * the policy, so one run at qlim=1 tells us what qlim=2 or 3 would have
         * delivered.  qh1/qh2 are exactly the frames the qlim=1 policy throws
         * away.  The one-frame/frame-and-a-half boundary uses the same slack, so
         * the histogram agrees with the decision. */
        if (q <= 0) g_qh[0]++;
        else if ((long)q <= frame + slack) g_qh[1]++;
        else g_qh[2]++;
        /* qlim=1 -> lim=0 -> "drop unless the queue is empty": byte-for-byte the
         * shipped behaviour, so the default changes nothing. */
        if ((long)q > lim){ g_rdrop++; g_send_ms = 0; return -1; }
      } }
    long wr = send(g_rsock, g_rgba, (unsigned long)total, 0x40 /*MSG_DONTWAIT*/ | 0x4000 /*MSG_NOSIGNAL*/);
    if (wr != total){
      /* *** FINISH THE FRAME, DO NOT DISCARD IT (2026-10-25) ***
       *
       * A short non-blocking write used to close the socket, drop the frame and
       * force the presenter to discard whatever it had reassembled.  Measured on
       * the car: 24 such connect/disconnect cycles in one session ("rgba-sock
       * client gone -> re-accept" in egl.log).  Every one of them costs a frame
       * AND a presenter reassembly reset.
       *
       * The remainder is bounded: 100 ms is ~1.5 frame periods at 15 fps, so a
       * presenter that is merely behind gets the rest of the frame and the
       * picture stays whole; only a genuinely wedged reader is dropped-and-
       * reconnected, which is the case the old code assumed was the only one. */
      long off2 = (wr > 0) ? wr : 0;
      long tend = now_ms() + 100;
      while (off2 < total && now_ms() < tend){
        long w2 = send(g_rsock, (unsigned char*)g_rgba + off2,
                       (unsigned long)(total - off2),
                       0x40 /*MSG_DONTWAIT*/ | 0x4000 /*MSG_NOSIGNAL*/);
        if (w2 > 0) off2 += w2;
        else usleep(1000);
      }
      if (off2 < total){
        close(g_rsock); g_rsock = -1; g_rdrop++; g_send_ms = now_ms()-t0; return -1;
      }
    }
    g_send_ms = now_ms()-t0; g_sent++; }
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
  struct v4l2_format f; int rc;
  memset(&f,0,sizeof f);
  f.type = type; FMT_W(&f)=w; FMT_H(&f)=h; FMT_FCC(&f)=fcc; FMT_FLD(&f)=V4L2_FIELD_ANY;
  FMT_NPL(&f)=(unsigned char)npl;
  if (psz0) FMT_PSZ(&f,0)=psz0;
  if (pbp0) FMT_PBP(&f,0)=pbp0;
  if (npl>1 && psz1) FMT_PSZ(&f,1)=psz1;
  rc = ioctl(fd, VIDIOC_S_FMT, &f);
  P("  S_FMT type="); Pn(type); P(" "); Pn(w); P("x"); Pn(h);
  P(" fcc=0x"); { const char*hx="0123456789abcdef"; int q; for(q=0;q<8;q++) write(1,&hx[(FMT_FCC(&f)>>(28-4*q))&0xf],1); }
  P(" npl="); Pn(FMT_NPL(&f)); P(" sz0="); Pn(FMT_PSZ(&f,0)); P(" bp0="); Pn(FMT_PBP(&f,0));
  P(" sz1="); Pn(FMT_PSZ(&f,1));
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
  int primed = 0, i;
  for (i=0;i<outn && *feed_i<nau;i++){
    struct v4l2_buffer b; struct v4l2_plane pl; u32 used; long lo, hi, j;
    lo = aub[*feed_i]; hi = aub[*feed_i+1]; used = (u32)(hi-lo);
    if (used > OUTBUF_SZ) used = OUTBUF_SZ;
    for (j=0;j<(long)used;j++) ((unsigned char*)outbuf[i])[j] = fdata[lo+j];
    memset(&b,0,sizeof b);
    memset(&pl,0,sizeof pl);
    setplane(&pl, (unsigned long)outbuf[i], outfd[i], outshare[i], (u32)outlen[i], used);
    b.index=(u32)i; b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b.memory=memtype;
    b.m.planes=&pl; b.length=1; b.bytesused=used;
    if (g_ts){ b.flags = 0x4000u; b.timestamp.tv_sec = 0; b.timestamp.tv_usec = (g_tsctr++) * 33333; }
    if (ioctl(fd, VIDIOC_QBUF, &b) != 0){ P("  QBUF out fail idx="); Pn(i);
      P(" used="); Pn((long)used); P(" errno="); Pn((long)ERRNO); P("\n"); break; }
    primed++; (*feed_i)++;
  }
  return primed;
}

/* Classify an access unit: does it carry a slice? an IDR? (Annex-B scan) */
static void au_scan(const unsigned char *p, unsigned long n, int *has_slice, int *has_idr, int *has_sps){
  unsigned long k = 0; *has_slice = 0; *has_idr = 0; *has_sps = 0;
  while (k + 3 < n){
    if (p[k] == 0 && p[k+1] == 0 && p[k+2] == 1){
      unsigned t = p[k+3] & 0x1f;
      if (t == 1 || t == 5){ *has_slice = 1; if (t == 5) *has_idr = 1; }
      if (t == 7) *has_sps = 1;
      k += 3; continue;
    }
    k++;
  }
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
  struct v4l2_requestbuffers rb; int t, rc, i, n, okp = 0, okq = 0;
  P("--- vendor second setup pass (renegotiate capture) ---\n");
  t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  Prc("  STREAMOFF capture", ioctl(fd, VIDIOC_STREAMOFF, &t));
  memset(&rb,0,sizeof rb);
  rb.count=0; rb.type=(u32)t; rb.memory=memtype;
  rc = ioctl(fd, VIDIOC_REQBUFS, &rb);
  P("  REQBUFS cap(0) rc="); Pn(rc); if (rc){ P(" errno="); Pn((long)ERRNO); } P("\n");
  memset(&rb,0,sizeof rb);
  rb.count=(u32)want; rb.type=(u32)t; rb.memory=memtype;
  rc = ioctl(fd, VIDIOC_REQBUFS, &rb);
  if (rc != 0){                        /* a flush still in flight refuses this */
    int tryn;
    for (tryn=0; tryn<4 && rc!=0; tryn++){ usleep(50000);
      memset(&rb,0,sizeof rb);
      rb.count=(u32)want; rb.type=(u32)t; rb.memory=memtype;
      rc = ioctl(fd, VIDIOC_REQBUFS, &rb); }
  }
  P("  REQBUFS cap ask="); Pn(want); P(" rc="); Pn(rc); if (rc){ P(" errno="); Pn((long)ERRNO); }
  P(" granted="); Pn((long)rb.count); P("\n");
  n = (int)rb.count;
  if (n <= 0 || n > CAPN_MAX){ P("  !! renegotiate gave no capture buffers\n"); return -1; }
  /* the vendor re-does the capture S_FMT here (G_FMT then S_FMT) */
  sfmt(fd, (u32)t, w, h, capfcc, 1, (u32)(w*h*3/2), w, 16384);
  for (i=0;i<n;i++){
    struct v4l2_buffer b; struct v4l2_plane pl[2];
    memset(&b,0,sizeof b);
    memset(&pl,0,sizeof pl);
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
    memset(&b,0,sizeof b);
    memset(&pl,0,sizeof pl);
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
  
  int do_probe = envnum("V4L2_PROBE", 1);
  int do_ctrl  = envnum("V4L2_CTRL", 1);
  int do_prep  = envnum("V4L2_PREPARE", 1);
  int CAPN = envnum("V4L2_CAPN", 20);
  int OUTN = envnum("V4L2_OUTN", 4);
  const char *dumpnv12 = getenv("V4L2_NV12");
  { /* read the AT offset once; it only matters when the dump is on */
    const char *at = getenv("V4L2_NV12_AT");
    g_nv12_at = (dumpnv12 && dumpnv12[0] && at && at[0]) ? my_atoi(at) : 0;
    if (g_nv12_at < 0) g_nv12_at = 0; }
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
  /* colour: the phone's SPS (captured 2026-10-17, re-read 2026-10-20) says
   * video_full_range_flag=1, matrix_coefficients=6 (BT.601).  Defaults follow it. */
  g_range_full = envnum("V4L2_RANGE_FULL", 1) ? 1 : 0;
  g_matrix709  = envnum("V4L2_MATRIX_709", 0) ? 1 : 0;
  /* fps lane (2026-10-20).  qlim=1 is the shipped policy; stats are counters +
   * one line per window (no per-frame I/O) and are ON by default because a
   * one-command bring-up cannot retro-fit an env var into a running decoder,
   * and the whole point is that the next car session MEASURES the phone instead
   * of inferring it from a record count.  V4L2_STATS=0 restores a silent log. */
  /* DEFAULT 4, NOT 1 (2026-10-25).  `qlim` is the LATENCY cap -- how many frames
   * we are willing to have in flight -- and it now also bounds the drop
   * threshold against the real SO_SNDBUF (see send_rgba).  At the old default
   * the shrink to the buffer still left a 1.125-frame limit, so a presenter that
   * routinely runs 2 frames behind lost 31% of the picture to save ~66 ms of
   * latency on a map that gains nothing from it.  4 lets the observed depth
   * through: ~2.9 frames of slack, ~190 ms worst case at 15 fps. */
  g_qlim    = envnum("V4L2_RGBA_QLIM", 4);
  g_stats   = envnum("V4L2_STATS", 1) ? 1 : 0;
  g_idle_us = envnum("V4L2_IDLE_US", 2000);
  g_besteffort = envnum("V4L2_BESTEFFORT", 0) ? 1 : 0;
  g_live_ms = envnum("V4L2_LIVE_MS", 250);
  if (g_qlim < 1) g_qlim = 1; if (g_qlim > 4) g_qlim = 4;
  if (g_idle_us < 0) g_idle_us = 0;
  g_framebytes = (long)w * (long)h * 4;
  P("  fps knobs: qlim="); Pn((long)g_qlim); P(" stats="); P(g_stats ? "on" : "off");
  P(" idle_us="); Pn((long)g_idle_us); P(" live_ms="); Pn((long)g_live_ms);
  P(" besteffort="); P(g_besteffort ? "ON" : "off"); P("\n");
  P("  colour: range="); P(g_range_full ? "FULL (0-255)" : "limited (16-235)");
  P("  matrix="); P(g_matrix709 ? "BT.709" : "BT.601"); P("\n");
  int loopfile = envnum("V4L2_LOOP",0) ? 1 : 0;
  int testpat  = envnum("V4L2_TESTPAT",0) ? 1 : 0;
  int do_restart = envnum("V4L2_RESTART",0) ? 1 : 0;
  { char *iv = getenv("V4L2_IONFLAG"); if (iv && iv[0]){ unsigned int r=0; int k=0;
      if (iv[0]=='0'&&(iv[1]=='x'||iv[1]=='X')){ k=2; for(;iv[k];k++){ int d; char c=iv[k]; if(c>='0'&&c<='9')d=c-'0'; else if(c>='a'&&c<='f')d=c-'a'+10; else if(c>='A'&&c<='F')d=c-'A'+10; else break; r=r*16+(unsigned)d; } }
      else { for(;iv[k]>='0'&&iv[k]<='9';k++) r=r*10+(unsigned)(iv[k]-'0'); }
      g_ionflags = r; } }
  if (g_dmabuf) memtype = 3 /* V4L2_MEMORY_DMABUF */;
  P("  heap=0x"); { const char*hx="0123456789abcdef"; int q; for(q=7;q>=0;q--) write(1,&hx[(g_heap>>(q*4))&0xf],1);} 
  P(" mem="); Pn((long)memtype); P("\n");

  if (CAPN > CAPN_MAX) CAPN = CAPN_MAX;
  if (OUTN > OUTN_MAX) OUTN = OUTN_MAX;

  P("v4l2_seq: file="); P(path); P("\n");

  { const char *is0 = getenv("V4L2_IN_SOCK"); int nbf = (is0 && is0[0]) ? 04000 /*O_NONBLOCK*/ : 0;
    fd = open("/dev/video32", 2 | nbf); }
  if (fd < 0){ P("FAIL open /dev/video32 errno="); Pn((long)ERRNO); P("\n"); return 1; }
  Pk("open /dev/video32 fd", fd);

  /* --- fast probe: which DECODER_CMD value does the driver accept? --- */
  if (argc>1 && argv[1][0]=='c' && argv[1][1]=='m'){
    unsigned long num = (3ul<<30)|(72ul<<16)|0x5600ul|96ul;
    int cv;
    for (cv=0; cv<=32; cv++){
      unsigned char buf[72];  memset(buf,0,72);
      *(u32*)buf = (u32)cv;
      rc = ioctl(fd, num, buf);
      P("  DECODER_CMD cmd="); Pn(cv); P(" rc="); Pn(rc); if (rc!=0){ P(" errno="); Pn((long)ERRNO); } P("\n");
      if (rc == 0) P("  ^^^ ACCEPTED\n");
    }
    close(fd); return 0;
  }

  /* --- subscribe to source-change events (the vendor does 9 of these) --- */
  { struct v4l2_event_subscription s; int k; u32 types[3]; types[0]=V4L2_EVENT_SOURCE_CHANGE; types[1]=8; types[2]=3;
    for (k=0;k<9;k++){  u32 ty = types[k%3];
      memset(&s,0,sizeof s);
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
      P("  S_CTRL 0x992019="); Pn(vals[k]); Prc(" rc", ioctl(fd, VIDIOC_S_CTRL, &c)); }
  }
  { struct v4l2_requestbuffers rb; 
    memset(&rb,0,sizeof rb);
    rb.count=1; rb.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS cap(1,USERPTR) rc="); Pn(rc); P(" granted="); Pn(rb.count); P("\n");
    memset(&rb,0,sizeof rb);
    rb.count=1; rb.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS out(1,USERPTR) rc="); Pn(rc); P(" granted="); Pn(rb.count); P("\n"); }

  /* --- vendor step: real formats at 1280x480 (sequence mirrors the strace) --- */
  sfmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, w, h, V4L2_PIX_FMT_H264, 1, 0, 0, 0);
  sfmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, w, h, V4L2_PIX_FMT_Q128, 1, 0, 0, 0);
  { struct v4l2_requestbuffers rb; 
    memset(&rb,0,sizeof rb);
    rb.count=1; rb.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS cap(1a) rc="); Pn(rc); P(" granted="); Pn(rb.count); P("\n"); }
  sfmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, w, h, V4L2_PIX_FMT_H264, 1, 0, 0, 0);
  { struct v4l2_requestbuffers rb; 
    memset(&rb,0,sizeof rb);
    rb.count=(u32)OUTN; rb.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS out rc="); Pn(rc); P(" granted="); Pn(rb.count); P("\n"); }
  sfmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, w, h, capfcc, 1, (u32)(w*h*3/2), (u32)w, 16384);
  { struct v4l2_requestbuffers rb; 
    memset(&rb,0,sizeof rb);
    rb.count=1; rb.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS cap(1b) rc="); Pn(rc); P(" granted="); Pn(rb.count); P("\n"); }
  { struct v4l2_control c; c.id = 0x992007; c.value = 0; Prc("  S_CTRL 0x992007=0", ioctl(fd, VIDIOC_S_CTRL, &c)); }
  sfmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, w, h, capfcc, 1, (u32)(w*h*3/2), (u32)w, 16384);
  { struct v4l2_requestbuffers rb; 
    memset(&rb,0,sizeof rb);
    rb.count=(u32)CAPN; rb.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory=memtype;
    rc=ioctl(fd, VIDIOC_REQBUFS, &rb); P("  REQBUFS cap rc="); Pn(rc); P(" granted="); Pn(rb.count); P("\n");
    CAPN = (int)rb.count; }

  /* --- the driver forces 2 capture planes; read the negotiated sizes --- */
  { struct v4l2_format f; 
    memset(&f,0,sizeof f);
    f.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_G_FMT, &f)==0){ capsz0=FMT_PSZ(&f,0); capsz1=FMT_PSZ(&f,1);
      Pk("  negotiated capsiz0", (long)capsz0); Pk("  negotiated capsiz1", (long)capsz1); Pk("  negotiated npl", FMT_NPL(&f)); } }
  if (capsz0 == 0) capsz0 = (u32)(w*h*3/2);
  if (capsz1 == 0) capsz1 = 16384;
  { struct v4l2_format f; 
    memset(&f,0,sizeof f);
    f.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd, VIDIOC_G_FMT, &f)==0){ outsz=FMT_PSZ(&f,0);
      Pk("  negotiated outsz", (long)outsz); } }

  /* --- ION allocation --- */
  ionfd = open("/dev/ion", 2);
  if (ionfd < 0){ P("FAIL open /dev/ion errno="); Pn((long)ERRNO); P("\n"); return 1; }
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
  { int k=0; const char*hx="0123456789abcdef"; unsigned long v=(unsigned long)capbuf[0];
    P("  capbuf[0]="); for(k=15;k>=0;k--) write(1,&hx[(v>>(k*4))&0xf],1);
    P(" capsz0="); Pn((long)capsz0); P(" capsz1="); Pn((long)capsz1); P("\n"); }

  /* --- PREPARE_BUF on every capture buffer (vendor step) --- */
  if (do_prep){
    int nprep = 0;
    for (i=0;i<CAPN;i++){
      struct v4l2_buffer b; struct v4l2_plane pl[2]; 
      memset(&b,0,sizeof b);
      memset(&pl,0,sizeof pl);
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
      struct v4l2_buffer b; struct v4l2_plane pl[2]; 
      memset(&b,0,sizeof b);
      memset(&pl,0,sizeof pl);
      setplane(&pl[0], (unsigned long)capbuf[i], capfd[i], capshare[i], capsz0, 0);
      setplane(&pl[1], (unsigned long)capbuf[i]+(unsigned long)capsz0, capfd[i], capshare[i], capsz1, 0);
      b.index=(u32)i; b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=memtype;
      b.m.planes=pl; b.length=2;
      rc=ioctl(fd, VIDIOC_QBUF, &b);
      if (rc==0) n++; else if (i<3){ P("  QBUF cap fail idx="); Pn(i); Prc(" rc", rc); }
    }
    Pk("  QBUF cap ok", n); }

  /* ================= ONLINE MODE: AUs from the shim's AF_UNIX socket ========
   * The shim (in carplay) connects and writes mib3helper.h frames:
   *   16-byte header {magic 0x4d334331 LE, u32 len LE, u32 seq LE, u32 pad}
   *   then `len` bytes of Annex-B access unit.
   * We decode each AU on the hw decoder and push RGBA to mib3egl's presenter. */
  if (g_in_sock[0]){
    int lfd, cfd = -1, nfree = 0, freeidx[OUTN_MAX], i2, restart_pending = 0;
    unsigned char *pend; unsigned long cfg_len = 0; int seen_idr = 0, cfg_fed = 0; long kwait = 0;
    unsigned char *abuf; unsigned long alen = 0, acap = (2u<<20);
    long nframes = 0;
    /* BACKPRESSURE STATE (2026-10-25).  See the nfree<=0 site below.  Persists
     * across loop iterations on purpose: it times how long we have been waiting
     * for an output buffer, so the give-up is bounded. */
    long nbuf_wait = 0; unsigned long g_awt = 0;
    struct { unsigned short fam; char path[108]; } sa;
    if (OUTN > OUTN_MAX) return 1;
    memset(sa.path,0,108);
    for (i2=0;i2<107 && g_in_sock[i2];i2++) sa.path[i2]=g_in_sock[i2];
    sa.fam = 1;
    unlink(g_in_sock);
    lfd = socket(1, 1, 0);
    if (lfd < 0 || bind(lfd, &sa, sizeof(sa)) != 0 || listen(lfd, 4) != 0){ P("FAIL in-sock setup\n"); return 1; }
    chmod(g_in_sock, 0777);
    abuf = (unsigned char*)malloc(acap);
    if (!abuf) return 1;
    pend = (unsigned char*)malloc(8192);
    if (!pend) return 1;
    P("  online: listening on "); P(g_in_sock); P("\n");
    for (i2=0;i2<OUTN;i2++) freeidx[nfree++] = i2;
    { int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; Prc("  online STREAMON output", ioctl(fd, VIDIOC_STREAMON, &t)); }
    { int got_ev = 0, guard;
      for (guard=0; guard<20; guard++){
        unsigned char ev[128]; int r2;
        memset(ev,0,128);
        r2 = ioctl(fd, VIDIOC_DQEVENT, ev);
        if (r2 == 0){ P("  online DQEVENT type="); Pn((long)(*(u32*)ev)); P("\n"); got_ev = 1; break; }
        usleep(10000);
      }
      if (got_ev){
        unsigned char d[72]; int r4;
        memset(d,0,72); *(u32*)d = 4;
        r4 = ioctl(fd, VIDIOC_DECODER_CMD, d);
        P("  online DECODER_CMD rc="); Pn(r4); P("\n");
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
      g_tmark = now_ms();
      for (;;){
        struct v4l2_buffer b; struct v4l2_plane p1; 
        memset(&b,0,sizeof b);
        memset(&p1,0,sizeof p1);
        b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b.memory=memtype; b.m.planes=&p1; b.length=1;
        if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) break;
        if (nfree < OUTN) freeidx[nfree++] = (int)b.index;
        did = 1;
      }
      t_out += now_ms() - g_tmark; g_tmark = now_ms();
      for (;;){
        struct v4l2_buffer b; struct v4l2_plane pl[2]; int isreal;
        memset(&b,0,sizeof b);
        memset(&pl,0,sizeof pl);
        b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=memtype; b.m.planes=pl; b.length=2;
        if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) break;
        g_capret++;
        isreal = ((b.flags & 0x18) != 0) ? 1 : 0;
        if (g_capret <= 24 || isreal){
          P("  CAP n="); Pn((long)g_capret); P(" idx="); Pn((long)b.index); P(" flags=0x");
          { const char*hx="0123456789abcdef"; int q; for(q=7;q>=0;q--) write(1,&hx[(b.flags>>(q*4))&0xf],1); }
          P(" p0.used="); Pn((long)pl[0].bytesused); P(" real="); Pn((long)isreal); P("\n");
        }
        if (isreal){
          unsigned long lum = 0; int k = 0, i2;
          const unsigned char *YY = (const unsigned char*)capbuf[b.index];
          for (i2=0; i2 < w*h; i2 += 997){ lum += YY[i2]; k++; }
          lum = k ? (lum / (unsigned long)k) : 0;
          nframes++;
          /* REAL NV12 DUMP, ONLINE MODE TOO (2026-10-26).  See dump_nv12_frame:
           * the live path used to have no dump at all, and the "first 12"
           * window only ever covered the clean connect -- the artifacts start
           * at guidance.  V4L2_NV12_AT=<N> aims the 12-frame window there. */
          if (dumpnv12 && nframes > g_nv12_at && nframes <= g_nv12_at + 12){
            dump_nv12_frame(nframes, (const unsigned char*)capbuf[b.index], capsz0);
            if (nframes == g_nv12_at + 1){
              P("  NV12 dump: writing frames "); Pn(nframes); P(".."); Pn(nframes+11); P(" to /tmp/fr<N>.nv12\n");
            }
          }
          /* did the PICTURE change, or is the phone re-sending a still scene?
           * A sender whose scene is not being asked to redraw cannot be made to
           * send more by anything we advertise. */
          if (g_stats){
            unsigned long sg = nv12_sig((const unsigned char*)capbuf[b.index], w, h);
            if (g_have_sig){ if (sg == g_sig) g_same++; else g_changed++; }
            g_sig = sg; g_have_sig = 1;
          }
          if (nframes<=8 || (nframes%50)==0){
            P("  FRAME "); Pn(nframes); P(" luma="); Pn((long)lum); P(" drops="); Pn((long)g_rdrop); P(" auskip="); Pn((long)g_adrop);
            P(" send="); Pn(g_send_ms); P("ms\n");
          }
          if (g_rsockpath[0]) send_rgba((const unsigned char*)capbuf[b.index], w, h);
        }
        { struct v4l2_buffer b2; struct v4l2_plane pl2[2]; 
          memset(&b2,0,sizeof b2);
          memset(&pl2,0,sizeof pl2);
          setplane(&pl2[0], (unsigned long)capbuf[b.index], capfd[b.index], capshare[b.index], capsz0, 0);
          setplane(&pl2[1], (unsigned long)capbuf[b.index]+(unsigned long)capsz0, capfd[b.index], capshare[b.index], capsz1, 0);
          b2.index=b.index; b2.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b2.memory=memtype;
          b2.m.planes=pl2; b2.length=2;
          ioctl(fd, VIDIOC_QBUF, &b2); }
        did = 1;
      }
      t_cap += now_ms() - g_tmark; g_tmark = now_ms();
      if (cfd < 0){
        int c = accept(lfd, 0, 0);
        if (c >= 0){
          struct { long s; long us; } tv; tv.s = 0; tv.us = 1000;
          /* 1 ms, NOT 20 ms: this read is at the top of the loop and a blocking
           * 20 ms timeout meant ~50 ms per iteration (measured 2026-10-18),
           * which capped the whole pipeline at a few fps no matter how fast the
           * decoder or the presenter were. */
          setsockopt(c, 1 /*SOL_SOCKET*/, 20 /*SO_RCVTIMEO*/, &tv, sizeof(tv));
          cfd = c; P("  online: shim CONNECTED\n");
          g_rast = 0; g_ams = 0;   /* a new connection starts its own clocks */
          /* Do NOT reset the keyframe gate here.  A transient reconnect (the
           * phone's CarPlay link dropping -- measured at 8 connections and 7
           * disconnects in one session) does NOT invalidate the decoder's
           * reference frames, and resetting made us throw away every AU until
           * the phone happened to send another keyframe: kdrop=1749, one frame
           * decoded, then a frozen picture.  The gate is reset only when the
           * sequence header CHANGES (i.e. a genuinely new stream) -- see the
           * cfg-change detection in the parse loop. */
          /* Renegotiate the capture port for a genuinely NEW stream -- NOT for a
           * transient reconnect.  THE STORM (measured live 2026-10-18): the
           * renegotiate makes us stop reading the socket for ~100 ms, the shim's
           * 200 ms SO_SNDTIMEO fires, it drops the AU and reconnects, and with
           * per-connection renegotiation that becomes a storm -- auskip climbed
           * past 1200 and the decoded frames came out black, because the phone's
           * stream is P-slices only (one IDR at frame 1) and each restart throws
           * the reference frames away.  So: always on the first connection, and
           * afterwards only after a real gap (V4L2_RESTART_GAP seconds). */
          if (envnum("V4L2_ONLINE_FIX",1)){
            long now = time(0);
            long gap = (long)envnum("V4L2_RESTART_GAP", 0);
            /* FIRST CONNECTION ONLY (measured live 2026-10-18).  The
             * renegotiate is safe only while NOTHING has been fed to the
             * driver yet: it tears the capture queue down (STREAMOFF +
             * REQBUFS(0)), and doing that MID-STREAM wedges the output queue
             * for good -- the driver then returns no output buffers and no
             * capture buffers, feed freezes and every later AU is dropped
             * (feed=37, nfree=0, frames frozen).  The first-connection pass
             * works because the output queue is still empty (that is the run
             * that produced real frames).
             * So: one renegotiate per decoder PROCESS.  A new phone session
             * must get a NEW decoder process (restart it), not a second
             * renegotiate.  V4L2_RESTART_GAP=<secs> re-enables the old
             * gap-gated behaviour for experiments only. */
            if (g_conns == 0 || (gap > 0 && g_last_gone && (now - g_last_gone) >= gap)){
              restart_pending = 1;
              P("  online: FIRST CONNECTION -> renegotiate\n");
            } else P("  online: later connection -> feed as-is (do NOT renegotiate mid-stream)\n");
            g_conns++;
          }
        }
      }
      if (cfd >= 0){
        unsigned long space = acap - alen;
        long r;
        /* A record bigger than the assembly buffer used to look exactly like an
         * EOF: space reached 0, the read was skipped, `r` stayed 0, and the code
         * CLOSED the socket ("shim gone") and threw the whole stream away.  The
         * buffer is a hard 2 MB and the record limit is 16 MB, so the two
         * disagreed.  Grow instead, and never synthesise an EOF. */
        if (space < 65536 && acap < (32u << 20)){
          unsigned long nacap = acap << 1; unsigned char *nbuf; unsigned long z2;
          nbuf = (unsigned char*)malloc(nacap);
          if (nbuf){
            for (z2 = 0; z2 < alen; z2++) nbuf[z2] = abuf[z2];
            abuf = nbuf; acap = nacap; space = acap - alen;
            P("  online: assembly buffer grown to "); Pn((long)acap); P("\n");
          }
        }
        /* rgap: how long THIS LOOP went without even attempting to read the AU
         * socket.  The phone drops frames once its own outgoing queue holds
         * >67 ms of media duration, so a large rgap means our loss is being
         * charged to the phone as well. */
        { long tn = now_ms();
          if (g_rast && tn - g_rast > g_rgap) g_rgap = tn - g_rast;
          g_rast = tn; }
        r = (space > 0) ? read(cfd, abuf + alen, space) : -1;
        if (r > 0){ alen += (unsigned long)r; did = 1; }
        else if (r == 0){
          P("  online: shim gone (alen="); Pn((long)alen); P(" space="); Pn((long)space);
          P(" nfree="); Pn((long)nfree); P(")\n");
          close(cfd); cfd = -1; alen = 0; g_last_gone = time(0);
        }
      }
      t_rd += now_ms() - g_tmark; g_tmark = now_ms();
      { unsigned long off = 0;
        while (alen - off >= 16){
          unsigned magic = 0; unsigned long ln = 0; int k;
          for (k=0;k<4;k++) magic |= (unsigned)abuf[off+k] << (k*8);
          for (k=0;k<4;k++) ln |= (unsigned long)abuf[off+4+k] << (k*8);
          if (magic != 0x4d334331u || ln == 0 || ln > (16u<<20)){ off++; continue; }
          if (alen - off < 16 + ln) break;
          g_aparse++;
          if (g_au_fd >= 0 && g_au_n < 6000){ write(g_au_fd, abuf+off+16, ln); g_au_n++; }
          /* Assemble ONE access unit per output buffer, and never start on a
           * P-slice.  Two defects measured live 2026-10-18:
           *  (1) the shim sends the SPS/PPS as its OWN record, so the parameter
           *      sets arrived as a separate buffer AFTER the first IDR -- that
           *      IDR was parameter-set-less, the firmware could not decode it,
           *      it held every output buffer and the whole stream wedged
           *      (capret=0, nfree=0, feed frozen at 10).
           *  (2) a parameter-set-only record also breaks the vendor's "one AU
           *      per buffer" rule and wastes an output buffer.
           * The phone's GOP is ~65 frames (one IDR every ~6.5 s at 10 fps), and
           * the stream starts mid-GOP, so we must WAIT for a keyframe rather
           * than feed P-slices at a decoder that has no reference. */
          { const unsigned char *rp = abuf + off + 16; unsigned long rl = ln;
            int hs = 0, hi = 0, hsp = 0;
            au_scan(rp, rl, &hs, &hi, &hsp);
            if (hs){
              /* ONE PHONE FRAME.  Not one record: the shim writes the SPS/PPS as
               * its own record before every AU, so records = ~2 x frames and
               * parse/2 was never a rate.  This is the rate. */
              long tnow = now_ms();
              g_rslice++; g_abytes += rl; if (rl > g_amax) g_amax = rl;
              if (g_ams){
                long d = tnow - g_ams;
                if (d < 20) g_gap[0]++;
                else if (d < 40) g_gap[1]++;
                else if (d < 80) g_gap[2]++;
                else if (d < 160) g_gap[3]++;
                else g_gap[4]++;
              }
              g_ams = tnow;
            }
            if (!hs){                       /* parameter sets / SEI: remember them --
              * but only forward them when the sequence CHANGES.  Stamping the
              * SPS/PPS onto EVERY AU made the firmware re-parse the sequence
              * header per frame, fall behind, hold its output buffers and force
              * ~88% of frames to be dropped (parse=3204 feed=196, measured
              * live 2026-10-18) -- that was the artifacting. */
              int same = (cfg_len == rl); unsigned long q;
              if (same && rl){ for (q=0;q<rl;q++) if (pend[q] != rp[q]){ same = 0; break; } }
              if (!same){
                if (rl <= 8192){ for (q=0;q<rl;q++) pend[q] = rp[q]; cfg_len = rl; }
                else cfg_len = 0;
                cfg_fed = 0;                /* a new sequence header must go out */
                seen_idr = 0; kwait = 0;    /* ...and a new sequence needs its keyframe */
              }
              off += 16 + ln; continue;
            }
            /* first feed requires a keyframe AND its sequence header -- but never
             * wait for one forever: this phone emitted only 2 keyframes in 1761
             * slices (measured 2026-10-18), so a closed gate that waits for one
             * starves the picture indefinitely.  After 3 s, feed best-effort. */
            if (!seen_idr){
              if (cfg_len == 0 && !hsp){ off += 16 + ln; g_kdrop++; g_adrop++; continue; }  /* no parameter sets yet */
              if (!hi){
                /* WAIT FOR A REAL KEYFRAME (2026-10-24).  This used to give up after
                 * 3 s and "feed best-effort" -- which is exactly what put a BLACK
                 * frame on the cluster: a decoder fed P-frames with no reference
                 * outputs luma=16, and the controller then switched the cluster to
                 * that surface.  Measured on the car: 10 of 11 re-armed decoders
                 * took this path, and only the few that later happened to receive
                 * an IDR recovered (luma 194-233).  Dropping is honest -- the
                 * presenter's dead-man blank shows neutral, never garbage.
                 * V4L2_BESTEFFORT=1 restores the old behaviour for comparison. */
                long nw = time(0);
                if (!kwait) kwait = nw;
                if (!g_besteffort){
                  if ((nw - kwait) >= 5 && ((nw - kwait) % 5) == 0){
                    P("  online: still waiting for a keyframe, dropped "); Pn((long)g_kdrop); P(" AUs\n");
                  }
                  off += 16 + ln; g_kdrop++; g_adrop++; continue;
                }
                if ((nw - kwait) < 3){ off += 16 + ln; g_kdrop++; g_adrop++; continue; }
                P("  online: keyframe wait timed out -> feeding best-effort (V4L2_BESTEFFORT=1)\n");
              } else P("  online: keyframe WITH sequence header -> decoding\n");
              seen_idr = 1;               /* fall through and feed this AU */
            }
            /* *** NEVER DROP AN ACCESS UNIT (2026-10-25) ***
             *
             * This used to discard the AU whenever the output queue was full:
             *     if (nfree <= 0){ off += 16+ln; g_qdrop++; g_adrop++; continue; }
             *
             * That is unrecoverable damage, not backpressure.  Dropping an AU
             * means the hardware decoder never sees one picture in the reference
             * chain, so the next P-frame references a frame it does not have.
             * The phone sends about ONE keyframe per stream (2 IDRs in 1761
             * slices, measured 2026-10-18), so there is nothing to resync to and
             * the picture stays blocky for the rest of the session.
             *
             * MEASURED on the car 2026-10-25: qdrop climbed 186 -> 192 -> 196
             * across ~50-frame samples, i.e. ~12% of AUs discarded in steady
             * state, while the picture was permanently artifacted -- on BOTH the
             * current build and the older one that was believed clean.
             *
             * The buffer frees when a CAPTURE completes (nfree++ elsewhere in
             * this loop), and captures are drained at the TOP of each iteration,
             * so the correct move is to leave this record in `abuf` and try again
             * next iteration.  `break` exits the parse loop with `off` still
             * pointing at this record, so the compaction below keeps it intact
             * and nothing is double-fed.  `did = 0` makes the loop sleep instead
             * of spinning.  The give-up is bounded so a genuinely wedged dequeuer
             * cannot hang the stream forever. */
            if (nfree <= 0){
              long tn2 = now_ms();
              if (!nbuf_wait) nbuf_wait = tn2;
              if ((tn2 - nbuf_wait) < 500){ g_awt++; did = 0; break; }
              nbuf_wait = 0;                 /* 500 ms with no buffer: last resort */
              off += 16 + ln; g_qdrop++; g_adrop++; continue;
            }
            { int idx = freeidx[--nfree];
              nbuf_wait = 0;                 /* got one: reset the wait timer */
              struct v4l2_buffer b3; struct v4l2_plane pl3;  unsigned long j, used = 0, n2, pfx;
              pfx = cfg_fed ? 0 : cfg_len;
              for (j=0;j<pfx && used < OUTBUF_SZ;j++) ((unsigned char*)outbuf[idx])[used++] = pend[j];
              n2 = (rl < OUTBUF_SZ - used) ? rl : OUTBUF_SZ - used;
              for (j=0;j<n2;j++) ((unsigned char*)outbuf[idx])[used+j] = rp[j];
              used += n2;
              memset(&b3,0,sizeof b3);
              memset(&pl3,0,sizeof pl3);
              setplane(&pl3, (unsigned long)outbuf[idx], outfd[idx], outshare[idx], (u32)outlen[idx], (u32)used);
              b3.index=(u32)idx; b3.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b3.memory=memtype;
              b3.m.planes=&pl3; b3.length=1; b3.bytesused=(u32)used;
              if (g_ts){ b3.flags = 0x4000u; b3.timestamp.tv_sec = 0; b3.timestamp.tv_usec = (g_tsctr++) * 33333; }
              if (ioctl(fd, VIDIOC_QBUF, &b3) == 0){
                g_afeed++;
                if (pfx) cfg_fed = 1;
                if (hi){ if (!seen_idr) P("  online: first keyframe WITH sequence header -> decoding\n");
                         seen_idr = 1; }
                /* ONE AU PER OUTER ITERATION (2026-10-20).
                 * nfree (free OUTPUT framebuffers) is only refreshed at the top
                 * of the outer loop, but this inner while drains EVERYTHING one
                 * socket read returned.  So a read carrying N access units fed
                 * exactly nfree of them and discarded the rest via the
                 * `if (nfree <= 0) qdrop` branch -- measured on the car as
                 * qdrop=19 with OUTN=8 on the first read, and ONE dropped
                 * P-frame breaks the reference chain until the next keyframe
                 * (the phone sends ~1 per stream): first frame clean, everything
                 * after it artefacted for the rest of the session.
                 * Advancing and breaking here lets the reclaim loop run between
                 * frames, so nothing is thrown away for want of a buffer. */
                off += 16 + ln;
                break;
              }
              else { g_qfail++; freeidx[nfree++] = idx;
                if (g_qfail <= 6){ P("  QBUF out FAIL idx="); Pn(idx); P(" len="); Pn((long)used);
                                   P(" errno="); Pn((long)ERRNO); P("\n"); } } }
            }
          off += 16 + ln;
        }
        if (off){ unsigned long k2; for (k2=0;k2<alen-off;k2++) abuf[k2]=abuf[off+k2]; alen -= off; }
      }
      if (restart_pending && g_afeed > 0){
        unsigned char d[72]; int r5;
        restart_pending = 0;
        usleep(60000);                 /* let the driver ingest the first AU */
        memset(d,0,72); *(u32*)d = 4;
        r5 = ioctl(fd, VIDIOC_DECODER_CMD, d);
        P("  online: first AU queued -> DECODER_CMD rc="); Pn(r5); P("\n");
        /* The driver answers the flush by handing back EVERY capture buffer
         * EMPTY (its "parse pass").  The vendor drains that completely BEFORE
         * it renegotiates -- and so must we: renegotiating with the flush still
         * in flight makes REQBUFS fail EINVAL, which destroys the capture queue
         * and kills the session (measured live 2026-10-17). */
        { int n = 0, spins;
          for (spins=0; spins<60; spins++){
            int got = 0;
            for (;;){
              struct v4l2_buffer b; struct v4l2_plane pl[2]; 
              memset(&b,0,sizeof b);
              memset(&pl,0,sizeof pl);
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
      t_feed += now_ms() - g_tmark; n_loop++;
      if (!did) usleep((unsigned int)g_idle_us);
      stats_maybe(nframes);
      /* LIVENESS HEARTBEAT (2026-10-24).  The controller's stall detector compares
       * the frame counter against a 1.2 s hold, but the only line carrying that
       * counter was the STATUS block below -- every 200 loop iterations, measured
       * at ~10 ms/iter on the car, i.e. a 2-4 s resolution against a 1.2 s timeout.
       * A healthy stream therefore false-fired "frames stalled": 2026-10-24, the
       * controller saw frames=659 frozen while this decoder had already reached
       * 743, tore the session down and handed the cluster back to the car's map.
       * One short line every g_live_ms gives the hold ~5 samples.  Lowercase on
       * purpose: dec_first_frame() matches "FRAME " and must not see this. */
      if (g_live_ms > 0){
        static long live_ms = 0;
        long _ln = now_ms();
        if (live_ms == 0 || (_ln - live_ms) >= (long)g_live_ms){
          live_ms = _ln;
          P("  LIVE frames="); Pn((long)nframes); P("\n");
        }
      }
      { static unsigned long st = 0;
        if ((++st % 200) == 0){
          P("  STATUS parse="); Pn((long)g_aparse); P(" feed="); Pn((long)g_afeed);
          P(" qfail="); Pn((long)g_qfail); P(" capret="); Pn((long)g_capret);
          P(" kdrop="); Pn((long)g_kdrop); P(" qdrop="); Pn((long)g_qdrop);
          P(" awt="); Pn((long)g_awt);
          P(" frames="); Pn((long)nframes); P(" nfree="); Pn((long)nfree); P(" alen="); Pn((long)alen); P("\n");
          if (n_loop){ P("  TIMING/iter out="); Pn(t_out*1000/n_loop); P("us cap="); Pn(t_cap*1000/n_loop);
            P("us read="); Pn(t_rd*1000/n_loop); P("us feed="); Pn(t_feed*1000/n_loop); P("us  iters="); Pn(n_loop); P("\n"); }
        } }
    }
    { int t; t=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; ioctl(fd, VIDIOC_STREAMOFF, &t);
             t=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  ioctl(fd, VIDIOC_STREAMOFF, &t); }
    close(fd);
    return 0;
  }

  int feed_i = 0;
  unsigned char *fdata = 0; long fsz = 0; static long aub[131072]; int nau = 0;
  /* --- load the bitstream and split it into access units: the vendor feeds
   * ONE AU PER OUTPUT BUFFER, not one big file chunk. --- */
  {
    src = open(path, 0);
    if (src < 0){ P("FAIL open input errno="); Pn((long)ERRNO); P("\n"); return 1; }
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
      Pk("  primed output", primed); }
  }
  { int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; Prc("  STREAMON output", ioctl(fd, VIDIOC_STREAMON, &t)); }
  /* The vendor, right after STREAMON(output), gets a SOURCE_CHANGE event and
   * answers it with a flush.  We only flush when an event actually arrives
   * (V4L2_DECCMD_ALWAYS=1 forces it).  A blind early flush returns all capture
   * buffers empty and muddies the picture. */
  { int got_ev = 0, guard;
    for (guard=0; guard<20; guard++){
      unsigned char ev[128]; int r2;
      memset(ev,0,128);
      r2 = ioctl(fd, VIDIOC_DQEVENT, ev);
      if (r2 == 0){ P("  DQEVENT type="); Pn((long)(*(u32*)ev)); P("\n"); got_ev = 1; break; }
      usleep(10000);
    }
    if (got_ev || envnum("V4L2_DECCMD_ALWAYS",0)){
      unsigned char d[72]; int r4;
      memset(d,0,72); *(u32*)d = 4;
      r4 = ioctl(fd, VIDIOC_DECODER_CMD, d);
      P("  DECODER_CMD cmd=4 rc="); Pn(r4); if (r4!=0){ P(" errno="); Pn((long)ERRNO); } P("\n");
    } else P("  (no DQEVENT -> no early flush)\n"); }

  /* V4L2_RESTART=1: replay the vendor's SECOND setup pass.  Its first pass
   * only parses headers (the driver flushes every capture buffer back empty);
   * the real decode happens only after the capture queue is renegotiated --
   * see data/v4l2_vendor_ref_20261017.txt lines 138-152. */
  if (do_restart){
    int drain = 0, got = 0;
    P("--- restart: absorbing the parse pass ---\n");
    for (;;){
      struct v4l2_buffer b; struct v4l2_plane pl[2]; 
      memset(&b,0,sizeof b);
      memset(&pl,0,sizeof pl);
      b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=memtype; b.m.planes=pl; b.length=2;
      if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) break;
      drain++;
      { struct v4l2_buffer b2; struct v4l2_plane pl2[2]; 
        memset(&b2,0,sizeof b2);
        memset(&pl2,0,sizeof pl2);
        setplane(&pl2[0], (unsigned long)capbuf[b.index], capfd[b.index], capshare[b.index], capsz0, 0);
        setplane(&pl2[1], (unsigned long)capbuf[b.index]+(unsigned long)capsz0, capfd[b.index], capshare[b.index], capsz1, 0);
        b2.index=b.index; b2.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b2.memory=memtype;
        b2.m.planes=pl2; b2.length=2;
        ioctl(fd, VIDIOC_QBUF, &b2); }
    }
    Pk("  restart: absorbed capture flush", drain);
    for (;;){          /* the parse pass's primed output buffers come back too */
      struct v4l2_buffer b; struct v4l2_plane p1; 
      memset(&b,0,sizeof b);
      memset(&p1,0,sizeof p1);
      b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b.memory=memtype; b.m.planes=&p1; b.length=1;
      if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) break;
      got++;
    }
    Pk("  restart: reclaimed output bufs", got);
    if (vendor_restart(fd, memtype, capbuf, capfd, capshare, capsz0, capsz1,
                       (u32)w, (u32)h, capfcc, CAPN) > 0){
      /* the new session has no reference frames: start again at AU 0 */
      feed_i = 0;
      { int primed = prime_output(fd, memtype, outbuf, outfd, outshare, outlen,
                                  aub, fdata, nau, &feed_i, OUTN);
        Pk("  restart: re-primed output", primed); }
    }
  }

  /* --- decode loop: feed every AU, drain capture, count only REAL frames -- */
  { int ncap=0, real=0, nout=0, loops=0, idle=0, flushed=0;
    for (loops=0; loops<20000; loops++){
      int did = 0;
      /* drain every ready capture buffer */
      for (;;){
        struct v4l2_buffer b; struct v4l2_plane pl[2]; int isreal;
        memset(&b,0,sizeof b);
        memset(&pl,0,sizeof pl);
        b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=memtype; b.m.planes=pl; b.length=2;
        rc = ioctl(fd, VIDIOC_DQBUF, &b);
        if (rc != 0) break;
        ncap++; did=1;
        isreal = ((b.flags & 0x18) != 0) ? 1 : 0;   /* KEYFRAME|PFRAME == decoded data (msm reports bytesused=0 even for real frames) */
        if (isreal) real++;
        if (ncap<=8 || (real>0 && real<=6) || (ncap%50)==0 || isreal){
          P("  CAP n="); Pn(ncap); P(" idx="); Pn((long)b.index); P(" flags=0x");
          { const char*hx="0123456789abcdef"; int q; for(q=7;q>=0;q--) write(1,&hx[(b.flags>>(q*4))&0xf],1); }
          P(" p0.used="); Pn((long)pl[0].bytesused); P(" real="); Pn((long)isreal); P("\n");
        }
        if (isreal){
          /* the FILE-mode dump keeps the same window/behaviour, now via the
           * shared helper so both loops cannot drift apart again. */
          if (dumpnv12 && real > (int)g_nv12_at && real <= (int)g_nv12_at + 12)
            dump_nv12_frame(real, (const unsigned char*)capbuf[b.index], capsz0);
          if (g_rsockpath[0]) send_rgba((const unsigned char*)capbuf[b.index], w, h);
        }
        { struct v4l2_buffer b2; struct v4l2_plane pl2[2]; 
          memset(&b2,0,sizeof b2);
          memset(&pl2,0,sizeof pl2);
          setplane(&pl2[0], (unsigned long)capbuf[b.index], capfd[b.index], capshare[b.index], capsz0, 0);
          setplane(&pl2[1], (unsigned long)capbuf[b.index]+(unsigned long)capsz0, capfd[b.index], capshare[b.index], capsz1, 0);
          b2.index=b.index; b2.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b2.memory=memtype;
          b2.m.planes=pl2; b2.length=2;
          ioctl(fd, VIDIOC_QBUF, &b2); }
      }
      /* reclaim spent output buffers and feed more */
      g_tmark = now_ms();
      for (;;){
        struct v4l2_buffer b; struct v4l2_plane p1; 
        memset(&b,0,sizeof b);
        memset(&p1,0,sizeof p1);
        b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b.memory=memtype; b.m.planes=&p1; b.length=1;
        rc = ioctl(fd, VIDIOC_DQBUF, &b);
        if (rc != 0) break;
        nout++; did=1;
        if (feed_i < nau){
          u32 used; long lo, hi, j;
          struct v4l2_buffer b3; struct v4l2_plane pl3; 
          lo = aub[feed_i]; hi = aub[feed_i+1]; used = (u32)(hi-lo);
          if (used > OUTBUF_SZ) used = OUTBUF_SZ;
          for (j=0;j<(long)used;j++) ((unsigned char*)outbuf[b.index])[j] = fdata[lo+j];
          feed_i++;
          if (loopfile && feed_i >= nau){ feed_i = 0; }
          memset(&b3,0,sizeof b3);
          memset(&pl3,0,sizeof pl3);
          setplane(&pl3, (unsigned long)outbuf[b.index], outfd[b.index], outshare[b.index], (u32)outlen[b.index], used);
          b3.index=b.index; b3.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b3.memory=memtype;
          b3.m.planes=&pl3; b3.length=1; b3.bytesused=used;
          if (g_ts){ b3.flags = 0x4000u; b3.timestamp.tv_sec = 0; b3.timestamp.tv_usec = (g_tsctr++) * 33333; }
          ioctl(fd, VIDIOC_QBUF, &b3);
        }
      }
      /* once every AU has been submitted, flush once to drain the decoder */
      if (!flushed && feed_i >= nau){
        unsigned char d[72];  int r5;
        memset(d,0,72); *(u32*)d = 4;
        r5 = ioctl(fd, VIDIOC_DECODER_CMD, d);
        P("  FLUSH (drain) rc="); Pn(r5); if (r5!=0){ P(" errno="); Pn((long)ERRNO); } P("\n");
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
