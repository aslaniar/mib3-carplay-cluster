/* ============================================================================
 * v4l2_dec.c -- drive the unit's OWN hardware H.264 decoder (/dev/video32)
 *               directly, via the standard V4L2 memory-to-memory API.
 *
 * WHY: the vendor's Cinemo pipeline builds a decoder but never renders in a
 * standalone process (tracer: it sets up EGL, never swaps; it opens /dev/video32
 * only once the framework's `play` is emulated, then does no per-frame work).
 * /dev/video32 is `msm_vdec_8974` under `msm_vidc_driver` -- a standard V4L2 M2M
 * codec node -- so we can decode the phone's frames ourselves and hand the result
 * to the EGL presenter that already puts pictures on the cluster.
 *
 * STAGE 1 (this file): decode a .264 file end-to-end and PROVE frames come back
 *   (count them, print the negotiated capture format).  No display, no EGL.
 *   Usage: v4l2_dec <file.264> [max_chunks]
 *
 * STAGE 2 (later): export each decoded buffer as a dmabuf (VIDIOC_EXPBUF) and
 *   present it on ivi 3000.
 *
 * Built -nostdlib against the unit's own libc, own _start (the house recipe).
 * Every ioctl number is computed from the _IOC macros so they cannot drift.
 * ==========================================================================*/
#define NULL ((void*)0)
extern int   open(const char *, int, ...);
extern long  read(int, void *, unsigned long);
extern int   close(int);
extern int   ioctl(int, unsigned long, ...);
extern int   write(int, const void *, unsigned long);
extern void  _exit(int);
extern void *mmap(void *, unsigned long, int, int, int, long);
extern unsigned int sleep(unsigned int);
typedef unsigned int u32; typedef unsigned long u64;

#define IOC_NONE  0u
#define IOC_WRITE 1u
#define IOC_READ  2u
#define IOC(dir,type,nr,size) (((dir)<<30)|((size)<<16)|((type)<<8)|(nr))
#define V4L2_IOC_READ  2u
#define V4L2_IOC_WRITE 1u

/* ---- the structs (hand-declared; layouts match the kernel's own types) ---- */
struct v4l2_plane_pix_format { u32 sizeimage; u32 bytesperline; unsigned short reserved[6]; };
#define V4L2_MAX_PLANES 8
struct v4l2_pix_format_mplane {
  u32 width, height, pixelformat, field;
  struct v4l2_plane_pix_format plane_fmt[V4L2_MAX_PLANES];
  unsigned char num_planes, flags, reserved[10];
};
/* NOTE (hard-won): struct v4l2_format is 208 bytes total and its `fmt` union sits at
 * offset 8 (the union is 8-aligned because v4l2_window contains a pointer), so a
 * naive {u32 type; union{...}} puts every field 4 bytes early and the driver returns
 * garbage (G_FMT gave width 0 / fourcc 0x440) and rejects S_FMT with EINVAL.
 * We therefore keep the exact 208-byte ioctl buffer and address the union by byte
 * offset: body[0..199] is the union, pix_mp fields start at body+0. */
struct v4l2_format { u32 type; u32 rsvd; unsigned char body[200]; };
#define FMT_W(f)    (*(u32*)((f)->body + 0))
#define FMT_H(f)    (*(u32*)((f)->body + 4))
#define FMT_FCC(f)  (*(u32*)((f)->body + 8))
#define FMT_FLD(f)  (*(u32*)((f)->body + 12))
#define FMT_SZ(f)   (*(u32*)((f)->body + 16))
#define FMT_BPL(f)  (*(u32*)((f)->body + 20))
#define FMT_NPL(f)  (*((unsigned char*)((f)->body + 176)))
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

#define V4L2_BUF_TYPE_VIDEO_CAPTURE       1
#define V4L2_BUF_TYPE_VIDEO_OUTPUT        2
#define V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 9
#define V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE  10
#define V4L2_MEMORY_MMAP 1
#define V4L2_FIELD_NONE  1
#define V4L2_PIX_FMT_H264 0x34363248u   /* 'H264' */
#define V4L2_PIX_FMT_NV12 0x3231564eu   /* 'NV12' */

#define VIDIOC_S_FMT      IOC(3u, 'V', 5,  sizeof(struct v4l2_format))
#define VIDIOC_REQBUFS    IOC(3u, 'V', 8,  sizeof(struct v4l2_requestbuffers))
#define VIDIOC_QUERYBUF   IOC(3u, 'V', 9,  sizeof(struct v4l2_buffer))
#define VIDIOC_QBUF       IOC(3u, 'V', 15, sizeof(struct v4l2_buffer))
#define VIDIOC_DQBUF      IOC(3u, 'V', 17, sizeof(struct v4l2_buffer))
#define VIDIOC_STREAMON   IOC(1u, 'V', 18, 4)
#define VIDIOC_STREAMOFF  IOC(1u, 'V', 19, 4)

static unsigned slen(const char *s){ unsigned n=0; while (s && s[n]) n++; return n; }
static void P(const char *s){ write(1, s, slen(s)); }
extern int *__errno_location(void);
#define ERRNO (*__errno_location())
static void Pv(long v){ char b[24]; int i=0; if(v<0){write(1,"-",1);v=-v;} if(!v){write(1,"0",1);return;} while(v){b[i++]=(char)('0'+v%10);v/=10;} while(i) write(1,&b[--i],1); }
static void Pkv(const char*t,long v){ P(t); P("="); Pv(v); P("\n"); }
static void Perr(const char *t, int rc){ P(t); P(" rc="); Pv(rc); P(" errno="); Pv((long)ERRNO); P("\n"); }

int main(int argc, char **argv);
void _start(void){
  static char cbuf[512]; static char *av[16];
  int n=0, argc=0, i=0, fd=open("/proc/self/cmdline",0);
  if (fd>=0){ long r=read(fd,cbuf,(unsigned long)sizeof(cbuf)-1); if(r>0) n=(int)r; close(fd); }
  cbuf[n]=0;
  while (i<n && argc<15){ av[argc++]=&cbuf[i]; while (i<n && cbuf[i]) i++; i++; }
  av[argc]=0;
  { extern void _exit(int); _exit(main(argc,av)); }
}
static int my_atoi(const char*s){ long v=0; while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return (int)v; }

#define OUTF 8
#define CAPF 8
#define CHUNK (512*1024)

int main(int argc, char **argv){
  const char *path = (argc>1) ? argv[1] : "/tmp/test1280.264";
  int maxchunk = (argc>2) ? my_atoi(argv[2]) : 40;

  /* ---- PROBE MODE: which S_FMT combinations does this driver accept? ---- */
  if (argc > 1 && argv[1][0]=='p' && argv[1][1]=='r'){
    int fd0 = open("/dev/video32", 2);
    P("open fd="); Pv(fd0); P(" errno="); Pv((long)ERRNO); P("\n");
    if (fd0 >= 0){
      int tt[2] = { 10, 9 };
      int z;
      for (z = 0; z < 2; z++){
        struct v4l2_format g; int i3, r3;
        { char *p=(char*)&g; for(i3=0;i3<(int)sizeof(g);i3++)p[i3]=0; }
        g.type = (u32)tt[z];
        r3 = ioctl(fd0, IOC(3u,'V',4,sizeof(struct v4l2_format)) /*G_FMT*/, &g);
        P("G_FMT type="); Pv(tt[z]); P(" rc="); Pv(r3); P(" errno="); Pv((long)ERRNO);
        if (r3 == 0){ P("  got "); Pv(FMT_W(&g)); P("x"); Pv(FMT_H(&g));
          P(" fcc=0x"); { const char*hx="0123456789abcdef"; int q; for(q=0;q<8;q++) write(1,&hx[(FMT_FCC(&g)>>(28-4*q))&0xf],1); }
          P(" planes="); Pv(FMT_NPL(&g)); }
        P("\n");
      }
      close(fd0);
    }

    /* ---- REQBUFS matrix: which (type, memory, count) does the driver accept? ---- */
    { int fdr = open("/dev/video32", 2);
      struct v4l2_format ff; int i4;
      { char *q=(char*)&ff; for(i4=0;i4<(int)sizeof(ff);i4++)q[i4]=0; }
      ff.type = 10; FMT_W(&ff)=1280; FMT_H(&ff)=480; FMT_FCC(&ff)=0x34363248u;
      FMT_FLD(&ff)=1; FMT_NPL(&ff)=1; FMT_SZ(&ff)=524288;
      ioctl(fdr, VIDIOC_S_FMT, &ff);
      { char *q=(char*)&ff; for(i4=0;i4<(int)sizeof(ff);i4++)q[i4]=0; }
      ff.type = 9; FMT_W(&ff)=1280; FMT_H(&ff)=480; FMT_FCC(&ff)=0x3231564eu;
      FMT_FLD(&ff)=1; FMT_NPL(&ff)=1; FMT_SZ(&ff)=921600;
      ioctl(fdr, VIDIOC_S_FMT, &ff);
      { int ty[2] = { 10, 9 }; int me[3] = { 1, 2, 4 }; int cn[3] = { 2, 4, 8 };
        int a,b,c;
        for (a=0;a<2;a++) for (b=0;b<3;b++) for (c=0;c<3;c++){
          struct v4l2_requestbuffers rr; int r4;
          { char *q=(char*)&rr; for(r4=0;r4<(int)sizeof(rr);r4++)q[r4]=0; }
          rr.count=(u32)cn[c]; rr.type=(u32)ty[a]; rr.memory=(u32)me[b];
          r4 = ioctl(fdr, VIDIOC_REQBUFS, &rr);
          P("REQBUFS type="); Pv(ty[a]); P(" mem="); Pv(me[b]); P(" count="); Pv(cn[c]);
          P(" -> rc="); Pv(r4); P(" errno="); Pv((long)ERRNO); P(" granted="); Pv(rr.count); P("\n");
          if (r4 != 0) { /* reset between attempts */ struct v4l2_requestbuffers r0; int r5;
            { char *q=(char*)&r0; for(r5=0;r5<(int)sizeof(r0);r5++)q[r5]=0; }
            r0.count=0; r0.type=(u32)ty[a]; r0.memory=(u32)me[b]; ioctl(fdr, VIDIOC_REQBUFS, &r0); }
        } }
      close(fdr); }
    return 0;
  }


  /* ---- QBUF probe: which ion heap / ordering lets the driver take a buffer? ---- */
  if (argc > 1 && argv[1][0]=='q' && argv[1][1]=='b'){
    unsigned int heaps[6]; int nh=6, hi4;
    heaps[0]=0xffffffffu; heaps[1]=1u; heaps[2]=2u; heaps[3]=8u; heaps[4]=16u; heaps[5]=4u;
    for (hi4=0; hi4<nh; hi4++){
      int fq = open("/dev/video32", 2);
      int io = open("/dev/ion", 2);
      struct v4l2_format q; int r9, i9;
      struct ion_alloc_qa { unsigned long len, align; unsigned int heap_mask, flags; int handle; } a1, a2;
      struct ion_fd_qa { int handle, fd; } f1, f2;
      void *p1, *p2; unsigned long l1, l2;
      struct v4l2_requestbuffers rb2; struct v4l2_buffer b2; struct v4l2_plane pl2;
      if (fq < 0 || io < 0){ P("open fail\n"); return 1; }
      { char*q=(char*)&q; for(i9=0;i9<(int)sizeof(q);i9++)q[i9]=0; }
      q.type=10; FMT_W(&q)=1280; FMT_H(&q)=480; FMT_FCC(&q)=0x34363248u; FMT_FLD(&q)=1; FMT_NPL(&q)=1; FMT_SZ(&q)=262144;
      ioctl(fq, VIDIOC_S_FMT, &q);
      { char*q=(char*)&q; for(i9=0;i9<(int)sizeof(q);i9++)q[i9]=0; }
      q.type=9; FMT_W(&q)=1280; FMT_H(&q)=480; FMT_FCC(&q)=0x3231564eu; FMT_FLD(&q)=1; FMT_NPL(&q)=1; FMT_SZ(&q)=921600;
      ioctl(fq, VIDIOC_S_FMT, &q);
      /* plain anonymous page-aligned memory (MAP_PRIVATE|MAP_ANONYMOUS = 0x22):
       * if the driver can pin user pages this works and we skip ion entirely. */
      (void)a1;(void)a2;(void)f1;(void)f2;(void)io;(void)r9;
      l1=262144; l2=921600;
      p1=mmap(NULL,l1,3,0x22,-1,0);
      p2=mmap(NULL,l2,3,0x22,-1,0);
      P("   anon mmap out="); Pv((long)p1); P(" cap="); Pv((long)p2); P(" errno="); Pv((long)ERRNO); P("\n");
      P("--- heap_mask=0x"); { const char*hx="0123456789abcdef"; unsigned v=heaps[hi4]; int k9; for(k9=0;k9<8;k9++) write(1,&hx[(v>>(28-4*k9))&0xf],1); } P("\n");
      P("   ion alloc rc="); Pv(r9); P(" errno="); Pv((long)ERRNO); P("\n");
      { char*q=(char*)&rb2; for(i9=0;i9<(int)sizeof(rb2);i9++)q[i9]=0; }
      rb2.count=2; rb2.type=10; rb2.memory=2;
      P("   REQBUFS out rc="); Pv(ioctl(fq, VIDIOC_REQBUFS, &rb2)); P(" errno="); Pv((long)ERRNO); P("\n");
      { char*q=(char*)&rb2; for(i9=0;i9<(int)sizeof(rb2);i9++)q[i9]=0; }
      rb2.count=2; rb2.type=9; rb2.memory=2;
      P("   REQBUFS cap rc="); Pv(ioctl(fq, VIDIOC_REQBUFS, &rb2)); P(" errno="); Pv((long)ERRNO); P("\n");
      { char*q=(char*)&pl2; for(i9=0;i9<(int)sizeof(pl2);i9++)q[i9]=0; }
      pl2.bytesused=1000; pl2.length=l1; pl2.m.userptr=(unsigned long)p1;
      { char*q=(char*)&b2; for(i9=0;i9<(int)sizeof(b2);i9++)q[i9]=0; }
      b2.index=0; b2.type=10; b2.memory=2; b2.m.planes=&pl2; b2.length=1; b2.bytesused=1000;
      P("   QBUF out rc="); Pv(ioctl(fq, VIDIOC_QBUF, &b2)); P(" errno="); Pv((long)ERRNO); P("\n");
      { char*q=(char*)&pl2; for(i9=0;i9<(int)sizeof(pl2);i9++)q[i9]=0; }
      pl2.length=l2; pl2.m.userptr=(unsigned long)p2;
      { char*q=(char*)&b2; for(i9=0;i9<(int)sizeof(b2);i9++)q[i9]=0; }
      b2.index=0; b2.type=9; b2.memory=2; b2.m.planes=&pl2; b2.length=1;
      P("   QBUF cap rc="); Pv(ioctl(fq, VIDIOC_QBUF, &b2)); P(" errno="); Pv((long)ERRNO); P("\n");
      close(fq); close(io);
    }
    return 0;
  }


  /* ---- QBUF probe 2: what memory mode is the queue REALLY in? ---- */
  if (argc > 1 && argv[1][0]=='q' && argv[1][1]=='2'){
    int f2q = open("/dev/video32", 2);
    struct v4l2_format q; int r, i9;
    struct v4l2_requestbuffers rb2; struct v4l2_buffer b2; struct v4l2_plane pl2;
    void *anon;
    if (f2q < 0){ P("open fail\n"); return 1; }
    { char*q=(char*)&q; for(i9=0;i9<(int)sizeof(q);i9++)q[i9]=0; }
    q.type=10; FMT_W(&q)=1280; FMT_H(&q)=480; FMT_FCC(&q)=0x34363248u; FMT_FLD(&q)=1; FMT_NPL(&q)=1; FMT_SZ(&q)=262144;
    P("S_FMT out rc="); Pv(ioctl(f2q, VIDIOC_S_FMT, &q)); P(" errno="); Pv((long)ERRNO); P("\n");
    { char*q=(char*)&q; for(i9=0;i9<(int)sizeof(q);i9++)q[i9]=0; }
    q.type=9; FMT_W(&q)=1280; FMT_H(&q)=480; FMT_FCC(&q)=0x3231564eu; FMT_FLD(&q)=1; FMT_NPL(&q)=1; FMT_SZ(&q)=921600;
    P("S_FMT cap rc="); Pv(ioctl(f2q, VIDIOC_S_FMT, &q)); P(" errno="); Pv((long)ERRNO); P("\n");
    /* ask for USERPTR */
    { char*q=(char*)&rb2; for(i9=0;i9<(int)sizeof(rb2);i9++)q[i9]=0; }
    rb2.count=2; rb2.type=10; rb2.memory=2;
    r=ioctl(f2q, VIDIOC_REQBUFS, &rb2);
    P("REQBUFS(out,memory=2) rc="); Pv(r); P(" errno="); Pv((long)ERRNO); P(" granted="); Pv(rb2.count); P("\n");
    /* QUERYBUF with memory=MMAP: does the driver actually have MMAP buffers? */
    { char*q=(char*)&pl2; for(i9=0;i9<(int)sizeof(pl2);i9++)q[i9]=0; }
    { int mm; for (mm=2; mm>=1; mm--){
        { char*q=(char*)&pl2; for(i9=0;i9<(int)sizeof(pl2);i9++)q[i9]=0; }
        { char*q=(char*)&b2; for(i9=0;i9<(int)sizeof(b2);i9++)q[i9]=0; }
        b2.index=0; b2.type=10; b2.memory=(u32)mm; b2.m.planes=&pl2; b2.length=1;
        r=ioctl(f2q, VIDIOC_QUERYBUF, &b2);
        P("QUERYBUF(memory="); Pv(mm); P(") rc="); Pv(r); P(" errno="); Pv((long)ERRNO);
        P(" pl.len="); Pv((long)pl2.length); P(" bytesused="); Pv((long)pl2.bytesused);
        P(" mem_offset="); Pv((long)pl2.m.mem_offset); P(" userptr="); Pv((long)pl2.m.userptr);
        P(" bq.length="); Pv((long)b2.length); P(" bq.bytesused="); Pv((long)b2.bytesused); P("\n");
      } }
    /* QBUF as MMAP (index only) */
    { char*q=(char*)&pl2; for(i9=0;i9<(int)sizeof(pl2);i9++)q[i9]=0; }
    pl2.length=262144;
    { char*q=(char*)&b2; for(i9=0;i9<(int)sizeof(b2);i9++)q[i9]=0; }
    b2.index=0; b2.type=10; b2.memory=1; b2.m.planes=&pl2; b2.length=1;
    r=ioctl(f2q, VIDIOC_QBUF, &b2);
    P("QBUF(memory=1 MMAP) rc="); Pv(r); P(" errno="); Pv((long)ERRNO); P("\n");
    /* QBUF as USERPTR with anonymous memory */
    anon=mmap(NULL,262144,3,0x22,-1,0);
    { char*q=(char*)&pl2; for(i9=0;i9<(int)sizeof(pl2);i9++)q[i9]=0; }
    pl2.length=262144; pl2.m.userptr=(unsigned long)anon; pl2.bytesused=1000;
    { char*q=(char*)&b2; for(i9=0;i9<(int)sizeof(b2);i9++)q[i9]=0; }
    b2.index=0; b2.type=10; b2.memory=2; b2.m.planes=&pl2; b2.length=1;
    r=ioctl(f2q, VIDIOC_QBUF, &b2);
    P("QBUF(memory=2 USERPTR) rc="); Pv(r); P(" errno="); Pv((long)ERRNO); P("\n");
    /* what does G_FMT say the OUTPUT side wants now? */
    { char*q=(char*)&q; for(i9=0;i9<(int)sizeof(q);i9++)q[i9]=0; }
    q.type=10; r=ioctl(f2q, IOC(3u,'V',4,sizeof(struct v4l2_format)), &q);
    P("G_FMT out rc="); Pv(r); P(" sz="); Pv((long)FMT_SZ(&q)); P(" planes="); Pv((long)FMT_NPL(&q)); P("\n");
    close(f2q);
    return 0;
  }


  /* ---- QBUF probe 3: is it the buffer LENGTH the driver rejects? ---- */
  if (argc > 1 && argv[1][0]=='q' && argv[1][1]=='3'){
    unsigned long olen[3]; unsigned long clen[3];
    int a,b;
    olen[0]=262144; olen[1]=1048576; olen[2]=4194304;
    clen[0]=921600; clen[1]=4194304; clen[2]=8388608;
    for (a=0;a<3;a++){
      for (b=0;b<3;b++){
        int fq=open("/dev/video32",2); struct v4l2_format q; int i9,r;
        struct v4l2_requestbuffers rb2; struct v4l2_buffer b2; struct v4l2_plane pl2;
        void *p1,*p2;
        if (fq<0){ P("open fail\n"); return 1; }
        { char*q=(char*)&q; for(i9=0;i9<(int)sizeof(q);i9++)q[i9]=0; }
        q.type=10; FMT_W(&q)=1280;FMT_H(&q)=480;FMT_FCC(&q)=0x34363248u;FMT_FLD(&q)=1;FMT_NPL(&q)=1;FMT_SZ(&q)=(u32)olen[a];
        if (ioctl(fq,VIDIOC_S_FMT,&q)!=0){ P("S_FMT out fail\n"); close(fq); continue; }
        { char*q=(char*)&q; for(i9=0;i9<(int)sizeof(q);i9++)q[i9]=0; }
        q.type=9; FMT_W(&q)=1280;FMT_H(&q)=480;FMT_FCC(&q)=0x3231564eu;FMT_FLD(&q)=1;FMT_NPL(&q)=1;FMT_SZ(&q)=(u32)clen[b];
        ioctl(fq,VIDIOC_S_FMT,&q);
        { char*q=(char*)&rb2; for(i9=0;i9<(int)sizeof(rb2);i9++)q[i9]=0; }
        rb2.count=2; rb2.type=10; rb2.memory=2; ioctl(fq,VIDIOC_REQBUFS,&rb2);
        { char*q=(char*)&rb2; for(i9=0;i9<(int)sizeof(rb2);i9++)q[i9]=0; }
        rb2.count=2; rb2.type=9; rb2.memory=2; ioctl(fq,VIDIOC_REQBUFS,&rb2);
        p1=mmap(NULL,olen[a],3,0x22,-1,0);
        p2=mmap(NULL,clen[b],3,0x22,-1,0);
        { char*q=(char*)&pl2; for(i9=0;i9<(int)sizeof(pl2);i9++)q[i9]=0; }
        pl2.bytesused=1000; pl2.length=(u32)olen[a]; pl2.m.userptr=(unsigned long)p1;
        { char*q=(char*)&b2; for(i9=0;i9<(int)sizeof(b2);i9++)q[i9]=0; }
        b2.index=0; b2.type=10; b2.memory=2; b2.m.planes=&pl2; b2.length=(u32)olen[a];
        r=ioctl(fq,VIDIOC_QBUF,&b2);
        P("outlen="); Pv((long)olen[a]); P(" caplen="); Pv((long)clen[b]);
        P(" QBUF(out) rc="); Pv(r); P(" errno="); Pv((long)ERRNO);
        { char*q=(char*)&pl2; for(i9=0;i9<(int)sizeof(pl2);i9++)q[i9]=0; }
        pl2.length=(u32)clen[b]; pl2.m.userptr=(unsigned long)p2;
        { char*q=(char*)&b2; for(i9=0;i9<(int)sizeof(b2);i9++)q[i9]=0; }
        b2.index=0; b2.type=9; b2.memory=2; b2.m.planes=&pl2; b2.length=(u32)clen[b];
        r=ioctl(fq,VIDIOC_QBUF,&b2);
        P("  QBUF(cap) rc="); Pv(r); P(" errno="); Pv((long)ERRNO); P("\n");
        close(fq);
      }
    }
    return 0;
  }

  int fd, rc, i, nout = 0, ncap = 0, chunks = 0, submitted = 0;
  int ionfd = -1;
  unsigned long capsiz = 0;
  int outfd[OUTF], capfd[CAPF];
  (void)outfd; (void)capfd;
  int w = 1280, h = 480;
  struct v4l2_format f;
  struct v4l2_requestbuffers rb;
  struct v4l2_buffer bq;
  void *outbuf[OUTF], *capbuf[CAPF];
  unsigned long outlen[OUTF], caplen[CAPF];
  int ofd, src;

  Pkv("sizeof v4l2_buffer", (long)sizeof(struct v4l2_buffer));
  Pkv("sizeof v4l2_format", (long)sizeof(struct v4l2_format));
  Pkv("sizeof requestbuffers", (long)sizeof(struct v4l2_requestbuffers));

  fd = open("/dev/video32", 2);
  if (fd < 0){ P("FAIL open /dev/video32\n"); return 1; }
  Pkv("open /dev/video32 fd", fd);

  /* --- OUTPUT (compressed) format: H.264, 1 plane --- */
  { char *p = (char*)&f; for (i=0;i<(int)sizeof(f);i++) p[i]=0; }
  f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  FMT_W(&f) = w; FMT_H(&f) = h;
  FMT_FCC(&f) = V4L2_PIX_FMT_H264;
  FMT_FLD(&f) = V4L2_FIELD_NONE;
  FMT_NPL(&f) = 1;
  FMT_SZ(&f) = CHUNK;
  rc = ioctl(fd, VIDIOC_S_FMT, &f);
  Perr("S_FMT OUTPUT", rc);
  if (rc != 0){ P("FAIL S_FMT output\n"); return 1; }
  Pkv("  out sizeimage", (long)FMT_SZ(&f));

  { char *p = (char*)&f; for (i=0;i<(int)sizeof(f);i++) p[i]=0; }
  f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  FMT_W(&f) = w; FMT_H(&f) = h;
  FMT_FCC(&f) = V4L2_PIX_FMT_NV12;
  FMT_FLD(&f) = V4L2_FIELD_NONE;
  FMT_NPL(&f) = 1;
  FMT_SZ(&f) = (u32)(w*h*3/2);
  rc = ioctl(fd, VIDIOC_S_FMT, &f);
  Perr("S_FMT CAPTURE", rc);
  if (rc != 0){ P("FAIL S_FMT capture\n"); return 1; }
  P("  negotiated capture: "); Pv(FMT_W(&f)); P("x"); Pv(FMT_H(&f));
  P(" fourcc=0x"); { char b[16]; int k=0; u32 v=FMT_FCC(&f); const char*hx="0123456789abcdef";
    for(k=0;k<8;k++) write(1,&hx[(v>>(28-4*k))&0xf],1); }
  P(" planes="); Pv(FMT_NPL(&f));
  P(" sizeimage="); Pv(FMT_SZ(&f)); P("\n");

  capsiz = (unsigned long)FMT_SZ(&f);
  if (capsiz < (unsigned long)(w*h*3/2)) capsiz = (unsigned long)(w*h*3/2);
  /* --- ION allocation (the driver takes USERPTR buffers, so WE allocate) --- */
  struct ion_alloc { unsigned long len, align; unsigned int heap_mask, flags; int handle; };
  struct ion_fd    { int handle, fd; };
  struct ion_handle_data { int handle; };
  #define ION_IOC_ALLOC 0xC0204900UL
  #define ION_IOC_MAP   0xC0084902UL
  #define ION_IOC_FREE  0xC0044901UL
  { int ion = open("/dev/ion", 2);
    if (ion < 0){ P("FAIL open /dev/ion\n"); return 1; }
    Pkv("ion fd", ion);
    ionfd = ion; }
  P("allocating decoder buffers from ion\n");
  for (i=0;i<OUTF;i++){
    struct ion_alloc a; struct ion_fd f; int r;
    { char*q=(char*)&a;for(r=0;r<(int)sizeof(a);r++)q[r]=0; }
    a.len = CHUNK; a.align = 4096; a.heap_mask = 0xffffffffu; a.flags = 0;
    r = ioctl(ionfd, ION_IOC_ALLOC, &a);
    if (r != 0){ P("ION alloc out FAIL errno="); Pv((long)ERRNO); P("\n"); return 1; }
    { char*q=(char*)&f;for(r=0;r<(int)sizeof(f);r++)q[r]=0; }
    f.handle = a.handle;
    r = ioctl(ionfd, ION_IOC_MAP, &f);
    if (r != 0){ P("ION map out FAIL errno="); Pv((long)ERRNO); P("\n"); return 1; }
    outbuf[i] = mmap(NULL, CHUNK, 3, 1, f.fd, 0);
    outlen[i] = CHUNK; outfd[i] = a.handle;
    if (outbuf[i] == (void*)-1){ P("mmap out FAIL\n"); return 1; }
  }
  for (i=0;i<CAPF;i++){
    struct ion_alloc a; struct ion_fd f; int r;
    { char*q=(char*)&a;for(r=0;r<(int)sizeof(a);r++)q[r]=0; }
    a.len = capsiz; a.align = 4096; a.heap_mask = 0xffffffffu; a.flags = 0;
    r = ioctl(ionfd, ION_IOC_ALLOC, &a);
    if (r != 0){ P("ION alloc cap FAIL "); Pv(i); P(" errno="); Pv((long)ERRNO); P("\n"); return 1; }
    { char*q=(char*)&f;for(r=0;r<(int)sizeof(f);r++)q[r]=0; }
    f.handle = a.handle;
    r = ioctl(ionfd, ION_IOC_MAP, &f);
    if (r != 0){ P("ION map cap FAIL\n"); return 1; }
    capbuf[i] = mmap(NULL, capsiz, 3, 1, f.fd, 0);
    caplen[i] = capsiz; capfd[i] = a.handle;
    if (capbuf[i] == (void*)-1){ P("mmap cap FAIL\n"); return 1; }
  }
  Pkv("allocated out bufs", OUTF); Pkv("allocated cap bufs", CAPF); Pkv("cap buf size", (long)capsiz);

  /* --- REQBUFS with USERPTR (what this driver accepts) --- */
  for (i=0;i<(int)sizeof(rb);i++) ((char*)&rb)[i]=0;
  rb.count = OUTF; rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; rb.memory = 2;
  rc = ioctl(fd, VIDIOC_REQBUFS, &rb); Perr("REQBUFS output", rc); Pkv("  granted", (long)rb.count);
  for (i=0;i<(int)sizeof(rb);i++) ((char*)&rb)[i]=0;
  rb.count = CAPF; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory = 2;
  rc = ioctl(fd, VIDIOC_REQBUFS, &rb); Perr("REQBUFS capture", rc); Pkv("  granted", (long)rb.count);

  src = open(path, 0);
  if (src < 0){ P("FAIL open input\n"); return 1; }
  /* prime OUTPUT with USERPTR buffers carrying the bitstream */
  for (i=0;i<OUTF;i++){
    struct v4l2_plane pl; u32 used;
    used = (u32)read(src, outbuf[i], CHUNK);
    if (used == 0) break;
    { int r5; for (r5=0;r5<(int)sizeof(pl);r5++) ((char*)&pl)[r5]=0; }
    pl.bytesused = used; pl.length = (u32)outlen[i]; pl.m.userptr = (unsigned long)outbuf[i];
    for (rc=0;rc<(int)sizeof(bq);rc++) ((char*)&bq)[rc]=0;
    bq.index=i; bq.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; bq.memory=2;
    bq.m.planes=&pl; bq.length=1; bq.bytesused=used;
    rc = ioctl(fd, VIDIOC_QBUF, &bq);
    if (rc!=0){ Perr("QBUF out", rc); return 1; }
    submitted++; chunks++;
  }
  Pkv("submitted OUTPUT", submitted);
  for (i=0;i<CAPF;i++){
    struct v4l2_plane pl; int r5;
    for (r5=0;r5<(int)sizeof(pl);r5++) ((char*)&pl)[r5]=0;
    pl.length = (u32)caplen[i]; pl.m.userptr = (unsigned long)capbuf[i];
    for (rc=0;rc<(int)sizeof(bq);rc++) ((char*)&bq)[rc]=0;
    bq.index=i; bq.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; bq.memory=2;
    bq.m.planes=&pl; bq.length=1;
    rc = ioctl(fd, VIDIOC_QBUF, &bq);
    if (rc!=0){ Perr("QBUF cap", rc); return 1; }
  }
  { int t;
    t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  rc = ioctl(fd, VIDIOC_STREAMON, &t); Perr("STREAMON output", rc);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rc = ioctl(fd, VIDIOC_STREAMON, &t); Perr("STREAMON capture", rc); }

  for (i=0;i<900;i++){
    struct v4l2_plane pl; u32 used; int r6;
    for (r6=0;r6<(int)sizeof(pl);r6++) ((char*)&pl)[r6]=0;
    for (rc=0;rc<(int)sizeof(bq);rc++) ((char*)&bq)[rc]=0;
    bq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; bq.memory = 2; bq.m.planes = &pl; bq.length = 1;
    rc = ioctl(fd, VIDIOC_DQBUF, &bq);
    if (rc == 0){
      ncap++;
      if (ncap <= 5 || (ncap % 100) == 0){
        P("  CAPTURE frame "); Pv(ncap); P(" index="); Pv((long)bq.index);
        P(" bytesused="); Pv((long)pl.bytesused); P(" seq="); Pv((long)bq.sequence); P("\n");
      }
      { struct v4l2_plane pl2; int r7;
        for (r7=0;r7<(int)sizeof(pl2);r7++) ((char*)&pl2)[r7]=0;
        pl2.length=(u32)caplen[bq.index]; pl2.m.userptr=(unsigned long)capbuf[bq.index];
        bq.m.planes=&pl2; bq.length=1;
        ioctl(fd, VIDIOC_QBUF, &bq); }
      continue;
    }
    for (r6=0;r6<(int)sizeof(pl);r6++) ((char*)&pl)[r6]=0;
    for (rc=0;rc<(int)sizeof(bq);rc++) ((char*)&bq)[rc]=0;
    bq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; bq.memory = 2; bq.m.planes = &pl; bq.length = 1;
    rc = ioctl(fd, VIDIOC_DQBUF, &bq);
    if (rc != 0) break;
    nout++;
    if (chunks < maxchunk){
      used = (u32)read(src, outbuf[bq.index], CHUNK);
      if (used > 0){
        struct v4l2_plane pl2; int r8;
        for (r8=0;r8<(int)sizeof(pl2);r8++) ((char*)&pl2)[r8]=0;
        pl2.bytesused=used; pl2.length=(u32)outlen[bq.index]; pl2.m.userptr=(unsigned long)outbuf[bq.index];
        bq.m.planes=&pl2; bq.length=1; bq.bytesused=used;
        ioctl(fd, VIDIOC_QBUF, &bq); chunks++;
      } else break;
    } else break;
  }
  P("---- result ----\n");
  Pkv("output consumed", nout);
  Pkv("DECODED FRAMES", ncap);
  { int t; t=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  ioctl(fd, VIDIOC_STREAMOFF, &t);
           t=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; ioctl(fd, VIDIOC_STREAMOFF, &t); }
  close(fd); close(src);
  P(ncap > 0 ? "STAGE1: HARDWARE DECODE WORKS\n" : "STAGE1: no frames decoded\n");
  return 0;
}
