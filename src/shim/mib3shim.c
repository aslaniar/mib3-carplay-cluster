/* ============================================================================
 * mib3shim.c -- LD_PRELOAD shim for the LG MIB3 CarPlay stack (R8830L).
 *
 * Purpose: make the head unit advertise a SECOND (instrument-cluster) display
 * to the iPhone, so iOS opens the type-111 Apple Maps cluster stream.
 *
 * Built with NO libc dependency (see build.sh): every symbol it needs (libc +
 * CoreFoundation + the real plugin functions) is resolved at runtime via
 * dlsym() from the host process.  That is why it links against glibc 2.23
 * without needing a matching toolchain -- see LANE_CARPLAY_SHIM.
 *
 * Interposes:
 *   AirPlayInfoArrayAddScreenDisplay  -- LG calls it once (main screen);
 *                                        we capture the args and call it AGAIN
 *                                        with cluster geometry.
 *   AirPlayReceiverSessionSetup       -- log + (optionally) inject the
 *                                        "enabledFeatures" array into the
 *                                        session SETUP response; owns the
 *                                        type-111 stream.
 *   AirPlayCopyServerInfo             -- complete the modern CarKit /info schema
 *                                        (uid/properties/initialVideoStreams +
 *                                        top-level displayPanels/extendedFeatures)
 *                                        on the mutable /info dict the unit is
 *                                        about to serialise.
 *
 * Logs to /tmp/mib3shim.log (world-writable tmpfs; SMACK `_`).
 * Env knobs:
 *   MIB3_SHIM_ADD=0/1         advertise the cluster display (default 1)
 *   MIB3_SHIM_W/H/FPS         cluster geometry (default 1280x480@30)
 *   MIB3_SHIM_INFO=0/1        complete the modern /info schema (default 1)
 *   MIB3_SHIM_ENABLEDFEAT=0/1 inject enabledFeatures into the CONTROL SETUP (default 1)
 *   MIB3_SHIM_PORT=<n>        cluster data port (default: 5000, then 6031, then
 *                             ephemeral).  5000 is INSIDE the unit's own
 *                             firewall allowlist (GENERIC_PORT_IN_FILTER
 *                             "-i ncm0 -p tcp --dport 5000:5001"); 6031 and any
 *                             ephemeral port are FIREWALL-DROPPED on ncm0/wlap
 *                             (the phone's SYN never reaches us) and need
 *                             live_kit/cluster_port_fw.sh first.
 *   MIB3_SHIM_CMDS=0/1        send forceKeyFrame/showUI/setNightMode to the
 *                             phone once the 111 stream connects (default 1)
 *   MIB3_SHIM_EF_IAPCHANNEL=1 also claim "iAPChannel" in enabledFeatures
 *                             (default 0 -- the rgi reference, the only
 *                             stock-HU-proven one, claims only altScreen +
 *                             viewAreas; every string flips a phone-side
 *                             endpoint flag, so claim nothing extra)
 *   MIB3_SHIM_DBG=0/1         verbose (default 1)
 * ==========================================================================*/

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long      u64;
typedef long               s64;
typedef long               ssize_t_;

/* CF types (opaque) */
typedef const void        *CFTypeRef;
typedef const void        *CFStringRef;
typedef const void        *CFArrayRef;
typedef void              *CFMutableArrayRef;
typedef const void        *CFDictionaryRef;
typedef void              *CFMutableDictionaryRef;
typedef const void        *CFAllocatorRef;
typedef long               CFIndex;
typedef unsigned char      Boolean;
typedef unsigned int       CFStringEncoding;

#define ULL unsigned long long

/* ---- libc + ld, resolved at runtime (declared here, no headers) ---------- */
extern void  *dlsym(void *handle, const char *name);
extern char  *getenv(const char *name);
extern int    open(const char *path, int flags, ...);
extern long   write(int fd, const void *buf, unsigned long n);
extern int    close(int fd);

#define RTLD_NEXT    ((void *)-1L)
#define RTLD_DEFAULT ((void *)0)

/* ---------------------------------------------------------------------------
 * Two-tier symbol resolution.
 *
 * RTLD_NEXT is correct when this shim is loaded BEFORE the real libraries
 * (LD_PRELOAD / DT_NEEDED ordering): it finds the next object after us.
 *
 * But when the shim is dlopen()ed into a process that is ALREADY RUNNING, it is
 * appended LAST in the search order -- so RTLD_NEXT finds nothing and EVERY
 * pointer in resolve() came back NULL. The shim then had no real function to
 * forward to and broke whatever call it intercepted.
 *
 * In that case RTLD_DEFAULT is correct: the real libraries are already in the
 * global scope and we are last, so the first definition found is the real one.
 *
 * Load-time behaviour is unchanged (RTLD_NEXT succeeds and wins); the fallback
 * only ever runs when RTLD_NEXT fails.
 * ------------------------------------------------------------------------- */
extern void *dlsym(void *handle, const char *name);
static void *R(const char *name)
{
  void *p = dlsym(RTLD_NEXT, name);
  if (!p) p = dlsym(RTLD_DEFAULT, name);
  return p;
}

#define O_WRONLY 1
#define O_CREAT  0100
#define O_APPEND 02000

/* ---- minimal string/format helpers (no libc stdio) ----------------------- */
static unsigned long slen_(const char *s){ unsigned long n=0; while(s && s[n]) n++; return n; }

typedef struct { char *p; unsigned long cap, n; } buf;
/* NB: n must never exceed cap, or logline()'s write(fd,mem,n) would read past
 * the fixed stack buffer.  Saturate (drop overlong tails) instead of counting
 * bytes we did not store.  Callers NUL-terminate explicitly after building. */
static void bch(buf *b, char c){ if (b->n < b->cap){ b->p[b->n]=c; b->n++; } }
static void bs(buf *b, const char *s){ unsigned long i=0; while(s && s[i]) bch(b,s[i++]); }
static void bu64(buf *b, u64 v, int base, int minw){
  char t[24]; int k=0; if(v==0) t[k++]='0';
  const char *hx="0123456789abcdef";
  while (v){ t[k++]=hx[v%base]; v/=base; }
  while (k<minw) t[k++]='0';
  while (k--) bch(b, t[k]);
}
static void bs64(buf *b, s64 v){ if(v<0){ bch(b,'-'); bu64(b,(u64)(-v),10,0);} else bu64(b,(u64)v,10,0); }
static void bhex(buf *b, u64 v){ bs(b,"0x"); bu64(b,v,16,0); }

/* ---- logging ------------------------------------------------------------- */
static int logfd = -1;
static void log_open(void){
  if (logfd < 0) logfd = open("/tmp/mib3shim.log", O_WRONLY|O_CREAT|O_APPEND, 0644);
}
static void logline(const char *tag, const char *msg){
  char mem[2048]; buf b = { mem, sizeof(mem)-2, 0 };
  bs(&b,"[mib3shim] "); bs(&b,tag); bs(&b,": "); bs(&b,msg); bch(&b,'\n');
  mem[b.n]=0;                       /* b.n <= cap = sizeof-2 -> always in-bounds */
  log_open();
  if (logfd >= 0) write(logfd, mem, b.n);
}

/* ---------------------------------------------------------------------------
 * INSTRUMENTATION (2026-10-15).
 *
 * A raw entry marker that depends on NOTHING -- not resolve(), not the /tmp log
 * path, not any env. If /dev/shm/mib3shim_hit never appears, the shim was never
 * entered at all, and the call never reached us.
 * ------------------------------------------------------------------------- */
#define O_TRUNC 01000
static void hit(const char *tag, unsigned long n)
{
  int f = open("/dev/shm/mib3shim_hit", O_WRONLY|O_CREAT|O_TRUNC, 0666);
  if (f >= 0) write(f, tag, n);
}
static void logkv(const char *tag, const char *key, u64 v, int hex){
  char mem[256]; buf b={mem,sizeof(mem)-2,0};
  bs(&b,key); bs(&b,"="); if(hex) bhex(&b,v); else bu64(&b,v,10,0);
  mem[b.n]=0;
  logline(tag, mem);
}

#include "mib3helper.h"

/* ==========================================================================
 * BREADCRUMBS (2026-10-16) -- evidence that SURVIVES A RESET.
 *
 * The 2026-10-16 live sink attempt blacked the centre screen and the unit
 * reset; the reset wipes /tmp and /dev/shm, so /tmp/mib3shim.log and the old
 * /dev/shm/mib3shim_hit entry marker both died -- the failing step was
 * UNDIAGNOSABLE.  The only storage that survives is the USB stick.
 *
 * So: before every risky display-touching call (and between sub-steps) append a
 * one-line marker to a file on the STICK with an explicit write()+fsync().
 * After a reset, the stick holds the exact last step reached.
 *
 * The file MUST be created BY THE SHIM: a root-created file carries the wrong
 * SMACK label and carplay's writes vanish silently (THE LABEL TRAP).
 *
 *   MIB3_SHIM_BC=<path>   explicit breadcrumb file
 *   else auto: <stick>/PART1/mib3shim.bc where <stick> is the same /tmp/USB/
 *         PORT<n> scan the activation gate uses; fallback /dev/shm/mib3shim.bc
 *         (RAM: best-effort only -- it does NOT survive a reset).
 * -------------------------------------------------------------------------*/
extern int  fsync(int fd);
extern int  access(const char *path, int mode);
static int  bc_fd = -2;                 /* -2 = untried, -1 = unavailable */
static char bc_path[256];

static int bc_pick_path(void){
  char *e = getenv("MIB3_SHIM_BC");
  unsigned long n = 0, i;
  if (e && e[0]){ while (e[n] && n < sizeof(bc_path)-1){ bc_path[n]=e[n]; n++; } bc_path[n]=0; return 1; }
  /* scan /tmp/USB/PORT<n>/PART1/mib3shim.enable (the gate marker) -> put the
   * breadcrumb beside it; the marker's presence means the stick is mounted and
   * carplay can write the FAT/tmpfs there. */
  for (i = 1; i <= 8; i++){
    char dir[64], mk[80], bcf[128]; unsigned long d = 0, k = 0;
    const char *pfx = "/tmp/USB/PORT"; while (pfx[k] && d < sizeof(dir)-8) dir[d++]=pfx[k++];
    if (i >= 10) dir[d++]=(char)('0'+i/10);
    dir[d++]=(char)('0'+i%10); dir[d]=0;
    { const char *s="/PART1/mib3shim.enable"; unsigned long j=0; k=0;
      while (dir[k] && k < sizeof(mk)-24) mk[k]=dir[k], k++;
      while (s[j] && k < sizeof(mk)-1) mk[k++]=s[j++]; mk[k]=0; }
    if (!access(mk, 0 /*F_OK*/)){
      /* build <dir>/PART1/mib3shim.bc */
      unsigned long j=0;
      { const char *s="/PART1/mib3shim.bc"; while (dir[j] && j < sizeof(bcf)-24) bcf[j]=dir[j], j++;
        { unsigned long m=0; while (s[m] && j < sizeof(bcf)-1) bcf[j++]=s[m++]; } bcf[j]=0; }
      { unsigned long m=0; while (bcf[m] && m < sizeof(bc_path)-1){ bc_path[m]=bcf[m]; m++; } bc_path[m]=0; }
      return 1;
    }
  }
  { const char *fb = "/dev/shm/mib3shim.bc"; unsigned long m=0;
    while (fb[m] && m < sizeof(bc_path)-1){ bc_path[m]=fb[m]; m++; } bc_path[m]=0; }
  return 1;
}
static void bc(const char *step){
  if (bc_fd == -2){ bc_pick_path(); bc_fd = open(bc_path, O_WRONLY|O_CREAT|O_APPEND, 0644); }
  if (bc_fd >= 0){
    char mem[220]; buf b = { mem, sizeof(mem)-2, 0 };
    extern long time(long *); extern int getpid(void);
    bs(&b,"[bc "); bu64(&b,(u64)time(0),10,0); bs(&b," pid="); bu64(&b,(u64)getpid(),10,0);
    bs(&b,"] "); bs(&b,step); bch(&b,'\n'); mem[b.n]=0;
    write(bc_fd, mem, b.n);
    fsync(bc_fd);                       /* force it to the stick NOW */
  }
  logline("bc", step);                  /* mirror into the (volatile) /tmp log */
}

/* ==========================================================================
 * OUT-OF-PROCESS DELIVERY (the SAFE sink) -- shim side (2026-10-16).
 *
 * Forward each decrypted access unit, length-prefixed (mib3helper.h), to a
 * SEPARATE helper process over an AF_UNIX socket.  carplay performs NO
 * display-touching call at all, so a helper fault cannot black the centre
 * screen or trip carplay.service's FailureAction=reboot-immediate.
 *
 * Best-effort: if the helper is absent the connect fails once and we drop
 * frames (counted).  A bounded send timeout stops a wedged helper from
 * blocking the receiver thread.
 *
 *   MIB3_SHIM_HELPER=<path>   default /dev/shm/mib3cluster.sock
 * -------------------------------------------------------------------------*/
extern int socket(int, int, int);
extern int connect(int, const void*, unsigned int);
extern int setsockopt(int, int, int, const void*, unsigned int);
extern long send(int, const void*, unsigned long, int);
extern int *__errno_location(void);
#define SHIM_ERRNO (*__errno_location())
#define O_NONBLOCK_ 04000
#define SOL_SOCKET_ 1
#define SO_SNDTIMEO_ 21
static int hlp_fd = -1;
static unsigned long hlp_seq = 0, hlp_sent = 0, hlp_drop = 0, hlp_conns = 0;
/* BOUNDED EAGAIN RECOVERY (2026-10-19).  A helper whose peer is gone (one-way:
 * nobody reading) makes every send() return EAGAIN with MSG_DONTWAIT.  The old
 * "keep the socket on EAGAIN" rule then held it FOREVER: every AU dropped, the
 * cluster frozen on an old frame while the shim kept logging "receiving video".
 * Track consecutive EAGAIN failures and the wall-clock of the last successful
 * send; close (and therefore reconnect on the next AU) once either bound trips.
 * A slow-but-alive helper resets the counter on every success and never trips it,
 * so only a truly wedged/one-way peer is torn down. */
static int  hlp_eagain = 0;          /* consecutive EAGAINs since the last success */
static long hlp_last_ok = 0;         /* time(0) of the last fully-sent record */
static void helper_close(void){
  if (hlp_fd >= 0){ close(hlp_fd); hlp_fd = -1; }
  hlp_eagain = 0;                    /* a fresh socket starts with a clean slate */
}
static const char *helper_path(void){
  char *v = getenv("MIB3_SHIM_HELPER"); return (v && v[0]) ? v : MIB3_HELP_SOCK_DEFAULT;
}
static int helper_connect(void){
  struct { unsigned short fam; char path[108]; } sa;
  int fd, i;
  if (hlp_fd >= 0) return 0;
  { const char *p = helper_path();
    for (i = 0; i < 108; i++) sa.path[i] = 0;
    for (i = 0; i < 107 && p[i]; i++) sa.path[i] = p[i]; }
  sa.fam = 1 /*AF_UNIX*/;
  fd = socket(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0);
  if (fd < 0){ logkv("sink","helper socket() errno",(u64)(unsigned)SHIM_ERRNO,0); return -1; }
  if (connect(fd, &sa, sizeof(sa)) != 0){
    logline("sink/helper-path", helper_path());
    logkv("sink","helper connect() errno",(u64)(unsigned)SHIM_ERRNO,0);
    close(fd); return -1;
  }
  { struct { long s; long us; } tv; tv.s = 0; tv.us = 200000;
    setsockopt(fd, SOL_SOCKET_, SO_SNDTIMEO_, &tv, sizeof(tv)); }
  hlp_fd = fd; hlp_conns++;
  return 0;
}
/* MSG_NOSIGNAL: a write to a socket whose peer is gone raises SIGPIPE as well as
 * returning EPIPE, and SIGPIPE's default action is to KILL THE PROCESS.  This shim
 * runs INSIDE carplay, whose failure action on this unit is reboot-immediate -- so
 * a helper (decoder) dying while we are mid-write must not be able to take the
 * process, and the car, down with it.  This became a routine path on 2026-10-18:
 * the lifecycle controller stops our decoder at the end of every session
 * (lanes/closeouts: LANE_CLUSTER_TRANSITION_2026-10-18.md S9).  send(MSG_NOSIGNAL)
 * keeps the fix local to these two writes; it does not touch the process's
 * signal disposition, so nothing else in carplay changes. */
#define MIB3_MSG_NOSIGNAL 0x4000
/* Non-blocking send.  Without this, a helper that stops reading (a wedged
 * decoder, measured 2026-10-19 with nfree=0) makes send() BLOCK inside
 * carplay's cluster receiver thread -- and the whole CarPlay session stutters
 * on the main screen.  A dropped AU is recoverable; a stalled receiver thread
 * is not. */
#define MIB3_MSG_DONTWAIT 0x40

extern long time(long *);                       /* used by the backoff + EAGAIN bound */
static u64 env_num(const char *name, u64 dflt); /* defined after the CF section */

static int helper_feed(const void *data, unsigned long len, unsigned long ts){
  unsigned char hdr[MIB3_HELP_HDRLEN];
  u32 m32; u64 m64; int i;
  (void)ts;
  if (!data || !len || len > MIB3_HELP_MAXLEN) return -1;
  if (hlp_fd < 0){
    /* With no decoder attached, every AU would otherwise attempt connect()
     * (~100/s, measured as syscall churn).  One attempt per second is plenty:
     * the socket appears once the decoder starts. */
    static long last_try = 0;
    extern long time(long *);
    long now = time(0);
    if (now != last_try){
      last_try = now;
      if (helper_connect() != 0){ hlp_drop++; return -1; }
    } else { hlp_drop++; return -1; }
  }
  m32 = MIB3_HELP_MAGIC; for (i=0;i<4;i++) hdr[i]   = (unsigned char)((m32 >> (i*8)) & 0xff);
  m32 = (u32)len;        for (i=0;i<4;i++) hdr[4+i] = (unsigned char)((m32 >> (i*8)) & 0xff);
  m64 = ++hlp_seq;       for (i=0;i<8;i++) hdr[8+i] = (unsigned char)((m64 >> (i*8)) & 0xff);
  { long r = send(hlp_fd, hdr, MIB3_HELP_HDRLEN, MIB3_MSG_NOSIGNAL|MIB3_MSG_DONTWAIT);
    if (r != (long)MIB3_HELP_HDRLEN){
      hlp_drop++;
      /* Distinguish "nothing written because the buffer is full" (EAGAIN) from
       * every other outcome.  EAGAIN leaves the stream in sync and MAY be kept
       * -- but only BOUNDED (see the recovery note at hlp_eagain).  A short
       * positive write (r>=0 but != HDRLEN) or a fatal errno (EPIPE after the
       * helper died, ECONNRESET, ...) MUST close: the record is half-gone and a
       * stale fd to an unlinked socket otherwise never reconnects (measured
       * 2026-10-19: "helper connected" while the decoder never saw one). */
      if (r < 0 && SHIM_ERRNO == 11 /*EAGAIN*/){
        long now = time(0);
        long emax = (long)env_num("MIB3_SHIM_HLP_EAGAIN", 8);
        long stall = (long)env_num("MIB3_SHIM_HLP_STALL", 2);
        hlp_eagain++;
        if (hlp_eagain >= emax ||
            (hlp_last_ok && now - hlp_last_ok > stall)){
          logkv("sink","helper EAGAIN bound tripped -- closing",(u64)(u32)hlp_eagain,0);
          helper_close();
        }
      } else {
        helper_close();
      }
      return -1; }
  }
  { long r = send(hlp_fd, data, len, MIB3_MSG_NOSIGNAL|MIB3_MSG_DONTWAIT);
    if (r != (long)len){
      /* Partial record = permanent desync; the helper must reconnect. */
      hlp_drop++; helper_close(); return -1; } }
  hlp_eagain = 0;                 /* a complete record went out: reset the bound */
  hlp_last_ok = time(0);
  hlp_sent++;
  return 0;
}

/* ---- CF/libAirPlay symbols, lazily resolved ------------------------------ */
typedef CFStringRef (*fn_CFStringCreateWithCString)(CFAllocatorRef,const char*,CFStringEncoding);
typedef Boolean     (*fn_CFStringGetCString)(CFStringRef,char*,CFIndex,CFStringEncoding);
typedef CFIndex     (*fn_CFArrayGetCount)(CFArrayRef);
typedef CFTypeRef   (*fn_CFArrayGetValueAtIndex)(CFArrayRef,CFIndex);
typedef CFMutableArrayRef (*fn_CFArrayCreateMutable)(CFAllocatorRef,CFIndex,const void*);
typedef void        (*fn_CFArrayAppendValue)(CFMutableArrayRef,const void*);
/* CFArrayAppendInt64(dict, s64) -- exported by the unit's libCoreUtils CFLite
 * (verified: llvm-readelf on lge/app_rw/lib64/libCoreUtils.so).  Needed for
 * adjacentViewAreas = [0] in updateViewArea (see the VIEW AREAS block). */
typedef void        (*fn_CFArrayAppendInt64)(CFMutableArrayRef,s64);
typedef void        (*fn_CFDictionarySetValue)(CFMutableDictionaryRef,const void*,const void*);
typedef CFTypeRef   (*fn_CFDictionaryGetValue)(CFDictionaryRef,const void*);
typedef void        (*fn_CFShow)(CFTypeRef);
typedef void        (*fn_CFRelease)(CFTypeRef);
typedef void        (*fn_CFDictionarySetInt64)(CFMutableDictionaryRef,const void*,s64);
typedef CFMutableDictionaryRef (*fn_CFDictionaryCreateMutable)(CFAllocatorRef,CFIndex,const void*,const void*);
typedef s64         (*fn_CFDictionaryGetInt64)(CFDictionaryRef,const void*,s64);
typedef unsigned long (*fn_CFGetTypeID)(CFTypeRef);
typedef unsigned long (*fn_CFTypeGetTypeID)(void);
/* for the value dumper */
typedef CFIndex     (*fn_CFDictionaryGetCount)(CFDictionaryRef);
typedef CFArrayRef  (*fn_CFDictionaryCopyKeys)(CFAllocatorRef,CFDictionaryRef);
typedef long long   (*fn_CFGetInt64)(CFTypeRef,int*);

/* --- cluster-stream (type 111) path: exported unit helpers the shim reuses --- */
typedef int  (*fn_ServerSocketOpen)(unsigned, int, int, int, int*, int, int*);
typedef int  (*fn_NetSocketCreate)(void**, int);
typedef int  (*fn_NetSocketNative)(void*);
typedef int  (*fn_SocketAccept)(int, unsigned long, int*, void*);
typedef void (*fn_SocketSetQoS)(int, int);
typedef int  (*fn_ScreenCreate)(void**);
typedef int  (*fn_ScreenSetup)(void*, void*);
typedef int  (*fn_ScreenStart)(void*, void*);
typedef int  (*fn_ScreenStop)(void*);
typedef void (*fn_ScreenDelete)(void*);   /* frees the screen (free()) -- StopSession does NOT */
typedef int  (*fn_ScreenProcFrames)(void*, void*, int);
typedef int  (*fn_ScreenSetSec)(void*, void*, void*);
typedef int  (*fn_ScreenSetChaCha)(void*, void*, s64);
typedef void (*fn_ScreenSetTS)(void*, void*);
typedef int  (*fn_DeriveAES)(void*, int, s64, void*, void*);
typedef int  (*fn_pthreadCreate)(void**, const void*, void*(*)(void*), void*);
typedef int  (*fn_close)(int);
typedef int  (*fn_chachaDerive)(void*, s64, int, int, void*); /* returns int status (0=ok) */
/* wire crypto (unit's own CFLite/ChaCha primitives) -- see stock FUN_00115800 +
 * AirPlayReceiverSessionScreen_ProcessFrames (corpus 0x11fbf0): the per-frame
 * screen path is init(ctx, key32, nonce8) -> add_aad(ctx, header, 128) ->
 * decrypt(ctx, body, bodySize-16, body) -> verify(ctx, dst+n, body+bodySize-16,
 * &ok); the last 16 body bytes are the Poly1305 tag, NOT ciphertext, and n+m
 * must equal bodySize-16.  (The old typedef passed verify's 3rd arg as `long`
 * -- Ghidra renders the pointer as long; it was never called so it went
 * unnoticed until now.) */
typedef void (*fn_cx_init)(void*, const void*, const void*);      /* ctx,key,nonce8 */
typedef void (*fn_cx_aad)(void*, const void*, unsigned long);       /* ctx,aad,len  */
typedef long (*fn_cx_dec)(void*, const void*, long, void*);        /* ctx,src,len,dst -> plainlen */
typedef long (*fn_cx_ver)(void*, const void*, const void*, int*);   /* ctx,tag_computed,tag_wire,&ok -> taglen */
typedef int  (*fn_ctr_init)(void*, const void*, const void*);     /* AES_CTR_Init(ctx,key,iv) */
typedef int  (*fn_ctr_upd)(void*, const void*, long, void*);      /* AES_CTR_Update(ctx,src,len,dst) */
/* half-2 diagnostic: the CarPlay output-surface selector (C++ mangled). */
typedef int  (*fn_SetSurface)(void*, void*, unsigned);
/* The framework's per-stream FRAME ENTRY (libCarPlayMediaFramework.so, exported
 * FUNC GLOBAL DEFAULT @ 0x1e000).  libAirPlay reaches it through its own PLT at
 * AirPlayReceiverSessionScreen_ProcessFrames:
 *     ScreenStreamProcessData(screen->stream [screen+0x328], buf, len, ts, 0,0,0)
 * It is the ONLY stage that carries BOTH (a) the frame payload and (b) the
 * per-stream ScreenStreamRef identity -> the frame-interception point. */
typedef int  (*fn_ScreenStreamProcessData)(void*, const void*, unsigned long,
                                           unsigned long, void*, void*, void*);

/* --- DECODE BRIDGE: the shim's own Cinemo pipeline (libNmeSDK.so) ----------
 * The cluster frames arrive COMPRESSED (H.264); the shim's RGBA surface path
 * cannot display them.  The bridge creates a SECOND Cinemo pipeline (its own
 * config + playlist + playback + window + projection, bound to the shim's ivi
 * surface id) and feeds the compressed frames to that projection, so Cinemo
 * decodes + renders them into the shim's surface.
 *
 * The Cinemo SDK (libNmeSDK.so) exposes these as `extern "C"` FACTORY functions
 * that malloc a fresh object each call -- there is no global one-shot guard, so
 * a second pipeline is constructible at the API level (see the lane doc for the
 * full verdict + the runtime caveat).  The call sequence + vtable offsets below
 * are copied verbatim from the unit's own CinemoMediaEngineUtils::screenSetup()
 * (libCarPlayMediaFramework.so 0x120ac4), which is proven to work on the unit. */
typedef int  (*fn_CinemoCreate)(void**);              /* Config, Playlist */
typedef int  (*fn_CinemoCreateStr)(const char*, void**); /* Window(devname), Projection(param) */
/* raw C++ vtable-call helpers: (obj + N args) */
typedef long (*fn_vc0)(void*);
typedef long (*fn_vc1)(void*, void*);
typedef long (*fn_vc2)(void*, void*, void*);
typedef long (*fn_vc3)(void*, void*, void*, void*);
typedef long (*fn_vc4)(void*, void*, void*, void*, void*);

/* --- ROUTE-B: the shim's own cluster SURFACE (Wayland/ivi + EGL/GLES) ------
 * Signatures mirror the unit's real API (libwayland-client, libwaylandegl,
 * libEGL/libGLESv2) as used by Cinemo's NmeWindowWayland / NmeDeviceEGL
 * (see corpus/libNmeVideoDevice.so: FUN_0013b180 window-create, NmeDeviceEGL
 * CreateContext).  Everything is resolved via dlsym; the bench substitutes a
 * stub for the Adreno-locked graphics stack. */
typedef void *(*fn_wl_connect)(const char*);
typedef void  (*fn_wl_disp_void)(void*);
typedef int   (*fn_wl_disp_int)(void*);
typedef void *(*fn_wl_pmc)(void*, unsigned, const void*, ...);          /* wl_proxy_marshal_constructor */
typedef void *(*fn_wl_pmcv)(void*, unsigned, const void*, unsigned, ...);/* wl_proxy_marshal_constructor_versioned */
typedef int   (*fn_wl_add_listener)(void*, void*, void*);
typedef void  (*fn_wl_destroy)(void*);
typedef void *(*fn_wl_eglwin_create)(void*, int, int);
typedef void  (*fn_wl_eglwin_destroy)(void*);
typedef void *(*fn_eglGetDisplay)(void*);
typedef int   (*fn_eglInitialize)(void*, int*, int*);
typedef int   (*fn_eglBindAPI)(unsigned);
typedef int   (*fn_eglChooseConfig)(void*, const int*, void**, int, int*);
typedef void *(*fn_eglCreateWindowSurface)(void*, void*, void*, const int*);
typedef void *(*fn_eglCreateContext)(void*, void*, void*, const int*);
typedef int   (*fn_eglMakeCurrent)(void*, void*, void*, void*);
typedef int   (*fn_eglSwapBuffers)(void*, void*);
typedef void  (*fn_gl_void1)(unsigned);
typedef void  (*fn_gl_void4)(int,int,int,int);
typedef void  (*fn_gl_gen)(int, unsigned*);
typedef void  (*fn_gl_bind)(unsigned, unsigned);
typedef void  (*fn_gl_tex)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
typedef void  (*fn_gl_draw)(unsigned,int,int);

/* NB: the 7 numeric args are 32-bit (undefined4 in the plugin) -- match the ABI. */
typedef int (*fn_AddScreenDisplay)(CFMutableArrayRef*, CFStringRef,
                                   int, int, int, int, int, int, int);
typedef int  (*fn_SessionSetup)(void*, void*, void**, int);   /* real ABI (rgi reference): returns the setup status, 4 args */
/* AirPlayReceiverSessionSendCommand(session, cmdDict{type,params}, completion, ctx)
 * -> status.  Verified on the unit's own libAirPlay (exported @ 0x17f3c): it
 * dispatch_sync_f's onto the session's own control queue (session+0x448) --
 * safe to call from our receiver thread; the CALLER CFReleases the cmd dict
 * after it returns (the stock AirPlayReceiverSessionForceKeyFrame pattern,
 * corpus 0x1e31c: create mutable dict, SetValue("type","forceKeyFrame"),
 * SendCommand(session,cmd,completion,ctx), CFRelease(cmd)). */
typedef int  (*fn_SendCommand)(void*, void*, void*, void*);
/* AirPlayCopyServerInfo(server, arg1, errOut) -> the mutable /info dict.
 * Verified call site: libAirPlay.so 0xeed4 sets only x0/x1/x2 (3 args) and the
 * callee prologue reads x0 (server), x1, x2 (err out).  RELOCATION:
 * R_AARCH64_JUMP_SLOT @ .got+0x3ec40 -> 0xe20c, so the intra-lib call is
 * interposable exactly like the other two seams.  This is the /info builder:
 * the dict it returns is CFDictionaryCreateMutable, handed straight to
 * CFBinaryPlistV0Create and out as the HTTP /info response the phone reads. */
typedef void *(*fn_CopyServerInfo)(void*, void*, void*);

static fn_CFStringCreateWithCString p_CFStringCreateWithCString;
static fn_CFStringGetCString        p_CFStringGetCString;
static fn_CFArrayGetCount           p_CFArrayGetCount;
static fn_CFArrayGetValueAtIndex    p_CFArrayGetValueAtIndex;
static fn_CFArrayAppendValue        p_CFArrayAppendValue;
static fn_CFArrayAppendInt64        p_CFArrayAppendInt64;
static fn_CFDictionarySetValue      p_CFDictionarySetValue;
static fn_CFDictionaryGetValue      p_CFDictionaryGetValue;
static fn_CFShow                    p_CFShow;
static fn_CFRelease                 p_CFRelease;
static fn_CFDictionarySetInt64      p_CFDictionarySetInt64;
static fn_CFDictionaryCreateMutable p_CFDictionaryCreateMutable;
static fn_CFDictionaryGetInt64      p_CFDictionaryGetInt64;
static fn_CFGetTypeID               p_CFGetTypeID;
static fn_CFTypeGetTypeID           p_CFArrayGetTypeID;
static fn_CFTypeGetTypeID           p_CFDictionaryGetTypeID;
static fn_CFTypeGetTypeID           p_CFStringGetTypeID;
static fn_CFTypeGetTypeID           p_CFNumberGetTypeID;
static fn_CFTypeGetTypeID           p_CFBooleanGetTypeID;
static fn_CFDictionaryGetCount      p_CFDictionaryGetCount;
static fn_CFDictionaryCopyKeys      p_CFDictionaryCopyKeys;
static fn_CFGetInt64                p_CFGetInt64;
static void *p_kCFTypeArrayCallBacks;
static void *p_kCFLDictKeyCB;
static void *p_kCFLDictValCB;
static void *p_kCFLBooleanTrue;
static void *p_kCFLBooleanFalse;

/* cluster-stream (type 111) helpers + libAirPlay base (for RVA calls) */
static fn_ServerSocketOpen  p_ServerSocketOpen;
static fn_NetSocketCreate   p_NetSocketCreate;
static fn_NetSocketNative   p_NetSocketNative;
static fn_SocketAccept      p_SocketAccept;
static fn_SocketSetQoS      p_SocketSetQoS;
static fn_ScreenCreate      p_ScreenCreate;
static fn_ScreenSetup       p_ScreenSetup;
static fn_ScreenStart       p_ScreenStart;
static fn_ScreenStop        p_ScreenStop;
static fn_ScreenDelete      p_ScreenDelete;
static fn_ScreenProcFrames  p_ScreenProcFrames;
static fn_ScreenSetSec      p_ScreenSetSec;
static fn_ScreenSetChaCha   p_ScreenSetChaCha;
static fn_ScreenSetTS       p_ScreenSetTS;
static fn_DeriveAES         p_DeriveAES;
static fn_cx_init           p_cx_init;
static fn_cx_aad            p_cx_aad;
static fn_cx_dec            p_cx_dec;
static fn_cx_ver            p_cx_ver;
static fn_ctr_init          p_ctr_init;
static fn_ctr_upd           p_ctr_upd;
static fn_pthreadCreate     p_pthreadCreate;
static fn_close             p_close;
static void *(*p_malloc)(unsigned long);
static void  (*p_free)(void*);
static fn_SetSurface        p_realSetSurface;
static fn_ScreenStreamProcessData p_realSSPD;
static char                *g_libap_base = 0;

/* decode-bridge: the shim's own Cinemo pipeline (resolved lazily) */
static fn_CinemoCreate     p_CinemoCreateConfig;
static fn_CinemoCreate     p_CinemoCreatePlaylist;
static fn_CinemoCreateStr  p_CinemoCreateWindow;
static fn_CinemoCreateStr  p_CinemoCreateProjection;
/* Cinemo logging (libNmeBaseClasses): turn it on from inside the helper so the
 * device's own decisions ("using %s swap-chain") become visible. */
static int (*p_NmeLogSetLevel)(int);
static int (*p_NmeLogSetTarget)(int,const char*,void*,void*);
static void               *p_opt_video_device_id;   /* &CINEMO_OPTION_VIDEO_DEVICE_ID */
static void               *p_iid_cinemo_option;     /* &ICinemoOption::iid */
/* The playback ICinemoOption keys the framework sets in screenSetup() (2026-10-17). */
static void               *p_opt_aspect, *p_opt_max_res, *p_opt_frame_drop, *p_opt_max_fps,
                          *p_opt_dist_en, *p_opt_dist_delay, *p_opt_hw_decode;

/* route-B graphics symbols (resolved lazily in resolve_gfx) */
static fn_wl_connect          p_wl_display_connect;
static fn_wl_disp_void        p_wl_display_disconnect;
static fn_wl_disp_int         p_wl_display_get_fd;
static fn_wl_disp_int         p_wl_display_roundtrip;
static fn_wl_disp_int         p_wl_display_dispatch_pending;
static fn_wl_disp_int         p_wl_display_flush;
static fn_wl_pmc              p_wl_pmc;
static fn_wl_pmcv             p_wl_pmcv;
static fn_wl_add_listener     p_wl_proxy_add_listener;
static fn_wl_destroy          p_wl_proxy_destroy;
static fn_wl_eglwin_create    p_wl_egl_window_create;
static fn_wl_eglwin_destroy   p_wl_egl_window_destroy;
static fn_eglGetDisplay       p_eglGetDisplay;
static fn_eglInitialize       p_eglInitialize;
static fn_eglBindAPI          p_eglBindAPI;
static fn_eglChooseConfig     p_eglChooseConfig;
static fn_eglCreateWindowSurface p_eglCreateWindowSurface;
static fn_eglCreateContext    p_eglCreateContext;
static fn_eglMakeCurrent      p_eglMakeCurrent;
static fn_eglSwapBuffers      p_eglSwapBuffers;
static fn_gl_void1            p_glClear;
static fn_gl_void4            p_glViewport;
static fn_gl_gen              p_glGenTextures;
static fn_gl_bind             p_glBindTexture;
static fn_gl_tex              p_glTexImage2D;
static fn_gl_tex              p_glTexSubImage2D;
static fn_gl_draw             p_glDrawArrays;
static void                  *p_wl_registry_interface;
static void                  *p_wl_compositor_interface;
static void                  *p_wl_surface_interface;
static void                  *p_ivi_application_interface;
static void                  *p_ivi_surface_interface;

static fn_AddScreenDisplay real_AddScreenDisplay;
static fn_SessionSetup     real_SessionSetup;
static fn_CopyServerInfo   real_CopyServerInfo;
static fn_SendCommand      p_SendCommand;    /* the stock command shipper (showUI/forceKeyFrame/setNightMode) */

/* ==========================================================================
 * RUNTIME GATE (the physical cure) -- see LANE_PERSISTENCE_INSTALL_2026-10-14.md
 *
 * The shim is now loaded on EVERY boot (it lives on /data, which the kernel
 * mounts before systemd), so the *load* is not the enable signal any more.
 * The enable signal is a marker on the USB stick, checked HERE at runtime:
 *
 *     <stick>/mib3shim.enable      (/tmp/USB/PORTn/PART1/mib3shim.enable)
 *
 * Why runtime and not boot: `sdcm` mounts the stick at multi-user.target,
 * UNORDERED relative to carplay.service, so a boot-time check loses the race
 * (FINDING_STICK_GATE_RACE_2026-10-10.md).  Session setup happens seconds-to-
 * minutes after boot, by which time the stick (if present) is mounted.
 *
 *   marker present  -> the cluster is activated (advertise + handle type-111).
 *   marker absent   -> behave exactly stock (forward everything).
 *   pull the stick  -> the NEXT session is inert.   <-- the physical cure
 * ========================================================================*/
typedef struct { unsigned long d_ino; long d_off; unsigned short d_reclen;
                 unsigned char d_type; char d_name[256]; } mib3_dirent;
typedef void *(*fn_opendir)(const char*);
typedef mib3_dirent *(*fn_readdir)(void*);
typedef int   (*fn_closedir)(void*);
typedef int   (*fn_access)(const char*, int);
static fn_opendir  p_opendir;
static fn_readdir  p_readdir;
static fn_closedir p_closedir;
static fn_access   p_access;

static int resolved = 0;
static volatile int saw_main_display = 0;

/* FIX (2026-10-15): the altScreen ENABLEMENT must be consistent with the altScreen
 * ADVERTISEMENT.  iOS enforces this: if the control-SETUP response claims
 * enabledFeatures ∋ "altScreen" it requires >=2 entries in the /info displays
 * array, and if the flag is absent it requires <=1 (see the reference receiver
 * luka-dev/mib2q-carplay-rgi: "_carEndpoint_setupStreams" count test).  Telling
 * iOS "I have an alt screen" while advertising ONE display makes iOS refuse the
 * session -- the observed "session does not come up".
 *
 * So the cluster is a single, process-wide decision: the display is advertised by
 * the AddScreenDisplay seam (gated MIB3_SHIM_ADD) and that sets this flag; the
 * SETUP seam injects "altScreen" ONLY when this flag is set.  No flag -> no
 * cluster -> no altScreen claim -> the response is byte-identical to stock. */
static volatile int g_cluster_advertised = 0;
static volatile int g_info_completed = 0;   /* set when the /info seam completed a cluster advert */

/* ---- the 2026-10-16 redteam findings this build carries -------------------
 * 1) THE WALL IS OUR OWN KERNEL FIREWALL, not the phone. The unit's `network`
 *    daemon applies iptables/ip6tables-restore from an embedded template with
 *    base policy INPUT=DROP and a per-interface allowlist (recovered by strings
 *    from lge/app_ro/network/network, section literally commented
 *    "## carplay for ncm0 (usb interface)"):
 *        -A GENERIC_PORT_IN_FILTER -i ncm0 -p tcp --dport 5000:5001 -j ACCEPT
 *        -A GENERIC_PORT_IN_FILTER -i ncm0 -p tcp --dport 6030    -j ACCEPT
 *        -A GENERIC_PORT_IN_FILTER -i ncm0 -p tcp --dport 7000    -j ACCEPT
 *    (plus UDP 5001/5020/5353/6000/6003/6011; the same set for wlap/wlap2/wlap5;
 *    both IPv4 and IPv6 tables; 80/443/49200:49499 are all-interface globals).
 *    NO rule allows 6031 or any ephemeral port on the phone-facing interface,
 *    so the phone's SYN to our advertised dataPort was silently DROPPED --
 *    which is exactly the observed live behaviour: the phone offers 111, we
 *    answer {type:111,dataPort}, and the phone never connects (its SYN never
 *    reaches our listener; no RST is ever sent either, so the phone just
 *    times out and the session stays up).  The phone's complete observed
 *    socket set to the car {7000, 5001, 6030} is EXACTLY the ncm0 TCP
 *    allowlist -- and the one historical case where the phone DID connect to
 *    an advertised 111 port was the 6030-collision era (6030 IS allowlisted).
 *    CONSEQUENCE: "port number" was never actually tested -- every port the
 *    knobs offered (6031, ephemeral) is equally firewall-dropped, which made
 *    the port ablation look like "no effect".  Fix = bind 5000 (inside the
 *    5000:5001 allowlist; observed free while a phone is plugged: the stock
 *    event listener is 5001 and nothing else listens on 5000) with 6031 and
 *    ephemeral as fallbacks, and live_kit/cluster_port_fw.sh to insert the
 *    RAM-only iptables/ip6tables rule when the fallbacks are used.
 * 2) enabledFeatures is scoped to the CONTROL SETUP (the request carries the
 *    "features" proposal key; media/stream SETUPs carry "streams" only), and
 *    we claim ONLY altScreen+viewAreas -- the rgi reference's proven set.
 *    iAPChannel is opt-in (MIB3_SHIM_EF_IAPCHANNEL=1): each string in the
 *    array flips a phone-side endpoint flag, so claim nothing the stock unit
 *    does not implement.
 * 3) Part B fixes vs the stock wire format (corpus ProcessFrames 0x11fbf0):
 *    chacha20 body length is bodySize-16 (last 16 bytes are the Poly1305 tag)
 *    and MUST be followed by chacha20_poly1305_verify(ctx, dst+n, src+bs-16,
 *    &ok) with n+m == bs-16; AES-CTR is a PERSISTENT keystream (AES_CTR_Init
 *    once at SetSecurityInfo; Update per frame -- re-init per frame garbles
 *    every frame after the first); the chacha nonce is the 8-byte LE counter
 *    (init_64x64 reads 8 bytes, not 12); AVCC must be rewritten to Annex-B
 *    (4-byte length -> 00 00 00 01) and the config parsed for SPS/PPS.
 * 4) The old receiver read bodies into a static 64 KiB buffer while
 *    bounds-checking only 8 MiB -- any IDR > 64 KiB overran static memory
 *    (a crash-the-unit bug waiting for the first successful connect).
 *    Bodies now go to a heap buffer that grows to the frame size.
 * 5) Once the phone connects it sends opcode-1 (avcC) + opcode-5 telemetry
 *    but NO video until the CAR drives the scene: forceKeyFrame{params:{uuid}}
 *    at stream start, then showUI{params:{uuid,url}} (+ setNightMode) -- the
 *    rgi/LIVI command set, shipped via the stock AirPlayReceiverSessionSendCommand
 *    (dispatch_sync onto the session's own control queue; caller CFReleases the
 *    cmd dict after it returns -- stock ForceKeyFrame pattern, corpus 0x1e31c).
 * ------------------------------------------------------------------------ */

/* the cluster uuid we advertised (same string goes into /info and into the
 * showUI/forceKeyFrame/setNightMode command params -- CarKit matches it) */
static char g_cluster_uuid_str[64];
static volatile long g_night_mode = 0;     /* the advertised nightMode (for setNightMode) */
/* session that owns the live cluster stream (command target for showUI etc.) */
static void *volatile g_cmd_session = 0;
/* COMMAND GENERATION (2026-10-19, view-area lane).  Bumped every time the
 * command target changes (cluster_setup for a new type-111 SETUP, and the
 * harness entry point).  A completion callback compares the generation it was
 * issued under with this value and DROPS (does not apply/log as current) a reply
 * that belongs to a superseded session.  Same idea as cl_thread_gen and
 * g_lsock_gen -- deliberately a THIRD, independent token, because it guards a
 * third thing (the RTSP command channel), and the three can disagree. */
static volatile unsigned g_cmd_gen = 0;
/* The view-area index we believe the PHONE has applied, and the index we last
 * REQUESTED.  g_va_current moves only on a status-0 completion (never on send),
 * so a refused switch cannot strand the display in a state we think we are in. */
static volatile int g_va_current = 0;

#define CLUSTER_URL_DEFAULT "maps:/car/instrumentcluster/map"
static char g_cluster_url[160];            /* showUI url (== the advertised initialURL) */

#define K128 (0x08000100)   /* kCFStringEncodingUTF8 */

static void resolve(void){
  if (resolved) return;
  resolved = 1;
  p_CFStringCreateWithCString = (fn_CFStringCreateWithCString)R("CFStringCreateWithCString");
  p_CFStringGetCString        = (fn_CFStringGetCString)       R("CFStringGetCString");
  p_CFArrayGetCount           = (fn_CFArrayGetCount)          R("CFArrayGetCount");
  p_CFArrayGetValueAtIndex    = (fn_CFArrayGetValueAtIndex)   R("CFArrayGetValueAtIndex");
  p_CFArrayAppendValue        = (fn_CFArrayAppendValue)       R("CFArrayAppendValue");
  p_CFArrayAppendInt64        = (fn_CFArrayAppendInt64)       R("CFArrayAppendInt64");
  p_CFDictionarySetValue      = (fn_CFDictionarySetValue)     R("CFDictionarySetValue");
  p_CFDictionaryGetValue      = (fn_CFDictionaryGetValue)     R("CFDictionaryGetValue");
  p_CFShow                    = (fn_CFShow)                   R("CFShow");
  p_CFRelease                 = (fn_CFRelease)                R("CFRelease");
  p_CFDictionarySetInt64      = (fn_CFDictionarySetInt64)     R("CFDictionarySetInt64");
  p_CFDictionaryCreateMutable = (fn_CFDictionaryCreateMutable)R("CFDictionaryCreateMutable");
  p_CFDictionaryGetInt64      = (fn_CFDictionaryGetInt64)     R("CFDictionaryGetInt64");
  p_CFGetTypeID               = (fn_CFGetTypeID)              R("CFGetTypeID");
  p_CFArrayGetTypeID          = (fn_CFTypeGetTypeID)          R("CFArrayGetTypeID");
  p_CFDictionaryGetTypeID     = (fn_CFTypeGetTypeID)          R("CFDictionaryGetTypeID");
  p_CFStringGetTypeID         = (fn_CFTypeGetTypeID)          R("CFStringGetTypeID");
  p_CFNumberGetTypeID         = (fn_CFTypeGetTypeID)          R("CFNumberGetTypeID");
  p_CFBooleanGetTypeID        = (fn_CFTypeGetTypeID)          R("CFBooleanGetTypeID");
  p_CFDictionaryGetCount      = (fn_CFDictionaryGetCount)     R("CFDictionaryGetCount");
  p_CFDictionaryCopyKeys      = (fn_CFDictionaryCopyKeys)     R("CFDictionaryCopyKeys");
  p_CFGetInt64                = (fn_CFGetInt64)               R("CFGetInt64");
  /* cluster-stream helpers (libAirPlay / libCoreUtils exports) */
  p_ServerSocketOpen  = (fn_ServerSocketOpen) R("ServerSocketOpen");
  p_NetSocketCreate   = (fn_NetSocketCreate)  R("NetSocket_CreateWithNative");
  p_NetSocketNative   = (fn_NetSocketNative)  R("NetSocket_GetNative");
  p_SocketAccept      = (fn_SocketAccept)     R("SocketAccept");
  p_SocketSetQoS      = (fn_SocketSetQoS)     R("SocketSetQoS");
  p_ScreenCreate      = (fn_ScreenCreate)     R("AirPlayReceiverSessionScreen_Create");
  p_ScreenSetup       = (fn_ScreenSetup)      R("AirPlayReceiverSessionScreen_Setup");
  p_ScreenStart       = (fn_ScreenStart)      R("AirPlayReceiverSessionScreen_StartSession");
  p_ScreenStop        = (fn_ScreenStop)       R("AirPlayReceiverSessionScreen_StopSession");
  p_ScreenDelete      = (fn_ScreenDelete)     R("AirPlayReceiverSessionScreen_Delete");
  p_ScreenProcFrames  = (fn_ScreenProcFrames) R("AirPlayReceiverSessionScreen_ProcessFrames");
  p_ScreenSetSec      = (fn_ScreenSetSec)     R("AirPlayReceiverSessionScreen_SetSecurityInfo");
  p_ScreenSetChaCha   = (fn_ScreenSetChaCha)  R("AirPlayReceiverSessionScreen_SetChaChaSecurityInfo");
  p_ScreenSetTS       = (fn_ScreenSetTS)      R("AirPlayReceiverSessionScreen_SetTimeSynchronizer");
  p_DeriveAES         = (fn_DeriveAES)        R("AirPlay_DeriveAESKeySHA512ForScreen");
  p_cx_init           = (fn_cx_init)          R("chacha20_poly1305_init_64x64");
  p_cx_aad            = (fn_cx_aad)           R("chacha20_poly1305_add_aad");
  p_cx_dec            = (fn_cx_dec)           R("chacha20_poly1305_decrypt");
  p_cx_ver            = (fn_cx_ver)           R("chacha20_poly1305_verify");
  p_ctr_init          = (fn_ctr_init)         R("AES_CTR_Init");
  p_ctr_upd           = (fn_ctr_upd)          R("AES_CTR_Update");
  p_pthreadCreate     = (fn_pthreadCreate)    R("pthread_create");
  p_close             = (fn_close)            R("close");
  p_malloc            = (void*(*)(unsigned long)) R("malloc");
  p_free              = (void(*)(void*))          R("free");
  p_realSetSurface    = (fn_SetSurface)       R("_Z29CarPlayAppFrameworkSetSurfaceP26CarPlayAppFrameworkPrivatePvj");
  /* the framework's per-stream frame entry -- the interception seam (see below).
   * On the unit this is libCarPlayMediaFramework.so; on the desk bench it is the
   * stub preloaded AFTER the shim (LD_PRELOAD="mib3shim.so libstubgfx.so"). */
  p_realSSPD          = (fn_ScreenStreamProcessData) R("ScreenStreamProcessData");
  /* the callbacks structs are data: dlsym returns their addresses.
   * NB: the unit's CoreFoundation Lite exports kCFL* callbacks -- the Apple
   * kCFTypeArrayCallBacks name does NOT exist here (that was a bug). */
  p_kCFTypeArrayCallBacks     = (void*)                        R("kCFLArrayCallBacksCFLTypes");
  p_kCFLDictKeyCB             = (void*)                        R("kCFLDictionaryKeyCallBacksCFLTypes");
  p_kCFLDictValCB             = (void*)                        R("kCFLDictionaryValueCallBacksCFLTypes");
  p_kCFLBooleanTrue           = (void*)                        R("kCFLBooleanTrue");
  p_kCFLBooleanFalse          = (void*)                        R("kCFLBooleanFalse");
  real_AddScreenDisplay       = (fn_AddScreenDisplay)          R("AirPlayInfoArrayAddScreenDisplay");
  real_SessionSetup           = (fn_SessionSetup)              R("AirPlayReceiverSessionSetup");
  real_CopyServerInfo         = (fn_CopyServerInfo)            R("AirPlayCopyServerInfo");
  p_SendCommand               = (fn_SendCommand)               R("AirPlayReceiverSessionSendCommand");

  /* runtime gate: stick-marker scan (opendir/readdir/access from libc) */
  p_opendir  = (fn_opendir)  R("opendir");
  p_readdir  = (fn_readdir)  R("readdir");
  p_closedir = (fn_closedir) R("closedir");
  p_access   = (fn_access)   R("access");

  /* libAirPlay load base, for the few UNEXPORTED internal helpers we must call
   * by RVA (chacha key derive, screen time-sync callbacks).  R8830L-specific. */
  if (real_AddScreenDisplay) g_libap_base = (char*)real_AddScreenDisplay - 0x1f670;

  logline("init","resolved");
  logkv("init","p_CFArrayAppendValue",(u64)p_CFArrayAppendValue,1);
  logkv("init","p_kCFTypeArrayCallBacks",(u64)p_kCFTypeArrayCallBacks,1);
  logkv("init","p_CFDictionarySetInt64",(u64)p_CFDictionarySetInt64,1);
  logkv("init","p_CFDictionaryCreateMutable",(u64)p_CFDictionaryCreateMutable,1);
  logkv("init","real_AddScreenDisplay",(u64)real_AddScreenDisplay,1);
  logkv("init","real_SessionSetup",(u64)real_SessionSetup,1);
  logkv("init","real_CopyServerInfo",(u64)real_CopyServerInfo,1);
  logkv("init","p_realSSPD",(u64)p_realSSPD,1);
  logkv("init","p_SendCommand",(u64)p_SendCommand,1);
}

static int env_on(const char *name, int dflt){
  char *v = getenv(name);
  if(!v || !v[0]) return dflt;
  return (v[0]=='1' || v[0]=='y' || v[0]=='Y' || v[0]=='t' || v[0]=='T');
}
static u64 env_num(const char *name, u64 dflt){
  char *v = getenv(name); if(!v) return dflt;
  u64 r=0; int any=0; while(*v>='0'&&*v<='9'){ r=r*10+(*v-'0'); v++; any=1; }
  return any?r:dflt;
}

/* ---- the runtime gate ---------------------------------------------------- */
/* Does <dir>/PART1/mib3shim.enable exist?  dir is a /tmp/USB/PORTn path. */
static int marker_at(const char *dir){
  char p[256]; unsigned long n = 0;
  const char *a = dir;            while (*a && n < sizeof(p)-32) p[n++] = *a++;
  const char *b = "/PART1/mib3shim.enable"; while (*b && n < sizeof(p)-1) p[n++] = *b++;
  p[n] = 0;
  return (p_access && p_access(p, 0 /*F_OK*/) == 0);
}
/* Scan /tmp/USB/PORT<n>/PART1/mib3shim.enable.  Returns 1 if any marker exists.
 * (The port number is the physical USB port and varies -- hence the scan.)
 * If opendir is unavailable, fall back to a fixed candidate list. */
static int marker_present(void){
  if (p_opendir && p_readdir){
    void *d = p_opendir("/tmp/USB");
    if (d){
      int found = 0; mib3_dirent *e;
      while (!found && (e = p_readdir(d))){
        char dir[256]; unsigned long n = 0;
        const char *base = "/tmp/USB/"; while (*base && n < sizeof(dir)-1) dir[n++] = *base++;
        const char *nm = e->d_name;     while (*nm && n < sizeof(dir)-2) dir[n++] = *nm++;
        dir[n] = 0;
        if (e->d_name[0]=='.' && (e->d_name[1]==0 || (e->d_name[1]=='.' && e->d_name[2]==0))) continue;
        if (marker_at(dir)) found = 1;
      }
      if (p_closedir) p_closedir(d);
      if (found) return 1;
    }
  }
  /* fixed fallback (the observed live port was PORT2; the VM uses PORT1) */
  { int i; for (i = 1; i <= 8; i++){
      char dir[32]; unsigned long n = 0;
      const char *base = "/tmp/USB/PORT"; while (*base && n < sizeof(dir)-1) dir[n++] = *base++;
      char nb[4]; int k = 0; if (i>=10){ nb[k++]='0'+i/10; } nb[k++]='0'+i%10; nb[k]=0;
      const char *t = nb; while (*t && n < sizeof(dir)-1) dir[n++] = *t++;
      dir[n]=0;
      if (marker_at(dir)) return 1;
  } }
  return 0;
}
/* The activation gate consulted by the seams.  MIB3_SHIM_GATE is a TEST HOOK
 * (0/1 forces the answer); unset -> the marker scan. */
static int gate_active(void){
  char *g = getenv("MIB3_SHIM_GATE");
  if (g && g[0]=='0') return 0;
  if (g && g[0]=='1') return 1;
  return marker_present();
}

static void log_uuid(const char *tag, CFStringRef s){
  char tmp[128]; tmp[0]=0;
  if (s && p_CFStringGetCString && p_CFStringGetCString(s,tmp,sizeof(tmp),K128))
    logline(tag,tmp);
  else
    logkv(tag,"uuid(ptr)",(u64)s,1);
}

/* ==========================================================================
 * Post-process the just-appended cluster display dict.
 *
 * Apple's AirPlayInfoArrayAddScreenDisplay only writes 8 keys (uuid, features,
 * primaryInputDevice, maxFPS, width/heightPixels, width/heightPhysical).  It
 * has NO "type" parameter -- so a second call alone yields a *typeless* display
 * that iOS treats as another main screen, never as the instrument cluster.
 * The reference receivers (PlayPort/LIVI) additionally set, on the cluster
 * entry: type=111, initialURL, and a viewAreas[] (with a safeArea).  We add
 * those here, on the mutable dict the real function just appended.
 * ========================================================================*/
static CFMutableDictionaryRef last_dict(CFMutableArrayRef *inArray){
  if (!inArray || !*inArray || !p_CFArrayGetCount || !p_CFArrayGetValueAtIndex) return 0;
  CFIndex n = p_CFArrayGetCount(*inArray);
  if (n < 1) return 0;
  return (CFMutableDictionaryRef)p_CFArrayGetValueAtIndex(*inArray, n-1);
}
static CFStringRef sstr(const char *s){ return p_CFStringCreateWithCString ? p_CFStringCreateWithCString(0,s,K128) : 0; }

/* set-and-release helpers.  The KEY strings (and, for dsetv, the VALUE string)
 * have +1 from CFStringCreateWithCString; the dict retains them, so we release
 * our own reference immediately.  The previous code created a fresh key with
 * sstr() at every call site and never released it -- one leaked CFString per key
 * on every cluster advertisement.  Values handed to dseto are BORROWED by the
 * caller (already +1 elsewhere or a CF constant) and are NOT released here. */
static void dseti(CFMutableDictionaryRef d, const char *key, s64 v){
  if (!d || !p_CFDictionarySetInt64) return;
  CFStringRef k = sstr(key);
  if (k){ p_CFDictionarySetInt64(d, k, v); if (p_CFRelease) p_CFRelease(k); }
}
static void dsetv(CFMutableDictionaryRef d, const char *key, const char *val){
  if (!d || !p_CFDictionarySetValue) return;
  CFStringRef k = sstr(key), v = sstr(val);
  if (k && v) p_CFDictionarySetValue(d, k, v);
  if (p_CFRelease){ if (v) p_CFRelease(v); if (k) p_CFRelease(k); }
}
static void dseto(CFMutableDictionaryRef d, const char *key, const void *obj){
  if (!d || !p_CFDictionarySetValue || !obj) return;
  CFStringRef k = sstr(key);
  if (k){ p_CFDictionarySetValue(d, k, obj); if (p_CFRelease) p_CFRelease(k); }
}
/* get helpers that create+release the key (the old call sites leaked one
 * CFString per read).  dgetk returns a BORROWED value (the dict owns it). */
static void *dgetk(CFDictionaryRef d, const char *key){
  if (!d || !p_CFDictionaryGetValue) return 0;
  CFStringRef k = sstr(key); void *v = 0;
  if (k){ v = (void*)p_CFDictionaryGetValue(d, k); if (p_CFRelease) p_CFRelease(k); }
  return v;
}
static s64 dgeti64k(CFDictionaryRef d, const char *key, s64 dflt){
  if (!d || !p_CFDictionaryGetValue || !p_CFDictionaryGetInt64) return dflt;
  CFStringRef k = sstr(key); s64 v = dflt;
  if (k){ if (p_CFDictionaryGetValue(d, k)) v = p_CFDictionaryGetInt64(d, k, 0);
          if (p_CFRelease) p_CFRelease(k); }
  return v;
}

/* ==========================================================================
 * VIEW AREAS (2026-10-19, view-area lane).
 *
 * WHY.  The instrument cluster has a VIEW button that cycles its layout: one
 * layout is a full-panel map, another is a two-gauge layout with the map in the
 * middle.  When the driver picks the gauge layout, the CLUSTER ECU paints the
 * gauges over the video we deliver -- and the PHONE never learns that its window
 * shrank, because we advertise exactly ONE view area (the full 1280x480 panel)
 * and never send updateViewArea.  So Apple Maps keeps drawing as if it owned the
 * whole panel, and a large part of it disappears behind the gauges.
 *
 * WHAT THE PHONE NEEDS.  The /info display entry carries `viewAreas` (an array of
 * {originXPixels,originYPixels,widthPixels,heightPixels,safeArea?}),
 * `initialViewArea` (an index) and `adjacentViewAreas`.  The phone bounds-checks
 * an updateViewArea index against THAT array, so a second area must be
 * PREDECLARED before we can switch to it.  Two-part change:
 *   (a) declare N areas here, and
 *   (b) switch between them with updateViewArea (cluster_send_viewarea).
 *
 * SHAPE CHOSEN (and why).  Per refs/rgi -- verified on real hardware,
 * docs/cluster-and-rgi/KDK_GEOMETRY_AND_ANIMATION.md + THIRD_PARTY_NAV_APPS.md --
 * EVERY declared area keeps the FULL encoded/visible frame and the areas differ
 * only in the NESTED `safeArea`.  rgi measured that an area whose *visible* frame
 * was reduced got the stream rejected at activation (root cause unknown); keeping
 * the visible frame full avoided that.  Our default follows that proven shape,
 * with MIB3_SHIM_VA_VISIBLE=1 as the deliberate experiment for the trip.
 *
 * KNOBS:
 *   MIB3_SHIM_VIEWAREAS=1        declare multiple view areas (default: off ->
 *                                the single full-panel area, unchanged)
 *   MIB3_SHIM_VA_COUNT=n         how many (default 3: this car has three layouts)
 *   MIB3_SHIM_VA_PRESET=1        derive class-safe boxes from CarPlay's own
 *                                classifier (default ON; see the CLASSIFIER block).
 *                                VA_PRESET=0 restores the old full-panel/guess.
 *   MIB3_SHIM_VA<i>="x,y,w,h[,sx,sy,sw,sh]"  explicit rects for area i (wins over
 *                                the preset)
 *   MIB3_SHIM_VA_VISIBLE=1       also shrink the *visible* frame of areas >0 to
 *                                their safe rect (the rgi-refuted shape)
 *   MIB3_SHIM_VA_NOOUTSIDE=1     omit drawUIOutsideSafeArea from safeArea
 *   MIB3_SHIM_VA_CMD=0           do not ship updateViewArea on a switch
 * ========================================================================*/
typedef struct { int x, y, w, h; int sx, sy, sw, sh; } va_rect;

/* LEGACY safe rect for the gauge layout (VA_PRESET=0 only; the preset derives a
 * class-safe box from CarPlay's classifier instead).  This one was a GUESS: it
 * is centred -- (1280-480)/2 = 400 and (480-400)/2 = 40 -- and 480 px wide so that CarPlay's
 * CPSInstrumentClusterCardLayout does not classify it "narrow" -- rgi measured
 * that threshold at ~498 px on a 1440 px panel = 34.6% of the width, and 34.6%
 * of 1280 is 443, so 480 clears it.  400 px tall clears rgi's other limit
 * (~290 px for the centred card).  It is NOT measured on the Arteon. */
#define VA_GAUGE_SAFE "400,40,480,400"

static int va_parse_rect(const char *s, va_rect *r)
{
  int v[8]; int n = 0; int any = 0;
  if (!s || !r) return 0;
  for (;;){
    while (*s == ' ' || *s == '\t' || *s == ',') s++;
    if (*s < '0' || *s > '9') break;
    { int x = 0; any = 1; while (*s >= '0' && *s <= '9'){ x = x*10 + (*s - '0'); s++; } v[n++] = x; }
    if (n >= 8) break;
  }
  if (n < 4 || !any) return 0;
  r->x = v[0]; r->y = v[1]; r->w = v[2]; r->h = v[3];
  if (n >= 8){ r->sx = v[4]; r->sy = v[5]; r->sw = v[6]; r->sh = v[7]; }
  else { r->sx = r->x; r->sy = r->y; r->sw = r->w; r->sh = r->h; }
  return 1;
}
static int va_count(void)
{
  /* 3 by default: this car's cluster cycles through THREE layouts, and one view
   * area per layout is the whole point of the multi-area path (MIB3_SHIM_VA_COUNT
   * overrides; the single-area path used by the proven config is untouched). */
  long n = (long)env_num("MIB3_SHIM_VA_COUNT", 3);
  if (n < 1) n = 1;
  if (n > 8) n = 8;
  return (int)n;
}
/* ==========================================================================
 * THE CLASSIFIER CarPlay applies to our safe box (desk reconstruction,
 * 2026-10-19, from rgi's decompile of
 * -[CPSInstrumentClusterCardViewController initWithSafeAreaFrame:…physicalPixelWidth:]
 * plus the DashBoard scene path).
 *
 * A safe box is NOT a crop and NOT a scale -- it is a CLASSIFICATION INPUT.  The
 * phone picks one of three card designs from its width and height:
 *
 *   scale   = 0.153 mm / (physicalWidthMm / pixelWidth)
 *   wide    : safeW >= 660*scale   -> 213 px card column at the box's L/R edge
 *   centred : safeH >  383*scale   -> centred platter (width clamped 165..300)
 *   else    : LAYOUT 0, "will not show cluster content" -- a BLANK cluster, ETA
 *             included.  There is no graceful degradation.
 *   and, independently: ETA is dropped unless safeH > 202 px.
 *
 * For this panel (1280 px at the advertised 292 mm) scale = 0.670, so
 * wide >= 443 px and centred needs safeH > 257 px.  BOTH cliffs move with the
 * advertised mm (a true 278 mm panel puts wide at >= 465 px), so a preset box
 * keeps a margin rather than sitting on a threshold.
 * ========================================================================*/
static int va_scale_milli(int cw, int phy_mm){
  if (phy_mm < 50 || phy_mm > 2000) phy_mm = 292;   /* absurd -> the default */
  if (cw < 100) cw = 1280;
  return (int)((153L * (long)cw) / (long)phy_mm);
}
static int va_wide_px(int cw, int phy_mm){ return (int)((660L * (long)va_scale_milli(cw, phy_mm)) / 1000L); }
static int va_centre_px(int cw, int phy_mm){ return (int)((383L * (long)va_scale_milli(cw, phy_mm)) / 1000L) + 1; }
/* 0 = undefined (blank), 1 = centred, 2 = wide.  Which SIDE a wide column takes
 * is the phone's choice (safe-area mid vs display mid), so it is not modelled. */
static int va_class_of(int sw, int sh, int cw, int phy_mm){
  if (sw <= 0 || sh <= 0) return 0;
  if (sw >= va_wide_px(cw, phy_mm)) return 2;
  if (sh >  va_centre_px(cw, phy_mm)) return 1;
  return 0;
}
/* 1 when the box clears its cliff by <= 10% -- i.e. do not trust it on the car. */
static int va_class_near_cliff(int sw, int sh, int cw, int phy_mm){
  int cls = va_class_of(sw, sh, cw, phy_mm);
  if (cls == 2 && sw < va_wide_px(cw, phy_mm) * 110 / 100) return 1;
  if (cls == 1 && sh < va_centre_px(cw, phy_mm) * 110 / 100) return 1;
  return 0;
}
/* Class-safe boxes for the THREE cluster layouts, DERIVED from the classifier
 * rather than invented: each box is placed well inside the class it must land
 * in, with margin against the mm error that moves the cliffs.
 *
 * These are PLACEHOLDERS in POSITION only -- the CLASS is deliberate, the
 * COORDINATES still have to be measured against the car's own clear window
 * (MIB3_RE/LANE_CLUSTER_LAYOUTS_2026-10-19.md, calibration grid).  Override any
 * of them with MIB3_SHIM_VA<i>="x,y,w,h,sx,sy,sw,sh"; MIB3_SHIM_VA_PRESET=0
 * restores the old full-panel/guess behaviour. */
static void va_preset_rect(int i, int cw, int chp, va_rect *r)
{
  int phy = (int)env_num("MIB3_SHIM_CWPHY", 292);
  int wt  = va_wide_px(cw, phy);
  int ct  = va_centre_px(cw, phy);
  /* The visible frame stays the FULL panel: reducing it got the 111 stream
   * rejected at activation (rgi, measured). */
  r->x = 0; r->y = 0; r->w = cw; r->h = chp;
  if (i == 0){
    /* full-map layout: WIDE, centred, generous margins */
    int sw = cw * 5 / 8, sh = chp * 7 / 12;
    if (sw < wt + wt / 3) sw = wt + wt / 3;
    if (sh < 242) sh = 242;                     /* ETA floor + margin */
    if (sw > cw) sw = cw;
    if (sh > chp) sh = chp;
    r->sx = (cw - sw) / 2; r->sy = (chp - sh) / 2; r->sw = sw; r->sh = sh;
  } else if (i == 1){
    /* dual-gauge layout: the card must sit BETWEEN the dials, so this box is
     * deliberately BELOW the wide cliff -> centred class. */
    int sw = wt * 9 / 10, sh = ct * 5 / 4;
    if (sh < 253) sh = 253;
    if (sh > chp - 20) sh = chp - 20;
    if (sw < 165) sw = 165;
    if (sw > cw) sw = cw;
    r->sx = (cw - sw) / 2; r->sy = (chp - sh) / 2; r->sw = sw; r->sh = sh;
  } else {
    /* third layout (unmeasured): WIDE with a real margin, offset left so the
     * wide-side branch is exercised. */
    int sw = cw * 47 / 100, sh = chp * 62 / 100;
    if (sw < wt + wt / 3) sw = wt + wt / 3;
    if (sh < 242) sh = 242;
    if (sw > cw) sw = cw;
    if (sh > chp) sh = chp;
    r->sx = cw / 20; r->sy = (chp - sh) / 2; r->sw = sw; r->sh = sh;
  }
}
/* Fill rect i (1-based env name suffix: area 0 -> MIB3_SHIM_VA0). */
static int va_rect_at(int i, int cw, int chp, va_rect *r)
{
  char nm[24]; int j = 0; const char *p = "MIB3_SHIM_VA";
  while (*p && j < (int)sizeof(nm) - 3) nm[j++] = *p++;
  nm[j++] = (char)('0' + (i % 10)); nm[j] = 0;
  { const char *s = getenv(nm); if (s && s[0] && va_parse_rect(s, r)) return 1; }
  if (env_on("MIB3_SHIM_VA_PRESET",1)){ va_preset_rect(i, cw, chp, r); return 1; }
  if (i == 0){                                     /* area 0 = the full panel */
    r->x = 0; r->y = 0; r->w = cw; r->h = chp;
    r->sx = 0; r->sy = 0; r->sw = cw; r->sh = chp;
    return 1;
  }
  { va_rect g; va_parse_rect(VA_GAUGE_SAFE, &g);   /* provisional default */
    r->x = 0; r->y = 0; r->w = cw; r->h = chp;
    r->sx = g.sx; r->sy = g.sy; r->sw = g.sw; r->sh = g.sh;
    /* clamp to the real panel so an odd size can never yield a safeArea that
     * lies outside the display */
    if (r->sx < 0) r->sx = 0;
    if (r->sy < 0) r->sy = 0;
    if (r->sw > cw - r->sx) r->sw = cw - r->sx;
    if (r->sh > chp - r->sy) r->sh = chp - r->sy;
  }
  return 1;
}
/* One area dict: visible rect + nested safeArea (+ optional
 * drawUIOutsideSafeArea, the same CFBoolean-true object the single-area path
 * uses today).  Returns +1 owned by the caller. */
static CFMutableDictionaryRef va_build_one(const va_rect *r)
{
  CFMutableDictionaryRef area, safe;
  if (!p_CFDictionaryCreateMutable || !p_kCFLDictKeyCB || !p_kCFLDictValCB) return 0;
  area = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
  safe = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
  if (!area || !safe){
    if (p_CFRelease){ if (safe) p_CFRelease(safe); if (area) p_CFRelease(area); }
    return 0;
  }
  dseti(area, "widthPixels",  (s64)r->w);
  dseti(area, "heightPixels", (s64)r->h);
  dseti(area, "originXPixels",(s64)r->x);
  dseti(area, "originYPixels",(s64)r->y);
  dseti(safe, "widthPixels",  (s64)r->sw);
  dseti(safe, "heightPixels", (s64)r->sh);
  dseti(safe, "originXPixels",(s64)r->sx);
  dseti(safe, "originYPixels",(s64)r->sy);
  if (p_kCFLBooleanTrue && !env_on("MIB3_SHIM_VA_NOOUTSIDE",0)){
    void *btrue = *(void **)p_kCFLBooleanTrue;
    if (btrue) dseto(safe, "drawUIOutsideSafeArea", btrue);
  }
  dseto(area, "safeArea", safe);
  if (p_CFRelease) p_CFRelease(safe);
  return area;
}
/* one "visible=x,y,w,h safe=x,y,w,h" line, no allocation, then the CLASSIFIER
 * verdict for this box (see the CLASSIFIER block: 0 = blank cluster). */
static void va_log_rect(int i, const va_rect *r, int cw)
{
  char b[160]; int k = 0; int v[8]; int q, j;
  v[0]=r->x; v[1]=r->y; v[2]=r->w; v[3]=r->h;
  v[4]=r->sx; v[5]=r->sy; v[6]=r->sw; v[7]=r->sh;
  for (q = 0; q < 2; q++){
    const char *t = q ? " safe=" : "visible=";
    while (*t) b[k++] = *t++;
    for (j = 0; j < 4; j++){
      int x = v[q*4 + j]; char d[12]; int m = 0;
      if (x < 0) x = 0;
      if (!x) d[m++] = '0';
      while (x){ d[m++] = (char)('0' + (x % 10)); x /= 10; }
      if (j) b[k++] = ',';
      while (m) b[k++] = d[--m];
    }
  }
  b[k] = 0;
  logkv("cluster/va","area index",(u64)(u32)i,0);
  logline("cluster/va/rect", b);
  /* the phone's own verdict on this box, computed BEFORE the session so the car
   * tells us what it will draw instead of us guessing from the picture */
  { int phy = (int)env_num("MIB3_SHIM_CWPHY", 292);
    int cls = va_class_of(r->sw, r->sh, cw, phy);
    logkv("cluster/va","class (0=BLANK 1=centred 2=wide)",(u64)(u32)cls,0);
    logkv("cluster/va","  safeW",(u64)(u32)r->sw,0);
    logkv("cluster/va","  safeH",(u64)(u32)r->sh,0);
    logkv("cluster/va","  wideThrPx",(u64)(u32)va_wide_px(cw, phy),0);
    logkv("cluster/va","  centreThrPx(safeH)",(u64)(u32)va_centre_px(cw, phy),0);
    logkv("cluster/va","  etaOk(safeH>202)",(u64)(unsigned)(r->sh > 202),0);
    logkv("cluster/va","  nearCliff(<=10%)",(u64)(unsigned)va_class_near_cliff(r->sw, r->sh, cw, phy),0); }
}

/* The multi-area `viewAreas` array.  Returns +1 owned by the caller.  Logs one
 * `cluster/va/area` line per declared area (new lines only -- the existing
 * "viewAreas added" meaning is untouched). */
static CFMutableArrayRef va_build(int cw, int chp)
{
  fn_CFArrayCreateMutable mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
  CFMutableArrayRef va;
  int n = va_count(), i;
  if (!mka || !p_kCFTypeArrayCallBacks || !p_CFArrayAppendValue) return 0;
  va = mka(0, n, p_kCFTypeArrayCallBacks);
  if (!va) return 0;
  for (i = 0; i < n; i++){
    va_rect r;
    CFMutableDictionaryRef a;
    if (!va_rect_at(i, cw, chp, &r)) continue;
    /* MIB3_SHIM_VA_VISIBLE=1: use the safe rect as the visible frame too (the
     * shape rgi measured as rejected at activation -- a deliberate trip test). */
    if (i > 0 && env_on("MIB3_SHIM_VA_VISIBLE",0)){
      r.x = r.sx; r.y = r.sy; r.w = r.sw; r.h = r.sh;
    }
    a = va_build_one(&r);
    if (!a) continue;
    p_CFArrayAppendValue(va, a);       /* array retains */
    if (p_CFRelease) p_CFRelease(a);
    va_log_rect(i, &r, cw);
  }
  logkv("cluster/va","count declared",(u64)(unsigned)p_CFArrayGetCount(va),0);
  return va;
}

static void add_cluster_fields(CFMutableArrayRef *inArray, int cw, int chp){
  CFMutableDictionaryRef d = last_dict(inArray);
  if (!d){ logline("cluster","no dict to annotate"); return; }

  /* type = 111 (alt / instrument-cluster stream) */
  { int dtype = (int)env_num("MIB3_SHIM_TYPE", 111);
    dseti(d, "type", (s64)dtype);
    logkv("cluster","type",(u64)(u32)dtype,0); }
  /* initialURL -- the cluster scene Apple Maps should render.  THE SAME string
   * is later sent as the showUI url (g_cluster_url) -- one knob, consistent.
   * MIB3_SHIM_NOURL=1 omits it (the rgi reference's cluster dict has NO initialURL). */
  if (!env_on("MIB3_SHIM_NOURL",0)){
  { char *url = getenv("MIB3_SHIM_URL");
    const char *us = (url && url[0]) ? url : (g_cluster_url[0] ? g_cluster_url : CLUSTER_URL_DEFAULT);
    dsetv(d, "initialURL", us);
    logline("cluster/initialURL", us); }
  }
  /* viewAreas = [ { view rect + safeArea (full panel, drawUIOutsideSafeArea) } ]
   *
   * DEFAULT PATH IS UNCHANGED (byte-for-byte): one full-panel area.  With
   * MIB3_SHIM_VIEWAREAS=1, va_build() declares N areas instead (see the VIEW
   * AREAS block above).  Both paths emit the SAME "viewAreas added" line, so no
   * existing log line changes meaning. */
  if (env_on("MIB3_SHIM_VIEWAREAS",0)){
    CFMutableArrayRef va2 = va_build(cw, chp);
    if (va2){
      dseto(d, "viewAreas", va2);
      dseti(d, "initialViewArea", 0);
      logline("cluster","viewAreas added");
      if (p_CFRelease) p_CFRelease(va2);        /* dict retains it */
    }
  } else if (p_CFDictionaryCreateMutable && p_CFDictionarySetInt64 && p_CFArrayAppendValue &&
      p_CFDictionarySetValue && p_kCFTypeArrayCallBacks && p_kCFLDictKeyCB && p_kCFLDictValCB){
    fn_CFArrayCreateMutable mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
    CFMutableArrayRef va = mka ? mka(0,0,p_kCFTypeArrayCallBacks) : 0;
    if (va){
      CFMutableDictionaryRef area = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
      CFMutableDictionaryRef safe = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
      if (area && safe){
        dseti(area, "widthPixels",  (s64)cw);
        dseti(area, "heightPixels", (s64)chp);
        dseti(area, "originXPixels",(s64)0);
        dseti(area, "originYPixels",(s64)0);
        dseti(safe, "widthPixels",  (s64)cw);
        dseti(safe, "heightPixels", (s64)chp);
        dseti(safe, "originXPixels",(s64)0);
        dseti(safe, "originYPixels",(s64)0);
        if (p_kCFLBooleanTrue){
          /* kCFLBooleanTrue is a CFBooleanRef *variable* (8-byte object): deref once. */
          void *btrue = *(void **)p_kCFLBooleanTrue;
          if (btrue) dseto(safe, "drawUIOutsideSafeArea", btrue);
        }
        dseto(area, "safeArea", safe);
        p_CFArrayAppendValue(va, area);
        dseto(d, "viewAreas", va);
        dseti(d, "initialViewArea", 0);
        logline("cluster","viewAreas added");
      }
      /* release OUR +1 on area/safe (va retains area, area retains safe); the
       * shim owned them and never released them -> leak on every cluster ad (F7). */
      if (p_CFRelease){ if (safe) p_CFRelease(safe); if (area) p_CFRelease(area); }
      if (p_CFRelease) p_CFRelease(va);          /* dict retains it */
    }
  }
  if (env_on("MIB3_SHIM_CFSHOW",0) && p_CFShow) { logline("cluster","CFShow(dict) ->"); p_CFShow(d); }
}

/* ==========================================================================
 * /info COMPLETION -- the MODERN CarKit display schema (2026-10-15).
 *
 * THE GAP.  The unit's libAirPlay 320.17.4 builds `/info` in
 * AirPlayCopyServerInfo(): it copies `displays` (and `extendedFeatures`,
 * `audioFormats`, ...) from the platform dict into a CFMutableDictionary and
 * returns it -- and the caller (libAirPlay 0xcc98, the /info HTTP handler)
 * serialises that SAME dict with CFBinaryPlistV0Create.  LG's
 * CarPlayConfiguration::setDisplays fills `displays` via
 * AirPlayInfoArrayAddScreenDisplay, whose entries use the OLD 210 schema:
 * identifier key `uuid`, and NO `initialVideoStreams`.  iOS-26-era CarKit
 * (CRDisplayInfo / CARSessionConfiguration, verified against the working
 * receivers) requires the NEW schema at the display-dict level and a SEPARATE
 * top-level key `displayPanels`; a `displays` entry it cannot parse makes it
 * reject the whole array and teardown-loop the session.
 *
 * Required fields (field-by-field, from luka-dev/mib2q-carplay-rgi
 * hook/altscreen/altscreen_hook.c, itself verified against the iOS 26.1 CarKit
 * decompile -- see LANE_ADVERT_COMPLETE_2026-10-15.md for the full table):
 *   per display :  uid (CarKit initWithPhysicalScreenDictionary reads "uid";
 *                  missing -> CRDisplayInfo nil -> the whole displays array is
 *                  rejected), uuid (AirPlaySender picks 110 vs 111 by a
 *                  uuid-vs-display-0 compare), name, type, maxFPS,
 *                  width/heightPixels, width/heightPhysical, viewAreas,
 *                  initialViewArea, adjacentViewAreas,
 *                  initialVideoStreams (MUST be a present, NON-empty array or
 *                  CRDisplayInfo returns nil), plus cornerMasks / nightMode /
 *                  limitedUI / limitedUIElements;
 *   cluster only:  properties = ["Instrument Cluster"]  (-> CarKit
 *                  supportsInstrumentClusterContent),
 *   top level   :  displayPanels = the same [main,cluster] array (the
 *                  CARSessionConfiguration consumer; without it the cluster is
 *                  unrecognized and iOS never proposes altScreen), and
 *                  extendedFeatures = ["altScreen","viewAreas"] (the /info
 *                  advertisement; a SEPARATE message from the SETUP response's
 *                  `enabledFeatures`).
 *
 * THE WIRING IS PROVEN FROM THE BINARY (R8830L, disassembly):
 *   1. CarPlayConfiguration::setDisplays (libCarPlayAppFramework 0x236e4) calls
 *      AirPlayInfoArrayAddScreenDisplay(&this[0x88], ...) -- the array our first
 *      seam appends the cluster entry to.
 *   2. CarPlayServerCallbacks::copyRequiredProperty(key=="displays", fw)
 *      (0x1db88) returns [fw+0x310].  CarPlayConfiguration is the sub-object at
 *      fw+0x288 (proved by the sibling case: key "audioFormats" returns
 *      [fw+0x288] and calls CarPlayConfiguration::getDeviceID(fw+0x288)), so
 *      fw+0x310 == config[0x88] -- EXACTLY the array from (1).
 *   3. AirPlayCopyServerInfo (libAirPlay 0xe3d8-0xe3ec) calls
 *      AirPlayReceiverServerPlatformCopyProperty(server,1,"displays",..) -> (2)
 *      -> CFDictionarySetValue(info,"displays",thatArray).
 *   4. The /info HTTP handler (libAirPlay 0xcc98) serialises that dict with
 *      CFBinaryPlistV0Create.  So this seam sees the SAME array the first seam
 *      annotated.
 * Also proved: CarPlayServerCallbacks::copyExtendedFeatures (0x1dc54) BUILDS
 * `extendedFeatures` from LG flags (`enhancedRequestCarUI` @[fw+0x300],
 * `vocoderInfo` @[fw+0x3f8]) -- so on this unit the key is often PRESENT without
 * altScreen, which is why we MERGE rather than skip-if-present or clobber.
 *
 * HONEST SCOPE.  The field SET is SUPPORTED (a working reference receiver,
 * verified against the iOS 26.1 CarKit decompile), not PROVEN against this
 * unit's own libraries -- the unit's libAirPlay has no `uid`/`displayPanels`/
 * `initialVideoStreams` strings at all, so there is nothing in the binary to
 * confirm the consumer side.  What IS proven here: the seam exists and is
 * interposable (exported symbol + JUMP_SLOT relocation), the array it completes
 * is the one the /info response carries (above), and the object the shim
 * produces serialises with the unit's own CFBinaryPlistV0Create and satisfies
 * the two-display/altScreen invariant.  The car decides the rest.
 *
 * Everything is gated on the CLUSTER ENTRY ACTUALLY BEING PRESENT (type==111 in
 * `displays`): if it is not there we publish NEITHER displayPanels NOR
 * extendedFeatures, so the 2-display/altScreen contract can never be advertised
 * without the second display -- the same both-ways discipline as
 * g_cluster_advertised (see LANE_SESSION_FIX_2026-10-15.md).
 * ========================================================================*/

#define MIB3_CLUSTER_TYPE  111

/* process-lifetime key/value cache: created once, never released (bounded, and
 * /info is a cold path so this is not a hot-loop allocation). */
static CFStringRef g_k_uid, g_k_uuid, g_k_name, g_k_type, g_k_props, g_k_ivs,
                   g_k_adj, g_k_corner, g_k_night, g_k_lui, g_k_luie,
                   g_k_dp, g_k_ef, g_k_displays, g_k_ox, g_k_oy, g_k_z,
                   g_v_ic, g_v_alt, g_v_va;
static int g_info_keys_ready = 0;
static void info_keys_init(void){
  if (g_info_keys_ready) return;
  g_k_uid      = sstr("uid");
  g_k_uuid     = sstr("uuid");
  g_k_name     = sstr("name");
  g_k_type     = sstr("type");
  g_k_props    = sstr("properties");
  g_k_ivs      = sstr("initialVideoStreams");
  g_k_adj      = sstr("adjacentViewAreas");
  g_k_corner   = sstr("cornerMasks");
  g_k_night    = sstr("nightMode");
  g_k_lui      = sstr("limitedUI");
  g_k_luie     = sstr("limitedUIElements");
  g_k_dp       = sstr("displayPanels");
  g_k_ef       = sstr("extendedFeatures");
  g_k_displays = sstr("displays");
  g_k_ox       = sstr("originXPixels");
  g_k_oy       = sstr("originYPixels");
  g_k_z        = sstr("zIndex");
  g_v_ic       = sstr("Instrument Cluster");
  g_v_alt      = sstr("altScreen");
  g_v_va       = sstr("viewAreas");
  /* Only latch once the CF runtime actually returned the strings: a first /info
   * that raced a not-yet-ready runtime must retry, not latch NULL forever. */
  if (g_k_uid && g_k_uuid && g_k_name && g_k_type && g_k_props && g_k_ivs &&
      g_k_adj && g_k_corner && g_k_night && g_k_lui && g_k_luie && g_k_dp &&
      g_k_ef && g_k_displays && g_k_ox && g_k_oy && g_k_z && g_v_ic && g_v_alt &&
      g_v_va)
    g_info_keys_ready = 1;
}

static int is_array(CFTypeRef v){
  return v && p_CFGetTypeID && p_CFArrayGetTypeID &&
         (p_CFGetTypeID(v) == p_CFArrayGetTypeID());
}
static int is_dict(CFTypeRef v){
  return v && p_CFGetTypeID && p_CFDictionaryGetTypeID &&
         (p_CFGetTypeID(v) == p_CFDictionaryGetTypeID());
}
static s64 dict_i64(CFDictionaryRef d, CFStringRef k){
  if (!p_CFDictionaryGetValue || !p_CFDictionaryGetInt64 || !d || !k) return 0;
  if (!p_CFDictionaryGetValue(d, k)) return 0;          /* absent -> 0 */
  return p_CFDictionaryGetInt64(d, k, 0);
}

/* keys we dump out of any dict.  SAFE: CFDictionaryGetValue only.  We do NOT
 * use CFDictionaryCopyKeys -- on the unit's CFLite it corrupted the dict we
 * dumped (the SETUP response then failed to serialise: -6732), verified on the
 * desk rig (RESPONSE_BROKEN vs RESPONSE_WELLFORMED with it off). */
static const char *g_dump_keys[] = {
  "displays","displayPanels","extendedFeatures","streams","enabledFeatures",
  "timingPort","eventPort","keepAlivePort","features","statusFlags","model",
  "name","deviceID","uuid","uid","type","properties","primaryInputDevice",
  "maxFPS","widthPixels","heightPixels","widthPhysical","heightPhysical",
  "viewAreas","initialViewArea","adjacentViewAreas","initialVideoStreams",
  "initialURL","cornerMasks","nightMode","limitedUI","limitedUIElements","edid",
  "safeArea","originXPixels","originYPixels","zIndex","drawUIOutsideSafeArea",
  "audioFormats","audioLatencies","hidDevices","modes","sourceVersion",
  "manufacturer","bluetoothIDs","rightHandDrive","keepAliveLowPower",
  "keepAliveSendStatsAsBody","macAddress","systemBuildVersion","osVersion", 0
};
static void dump_cf(const char *tag, CFTypeRef v, int depth);
static void dump_known(const char *tag, CFDictionaryRef d){
  int i;
  for (i = 0; g_dump_keys[i]; i++){
    CFTypeRef v = dgetk(d, g_dump_keys[i]);
    if (!v) continue;
    logline(tag, g_dump_keys[i]);
    dump_cf(tag, v, 1);
  }
}

/* ---- value dumper: log a whole CF tree (bounded) so we can see exactly what
 * we hand the phone.  One log line per key/scalar, depth-indented. ---------- */
static void dump_cf(const char *tag, CFTypeRef v, int depth){
  char b[240];
  unsigned long t;
  int i;
  if (depth > 6){ logline(tag, "        ...(max depth)"); return; }
  if (!v){ logline(tag, "<null>"); return; }
  t = p_CFGetTypeID ? p_CFGetTypeID(v) : 0;
  if (p_CFStringGetTypeID && t == p_CFStringGetTypeID()){
    b[0]=0; for (i=0; i<depth && i<20; i++) b[i]=' '; b[i]=0;
    if (p_CFStringGetCString && p_CFStringGetCString((CFStringRef)v, b+i, sizeof(b)-i, K128))
      logline(tag, b);
    else logline(tag, "<string>");
    return;
  }
  if (p_CFNumberGetTypeID && t == p_CFNumberGetTypeID()){
    s64 n = p_CFGetInt64 ? (s64)p_CFGetInt64(v, 0) : 0;
    logkv(tag, p_CFGetInt64 ? "num" : "num(?)", (u64)n, 0);
    return;
  }
  if (p_CFBooleanGetTypeID && t == p_CFBooleanGetTypeID()){
    logline(tag, (p_kCFLBooleanTrue && v == *(void**)p_kCFLBooleanTrue) ? "bool=true" : "bool=false");
    return;
  }
  if (p_CFArrayGetTypeID && t == p_CFArrayGetTypeID()){
    CFIndex k, m = p_CFArrayGetCount((CFArrayRef)v);
    logkv(tag, "array[count]", (u64)m, 0);
    for (k=0;k<m && k<64;k++) dump_cf(tag, (CFTypeRef)p_CFArrayGetValueAtIndex((CFArrayRef)v,k), depth+1);
    return;
  }
  if (p_CFDictionaryGetTypeID && t == p_CFDictionaryGetTypeID()){
    CFIndex m = p_CFDictionaryGetCount ? p_CFDictionaryGetCount((CFDictionaryRef)v) : -1;
    logkv(tag, "dict[count]", (u64)m, 0);
    dump_known(tag, (CFDictionaryRef)v);   /* known keys only (GetValue) -- safe */
    return;
  }
  logline(tag, "<other type>");
}

/* Does the display list already carry a type-111 (cluster) entry?  Makes the
 * advertisement idempotent PER DISPLAY LIST.  LG rebuilds `displays` on every
 * session, so the old "add once per process" latch meant only the FIRST phone
 * plug after injection ever advertised -- and every later plug still claimed
 * altScreen while showing one display (the 1-display/altScreen contradiction
 * that kills the session).  Correct whether the array persists or is rebuilt. */
static int array_has_cluster(CFMutableArrayRef *inArray){
  if (!inArray || !*inArray) return 0;
  if (!p_CFArrayGetCount || !p_CFArrayGetValueAtIndex || !p_CFGetTypeID ||
      !p_CFDictionaryGetTypeID || !p_CFDictionaryGetValue || !p_CFDictionaryGetInt64)
    return 0;
  info_keys_init();
  if (!g_info_keys_ready || !g_k_type) return 0;
  CFArrayRef a = *inArray;
  CFIndex n = p_CFArrayGetCount(a), i;
  int want = (int)env_num("MIB3_SHIM_TYPE", MIB3_CLUSTER_TYPE);
  for (i = 0; i < n; i++){
    CFTypeRef e = p_CFArrayGetValueAtIndex(a, i);
    if (!e || p_CFGetTypeID(e) != p_CFDictionaryGetTypeID()) continue;
    if (!p_CFDictionaryGetValue((CFDictionaryRef)e, g_k_type)) continue;
    if ((int)p_CFDictionaryGetInt64((CFDictionaryRef)e, g_k_type, 0) == want) return 1;
  }
  return 0;
}

/* one initialVideoStreams entry: {originXPixels,originYPixels,uuid,zIndex}
 * (CarKit CARStreamInfo::initWithDictionary).  uuidRef is borrowed. */
static CFMutableDictionaryRef mk_stream(CFStringRef uuidRef){
  if (!p_CFDictionaryCreateMutable || !p_kCFLDictKeyCB || !p_kCFLDictValCB) return 0;
  CFMutableDictionaryRef d = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
  if (!d) return 0;
  if (p_CFDictionarySetInt64){
    p_CFDictionarySetInt64(d, g_k_ox, 0);
    p_CFDictionarySetInt64(d, g_k_oy, 0);
    p_CFDictionarySetInt64(d, g_k_z, 0);
  }
  if (uuidRef && p_CFDictionarySetValue) p_CFDictionarySetValue(d, g_k_uuid, uuidRef);
  return d;
}
static CFMutableArrayRef mk_stream_array(CFStringRef uuidRef){
  fn_CFArrayCreateMutable mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
  if (!mka || !p_kCFTypeArrayCallBacks) return 0;
  CFMutableArrayRef a = mka(0,1,p_kCFTypeArrayCallBacks);
  if (!a) return 0;
  CFMutableDictionaryRef s = mk_stream(uuidRef);
  if (s){ p_CFArrayAppendValue(a, s); if (p_CFRelease) p_CFRelease(s); } /* array retains */
  return a;
}
static CFMutableArrayRef mk_empty_array(void){
  fn_CFArrayCreateMutable mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
  return (mka && p_kCFTypeArrayCallBacks) ? mka(0,0,p_kCFTypeArrayCallBacks) : 0;
}

/* Complete ONE display dict to the modern schema.  is_cluster adds
 * name+properties (the "Instrument Cluster" marks).  Idempotent: a key already
 * present is left alone (a later /info may re-request the same cached dict). */
static int info_complete_entry(CFMutableDictionaryRef d, int is_cluster, s64 night){
  if (!d || !p_CFDictionarySetValue || !p_CFDictionaryGetValue) return 0;
  CFTypeRef uuidv = p_CFDictionaryGetValue(d, g_k_uuid);
  if (!is_dict(d)) return 0;

  /* uid = uuid  (CarKit and displayPanels both read "uid"; a missing uid makes
   * CRDisplayInfo init return nil -> the whole displays array is rejected). */
  if (uuidv && !p_CFDictionaryGetValue(d, g_k_uid)) p_CFDictionarySetValue(d, g_k_uid, uuidv);

  if (is_cluster){
    if (!p_CFDictionaryGetValue(d, g_k_name)) p_CFDictionarySetValue(d, g_k_name, g_v_ic);
    if (!p_CFDictionaryGetValue(d, g_k_props)){
      CFMutableArrayRef pr = mk_empty_array();
      if (pr && g_v_ic) p_CFArrayAppendValue(pr, g_v_ic);
      if (pr){ p_CFDictionarySetValue(d, g_k_props, pr); if (p_CFRelease) p_CFRelease(pr); }
    }
  }

  /* per-display scalars a real HU sends; incomplete dicts are accepted at ADD
   * but rejected at SESSION start (0 s teardown). */
  if (p_CFDictionarySetInt64){
    if (!p_CFDictionaryGetValue(d, g_k_corner)){
      /* LIVI sends cornerMasks as a BOOLEAN (false), not an int. */
      void *bf = p_kCFLBooleanFalse ? *(void**)p_kCFLBooleanFalse : 0;
      if (bf && p_CFDictionarySetValue) p_CFDictionarySetValue(d, g_k_corner, bf);
      else p_CFDictionarySetInt64(d, g_k_corner, 0);
    }
    if (!p_CFDictionaryGetValue(d, g_k_lui))    p_CFDictionarySetInt64(d, g_k_lui, 0);
    p_CFDictionarySetInt64(d, g_k_night, night);
  }
  if (!p_CFDictionaryGetValue(d, g_k_luie)){
    CFMutableArrayRef luie = mk_empty_array();
    if (luie){ p_CFDictionarySetValue(d, g_k_luie, luie); if (p_CFRelease) p_CFRelease(luie); }
  }

  /* initialVideoStreams -- REQUIRED, present and NON-empty. */
  if (!p_CFDictionaryGetValue(d, g_k_ivs) && uuidv){
    CFMutableArrayRef ivs = mk_stream_array((CFStringRef)uuidv);
    if (ivs){ p_CFDictionarySetValue(d, g_k_ivs, ivs); if (p_CFRelease) p_CFRelease(ivs); }
  }

  /* adjacentViewAreas -- present (may be empty) on the cluster. */
  if (is_cluster && !p_CFDictionaryGetValue(d, g_k_adj)){
    CFMutableArrayRef adj = mk_empty_array();
    if (adj){ p_CFDictionarySetValue(d, g_k_adj, adj); if (p_CFRelease) p_CFRelease(adj); }
  }
  return 1;
}

/* Returns 1 iff a cluster entry was found and completed.  Exported for the
 * harness as mib3shim_complete_info(). */
/* displayPanels in the LIVI shape (info.rs:319 add_display_panels): a panel
 * per display with uid="{uuid}-panel", the display's size, properties
 * (showsInstruments for the cluster) and initialVideoStreams=[{uuid}].  The
 * previous code set displayPanels = the same `displays` array. */
static void panels_build(CFMutableDictionaryRef info){
  CFTypeRef dv;
  fn_CFArrayCreateMutable mka;
  CFMutableArrayRef panels;
  CFStringRef k_sh;
  CFIndex n, i;
  info_keys_init();
  if (!g_info_keys_ready || !p_CFDictionaryGetValue || !p_CFDictionaryCreateMutable) return;
  dv = p_CFDictionaryGetValue(info, g_k_displays);
  if (!is_array(dv)) return;
  mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
  if (!mka || !p_kCFTypeArrayCallBacks) return;
  panels = mka(0,0,p_kCFTypeArrayCallBacks);
  if (!panels) return;
  k_sh = sstr("showsInstruments");
  n = p_CFArrayGetCount((CFArrayRef)dv);
  for (i = 0; i < n; i++){
    CFMutableDictionaryRef d = (CFMutableDictionaryRef)p_CFArrayGetValueAtIndex((CFArrayRef)dv, i);
    CFStringRef uuid; CFMutableDictionaryRef panel;
    char ub[64]; char pb[80]; unsigned long k = 0, j = 0;
    if (!is_dict(d)) continue;
    uuid = (CFStringRef)p_CFDictionaryGetValue(d, g_k_uuid);
    if (!uuid || !p_CFStringGetCString) continue;
    ub[0] = 0;
    if (!p_CFStringGetCString(uuid, ub, sizeof(ub), K128)) continue;
    while (ub[k] && k < 60){ pb[k] = ub[k]; k++; }
    while (j < 8 && k < 79){ pb[k++] = "-panel"[j++]; }  /* uid = uuid + "-panel" */
    pb[k] = 0;
    panel = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
    if (!panel) continue;
    { CFStringRef puid = p_CFStringCreateWithCString(0, pb, K128);
      if (puid){ dseto(panel, "uid", puid); if (p_CFRelease) p_CFRelease(puid); } }
    dseti(panel, "widthPixels",   dgeti64k(d, "widthPixels", 0));
    dseti(panel, "heightPixels",  dgeti64k(d, "heightPixels", 0));
    dseti(panel, "widthPhysical", dgeti64k(d, "widthPhysical", 0));
    dseti(panel, "heightPhysical",dgeti64k(d, "heightPhysical", 0));
    { int iscl = ((int)dict_i64(d, g_k_type) == MIB3_CLUSTER_TYPE);
      CFMutableArrayRef pr = mk_empty_array();
      if (pr){ if (iscl && k_sh) p_CFArrayAppendValue(pr, k_sh); dseto(panel, "properties", pr); if (p_CFRelease) p_CFRelease(pr); } }
    { CFMutableArrayRef ivs = mk_stream_array(uuid);
      if (ivs){ dseto(panel, "initialVideoStreams", ivs); if (p_CFRelease) p_CFRelease(ivs); } }
    p_CFArrayAppendValue(panels, panel);
    if (p_CFRelease) p_CFRelease(panel);
  }
  logkv("info","displayPanels count",(u64)p_CFArrayGetCount(panels),0);
  dseto(info, "displayPanels", panels);
  if (p_CFRelease) p_CFRelease(panels);   /* info retains it */
}

static int info_complete(CFMutableDictionaryRef info){
  if (!info || !p_CFDictionaryGetValue || !p_CFArrayGetCount ||
      !p_CFArrayGetValueAtIndex) return 0;
  info_keys_init();
  if (!g_info_keys_ready){ logline("info","keys unavailable -- skipped"); return 0; }

  CFTypeRef dv = p_CFDictionaryGetValue(info, g_k_displays);
  if (!is_array(dv)){ logline("info","no displays array in /info"); return 0; }
  CFArrayRef arr = (CFArrayRef)dv;
  CFIndex n = p_CFArrayGetCount(arr), i;

  CFMutableDictionaryRef cluster = 0, main = 0;
  for (i = 0; i < n; i++){
    CFTypeRef e = p_CFArrayGetValueAtIndex(arr, i);
    if (!is_dict(e)) continue;
    s64 t = dict_i64((CFDictionaryRef)e, g_k_type);
    if ((int)t == MIB3_CLUSTER_TYPE && !cluster) cluster = (CFMutableDictionaryRef)e;
    else if (!main) main = (CFMutableDictionaryRef)e;   /* display-0 / main */
  }
  if (!cluster){ logline("info","no cluster entry in displays -- publish nothing"); return 0; }

  s64 night = main ? dict_i64(main, g_k_night) : 0;
  if (getenv("MIB3_SHIM_NIGHT")) night = (s64)env_on("MIB3_SHIM_NIGHT",0);
  g_night_mode = night;   /* the value we later send as setNightMode{uuid,nightMode} */
  if (main && env_on("MIB3_SHIM_MAINMOD",1)) info_complete_entry(main, 0, night);
  info_complete_entry(cluster, 1, night);

  /* top-level keys: publish the SAME array under displayPanels, and the /info
   * feature advertisement.  Only when the cluster exists (above).
   *
   * displayPanels: set unconditionally (= the displays array) -- nothing in the
   * unit provides this key at all, and the reference overwrites it too.
   * extendedFeatures: MERGE, not skip or clobber.  The unit's libAirPlay reads
   * this key from the platform (it has native support), so a value may already
   * be present; overwriting could drop a real feature and skipping-if-present
   * could leave altScreen out.  Append our two strings iff missing. */
  /* displayPanels: LIVI-shaped panels (uid="{uuid}-panel" + showsInstruments +
   * initialVideoStreams).  MIB3_SHIM_PANELS=0 restores the old behaviour
   * (displayPanels = the same displays array). */
  if (env_on("MIB3_SHIM_PANELS",1)) panels_build(info);
  else p_CFDictionarySetValue(info, g_k_dp, arr);
  (void)arr;
  {
    CFMutableArrayRef ef = mk_empty_array();
    if (ef){
      /* carry over any existing strings except the two we control */
      CFTypeRef cur = p_CFDictionaryGetValue(info, g_k_ef);
      if (is_array(cur)){
        CFIndex m = p_CFArrayGetCount((CFArrayRef)cur), j;
        for (j = 0; j < m; j++){
          CFTypeRef s = p_CFArrayGetValueAtIndex((CFArrayRef)cur, j);
          if (s && p_CFStringGetCString){
            char tmp[64]; tmp[0]=0;
            if (p_CFStringGetCString((CFStringRef)s, tmp, sizeof(tmp), K128)){
              const char *a="altScreen", *b="viewAreas"; int ka=0, kb=0;
              while (a[ka] && tmp[ka] && a[ka]==tmp[ka]) ka++;
              while (b[kb] && tmp[kb] && b[kb]==tmp[kb]) kb++;
              if ((a[ka]==0 && tmp[ka]==0) || (b[kb]==0 && tmp[kb]==0)) continue;
              p_CFArrayAppendValue(ef, s);
            }
          }
        }
      }
      if (env_on("MIB3_SHIM_EXTFEAT",1)){    /* refs (LIVI/PlayPort) OMIT these here; rgi adds them (not the +0x3E gate) */
        if (g_v_alt) p_CFArrayAppendValue(ef, g_v_alt);
        if (g_v_va)  p_CFArrayAppendValue(ef, g_v_va);
      }
      p_CFDictionarySetValue(info, g_k_ef, ef);
      if (p_CFRelease) p_CFRelease(ef);
    }
  }
  logline("info","completed: uid/properties/initialVideoStreams + displayPanels + extendedFeatures");
  logkv("info","displays count",(u64)n,0);
  return 1;
}

/* ==========================================================================
 * Cluster (type 111) STREAM handling.
 *
 * The unit's stream SETUP dispatcher (inside AirPlayReceiverSessionSetup)
 * handles types 100/101/102/110 and DROPS everything else -- including 111 --
 * at its `sub w1,w0,#0x64; cmp w1,#0xa; b.hi` guard (only indices 0..10 are in
 * the jump table; 103..109 and >=111 share the drop branch).
 *
 * Worse, the session holds exactly ONE screen: the 110 case ("_ScreenSetup")
 * uses session+0x1af0 (screen), +0x1ae8 (screenInitialized), +0x1af8 (listen
 * sock), +0x1b00 (net sock) and a single _ScreenThread -- and it asserts if a
 * second 110 arrives.  So we cannot simply spoof 111->110.
 *
 * The shim therefore owns a SECOND screen object and reproduces the unit's own
 * _ScreenSetup + _ScreenThread sequence for it, using the exported
 * AirPlayReceiverSessionScreen_* API.  Two helpers the unit uses are NOT
 * exported (chacha key derive; the two screen time-sync callbacks); those are
 * called by RVA off the libAirPlay base (train-specific, flagged in the log).
 * ========================================================================*/
/* forward decls: route-B surface module (defined later in this file) */
static int surface_create(int w, int h, unsigned ivi_id);
static const char *surf_name(void);
static int surface_write_frame(const void *px, int w, int h);
/* DECODE BRIDGE (the shim's own Cinemo pipeline) -- forward decls */
static int bridge_create(int w, int h, unsigned ivi_id);
static int bridge_feed(const void *data, unsigned long len, unsigned long ts);
static void bridge_destroy(void);
static int bridge_active(void);
/* SINK DISPATCHER (2026-10-16): the one place that decides WHERE decoded frames
 * go, and the only place that may touch a display.  Modes (MIB3_SHIM_SINK):
 *   off (default) | dry | helper (out-of-process) | local (in-carplay, ACK-gated)
 * See the big comment at the definition (after the bridge module). */
#define SINK_OFF 0
#define SINK_DRY 1
#define SINK_HELPER 2
#define SINK_LOCAL 3
static int g_sink = -1;          /* -1 = unresolved */
static int g_sink_local = 0;     /* 0=bridge, 1=surface, 2=both */
static int g_sink_lf = 0;        /* local bridge->surface fallback allowed */
static void sink_resolve(void);
static int  sink_start(int w, int h, unsigned ivi_id);   /* deferred to cluster_thread */
static int  sink_feed(const void *data, unsigned long len, unsigned long ts);
int  mib3shim_helper_connected(void);
/* FRAME INTERCEPTION state (route B).  The cluster stream's ScreenStreamRef
 * (per-stream identity) is held in a small REGISTRY (not a single slot): the
 * interposer diverts any stream present in it, and every cluster thread removes
 * its own entry when it exits.  A single global would be overwritten on a
 * reconnect / a second type-111 stream, which would (a) forward the old stream's
 * frames to the singleton (main-screen corruption) and (b) let one thread mutate
 * another thread's state. */
static int cluster_on_frame(const void *data, unsigned long len, unsigned long ts);
static unsigned long g_cluster_frames;

#define SCREEN_SOCK_QOS    0xc
#define TIMESYNC_CB1_RVA   0x13b84   /* FUN_00113b84  (unexported) */
#define TIMESYNC_CB2_RVA   0x13b48   /* FUN_00113b48  (unexported) */
#define CHACHA_DERIVE_RVA  0x163d8   /* FUN_001163d8  (unexported) */
#define MIB3_MAX_CL_STREAMS 8

static void *g_cl_streams[MIB3_MAX_CL_STREAMS];
static int cl_stream_add(void *s){
  int i; if (!s) return -1;
  for (i=0;i<MIB3_MAX_CL_STREAMS;i++) if (g_cl_streams[i]==s) return 0;
  for (i=0;i<MIB3_MAX_CL_STREAMS;i++) if (!g_cl_streams[i]){ g_cl_streams[i]=s; return 0; }
  return -1;
}
static void cl_stream_remove(void *s){
  int i; if (!s) return;
  for (i=0;i<MIB3_MAX_CL_STREAMS;i++) if (g_cl_streams[i]==s){ g_cl_streams[i]=0; return; }
}
static int cl_stream_has(void *s){
  int i; if (!s) return 0;
  for (i=0;i<MIB3_MAX_CL_STREAMS;i++) if (g_cl_streams[i]==s) return 1;
  return 0;
}

/* ==========================================================================
 * COMMANDS TO THE PHONE (the refs' post-connect sequence).
 *
 * The verified iOS sequence (rgi ALTSCREEN_CLUSTER_VIDEO.md, iOS 26.6
 * AirPlaySender/CarKit decompile): once the phone connects the 111 stream it
 * sends opcode-1 (avcC) + opcode-5 (telemetry) but ZERO video until the CAR
 * drives the scene: forceKeyFrame{params:{uuid}} at stream start, then
 * showUI{params:{uuid,url}} (+ setNightMode{params:{uuid,nightMode}}) -- the
 * phone then renders the cluster scene (Apple Maps) and the video flows.
 * Command shapes verified against BOTH working receivers:
 *   rgi  altscreen_hook.c:2644-2665  showUI   {uuid, url}
 *   rgi  altscreen_hook.c:2565-2588  forceKeyFrame {uuid}
 *   rgi  altscreen_hook.c:2470-2485  setNightMode {uuid, nightMode}
 *   LIVI stack.rs:1826-1843         same three, same keys
 * Shipped via the stock AirPlayReceiverSessionSendCommand (the rgi's exact
 * route).  The uuid MUST be the CLUSTER DISPLAY uuid we advertised.
 * ======================================================================== */
/* build {type:.., params:..}; params is consumed (retained by the cmd dict,
 * our +1 released here after SendCommand returns, per the stock pattern). */
/* ---- COMMAND COMPLETION (2026-10-19, view-area lane) ----------------------
 * AirPlayReceiverSessionSendCommand(session, cmd, completion, ctx): the stock
 * response handler for POST /command calls
 *     completion(int status, CFDictionaryRef resp|NULL, void *ctx)
 * with status 0 = the phone ACCEPTED/applied and non-zero = refused.  Until now
 * we always passed (0,0) and could not tell "delivered" from "refused" -- the
 * same blindness that hid the missing "type" key (INTEGRATION_2026-10-18).
 *
 * WHERE IT RUNS: libairplay's HTTP-client thread, some time AFTER SendCommand
 * returns, and it may outlive the session that issued it.  It therefore does
 * NOTHING but compare the generation it was issued under with g_cmd_gen and
 * write one log line.  It touches no CF object (the resp dictionary is
 * deliberately ignored), takes no lock, and performs no display or socket I/O,
 * so it cannot deadlock the control queue or race a teardown.
 *
 * LIFETIME: `ctx` is one of a small ring of STATIC slots, valid for the life of
 * the process -- no malloc, nothing to free, no cross-thread ownership.  ship_cmd
 * runs on one thread at a time (the cluster receiver thread / the harness), so
 * the round-robin is monotonic; a slot reused before its reply arrives simply
 * logs under the newer command's name, which is bounded and harmless.
 *
 * MIB3_SHIM_CMDACK=0 restores the exact old call, SendCommand(session, cmd, 0, 0).
 * -------------------------------------------------------------------------*/
#define CMD_ACK_SLOTS 8
typedef struct {
  volatile int  inuse;
  volatile unsigned gen;
  volatile int  idx;
  char          type[24];
} cmd_ack_slot;
static cmd_ack_slot g_ack_slots[CMD_ACK_SLOTS];
static volatile int g_ack_rr = 0;
/* counters, so one grep shows the health of the whole command channel */
static volatile long g_ack_ok = 0, g_ack_refused = 0, g_ack_stale = 0;

static void cmd_ack_name(int idx)
{
  if (idx == 0)        logline("cluster/cmd/ack","viewarea index 0 (full panel) APPLIED");
  else if (idx < 0)    logline("cluster/cmd/ack","(not a view-area command)");
  else                 logkv("cluster/cmd/ack","viewarea index APPLIED",(u64)(u32)idx,0);
}

/* The completion callback.  Return type is void; the stock pattern in the
 * decompiled corpus (libAirPlay ProcessFrames' response path) is a plain
 * function pointer with this exact 3-argument shape. */
static void cmd_completion(int status, void *resp, void *ctx)
{
  cmd_ack_slot *s = (cmd_ack_slot *)ctx;
  (void)resp;                       /* deliberately untouched: CF object, other thread */
  if (!s) return;
  if (s->gen != g_cmd_gen){
    /* A superseded type-111 session's late reply.  Dropped, never applied --
     * acting on it would command a session that no longer exists. */
    g_ack_stale++;
    logkv("cluster/cmd/ack/stale","status",(u64)(u32)status,0);
    logline("cluster/cmd/ack/stale", s->type);
    return;
  }
  if (status == 0) g_ack_ok++; else g_ack_refused++;
  logkv("cluster/cmd/ack","status (0=accepted)",(u64)(u32)status,0);
  logline("cluster/cmd/ack/type", s->type);
  if (s->idx >= 0){
    if (status == 0){
      /* the phone APPLIED the view-area switch -- only now do we believe it */
      g_va_current = s->idx;
      logkv("cluster/va/applied","index",(u64)(u32)s->idx,0);
      cmd_ack_name(s->idx);
    } else {
      /* refused: g_va_current is deliberately NOT moved.  The display is
       * whatever it was before; this is a logged no-op. */
      logkv("cluster/va/refused","index",(u64)(u32)s->idx,0);
    }
  }
  if (status != 0) logline("cluster/cmd/ack/refused", s->type);
}

/* hand out the next slot for a command of `type` (idx<0 = no view-area index) */
static cmd_ack_slot *cmd_ack_take(const char *type, int idx)
{
  cmd_ack_slot *s;
  int n = g_ack_rr;
  g_ack_rr = (n + 1) % CMD_ACK_SLOTS;
  s = &g_ack_slots[n];
  { int i = 0; while (type && type[i] && i < (int)sizeof(s->type) - 1){ s->type[i] = type[i]; i++; }
    s->type[i] = 0; }
  s->idx   = idx;
  s->gen   = g_cmd_gen;
  s->inuse = 1;
  return s;
}

static int ship_cmd_i(void *session, const char *type, CFMutableDictionaryRef params, int va_idx)
{
  if (!session || !p_SendCommand || !p_CFDictionaryCreateMutable ||
      !p_CFDictionarySetValue || !p_kCFLDictKeyCB || !p_kCFLDictValCB || !p_CFRelease)
    return -1;
  /* ---- GENERATION GUARD (stability, 2026-10-18) ---------------------------
   * AirPlayReceiverSessionSendCommand is a dispatch_sync_f onto the SESSION's own
   * control queue, with NO timeout, that builds an RTSP POST /command and
   * HTTPClientSendMessage()s it (corpus libAirPlay decompiled.c:12863-12899 ->
   * FUN_00113038 at 9935; the shipped binary at VA 0x17f3c calls
   * dispatch_sync_f@plt).  This runs on the CLUSTER RECEIVER THREAD.
   *
   * Two hazards follow, and the refs/rgi implementation removed this exact
   * pattern (its SESSION_LIFECYCLE.md defects 3 and 5: "stock command under
   * alt_lock ... deadlock and use-after-free edge" and "night-mode refresh on
   * the receive thread ... could outlive a teardown"):
   *   (a) while the call is in flight we are not read()ing the phone's cluster
   *       socket, so that stream looks silent-but-ESTABLISHED; and
   *   (b) `session` was captured at SETUP time.  A superseded receiver thread
   *       (a new type-111 SETUP creates a new one; the old thread lives until
   *       its socket ends) would command through an OLD session object.
   *
   * Cheap guards, no behaviour change on the healthy path:
   *   1. only the CURRENT setup generation may command (g_cmd_session is written
   *      by cluster_setup on every type-111 SETUP);
   *   2. mirror stock's own liveness byte (session+0x446, set once by
   *      AirPlayReceiverSessionStart and never cleared: decompiled.c:13087,
   *      tested at 12877) so we do not enter dispatch_sync on a dead session.
   * MIB3_SHIM_CMDGEN=0 restores the old unconditional behaviour. */
  if (env_on("MIB3_SHIM_CMDGEN",1)){
    if (session != g_cmd_session){
      logline("cluster/cmd/skipped-superseded", type);
      return -2;
    }
    /* The 0x446 liveness byte is a REVERSE-ENGINEERED offset from the decompiled
     * corpus (AirPlayReceiverSessionStart / decompiled.c:13087).  The corpus is a
     * different BUILD from this unit in general, so if this offset is wrong on
     * this image the check silently disables EVERY command (showUI /
     * forceKeyFrame / nightMode / changeMapZoomLevel) while everything still
     * looks connected -- a silent, trip-costing failure.  It is therefore its own
     * knob so the operator can neutralise it WITHOUT a rebuild:
     *   MIB3_SHIM_CMDALIVE=0  -> skip the byte check (the generation check above
     *                            still applies; that one is sound by construction).
     * Every trip should grep the log for these two skip lines. */
    if (env_on("MIB3_SHIM_CMDALIVE",1) &&
        *(volatile signed char*)((char*)session + 0x446) == 0){
      logline("cluster/cmd/skipped-session-not-started", type);
      return -3;
    }
  }
  CFMutableDictionaryRef cmd = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
  if (!cmd) return -1;
  int rc = -1;
  {
    /* KEY BUG FOUND 2026-10-18 by the new command-capture bench (harness/
     * libstubcmd.c): this used to pass `sstr(type)` as the dictionary KEY, so
     * every command went out as {"<name>": "<name>", "params": {...}} -- with
     * NO "type" key at all.  Both reference receivers build
     * {"type": "<name>", "params": {...}} (playport AirPlaySession.kt:139,
     * rgi altscreen_hook.c alt_ship) and iOS dispatches on that key.
     * `showUI`/`forceKeyFrame`/`setNightMode`/`changeMapZoomLevel` were all
     * therefore shipped in a shape the phone does not recognise. */
    CFStringRef kt = sstr("type"), kp = sstr("params"), vt = sstr(type);
    if (kt && kp && vt && params){
      p_CFDictionarySetValue(cmd, kt, vt);
      p_CFDictionarySetValue(cmd, kp, params);
      /* COMPLETION (2026-10-19): MIB3_SHIM_CMDACK=1 (default) passes a real
       * callback so the phone's verdict is visible in the log; =0 is the exact
       * pre-2026-10-19 call, SendCommand(session, cmd, 0, 0). */
      if (env_on("MIB3_SHIM_CMDACK",1)){
        cmd_ack_slot *slot = cmd_ack_take(type, va_idx);
        rc = p_SendCommand(session, cmd, (void*)cmd_completion, slot);
      } else {
        rc = p_SendCommand(session, cmd, 0, 0);  /* serialized onto the session's own control queue */
      }
    }
    if (p_CFRelease){ if (vt) p_CFRelease(vt); if (kp) p_CFRelease(kp); if (kt) p_CFRelease(kt); }
  }
  p_CFRelease(cmd);                            /* caller releases after SendCommand returns (stock pattern) */
  return rc;
}
static int ship_cmd(void *session, const char *type, CFMutableDictionaryRef params)
{
  return ship_cmd_i(session, type, params, -1);
}
/* params dict with the cluster display uuid preset */
static CFMutableDictionaryRef cmd_params_uuid(void)
{
  if (!p_CFDictionaryCreateMutable || !p_kCFLDictKeyCB || !p_kCFLDictValCB) return 0;
  CFMutableDictionaryRef p = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
  if (!p) return 0;
  if (!g_cluster_uuid_str[0]) return p;        /* no uuid yet -- caller checks */
  { CFStringRef k = sstr("uuid"), v = sstr(g_cluster_uuid_str);
    if (k && v) p_CFDictionarySetValue(p, k, v);
    if (p_CFRelease){ if (v) p_CFRelease(v); if (k) p_CFRelease(k); } }
  return p;
}
static void cluster_send_showui(void *session, const char *why)
{
  if (!env_on("MIB3_SHIM_CMDS",1) || !session) return;
  if (!g_cluster_uuid_str[0]){ logline("cluster/cmd","showUI skipped (no cluster uuid)"); return; }
  CFMutableDictionaryRef p = cmd_params_uuid();
  if (!p) return;
  { const char *url = g_cluster_url[0] ? g_cluster_url : CLUSTER_URL_DEFAULT;
    CFStringRef k = sstr("url"), v = sstr(url);
    if (k && v) p_CFDictionarySetValue(p, k, v);
    if (p_CFRelease){ if (v) p_CFRelease(v); if (k) p_CFRelease(k); } }
  int rc = ship_cmd(session, "showUI", p);
  logkv("cluster/cmd","showUI rc",(u64)(u32)rc,0);
  logline("cluster/cmd/showui-why", why);
  if (p_CFRelease) p_CFRelease(p);
}
static void cluster_send_forcekeyframe(void *session, const char *why)
{
  if (!env_on("MIB3_SHIM_CMDS",1) || !session) return;
  if (!g_cluster_uuid_str[0]){ logline("cluster/cmd","forceKeyFrame skipped (no cluster uuid)"); return; }
  CFMutableDictionaryRef p = cmd_params_uuid();
  if (!p) return;
  int rc = ship_cmd(session, "forceKeyFrame", p);
  logkv("cluster/cmd","forceKeyFrame rc",(u64)(u32)rc,0);
  logline("cluster/cmd/idr-why", why);
  if (p_CFRelease) p_CFRelease(p);
}
static void cluster_send_nightmode(void *session)
{
  if (!env_on("MIB3_SHIM_CMDS",1) || !session) return;
  if (!g_cluster_uuid_str[0]) return;
  CFMutableDictionaryRef p = cmd_params_uuid();
  if (!p) return;
  dseti(p, "nightMode", (s64)(g_night_mode ? 1 : 0));
  int rc = ship_cmd(session, "setNightMode", p);
  logkv("cluster/cmd","setNightMode rc",(u64)(u32)rc,0);
  if (p_CFRelease) p_CFRelease(p);
}

/* ==========================================================================
 * UPDATE VIEW AREA (2026-10-19, view-area lane) -- THE KEY ITEM.
 *
 * The cluster's VIEW button changes the layout the ECU paints, but nothing tells
 * the phone, so Apple Maps keeps rendering into the whole panel and the gauges
 * cover it.  This ships the CarPlay command that re-frames the cluster scene:
 *
 *   {type:"updateViewArea",
 *    params:{uuid, viewAreaIndex, animationDurationMillis, adjacentViewAreas}}
 *
 * `adjacentViewAreas` MUST ALWAYS BE PRESENT (rgi, verified against the stock
 * MHI3 AirPlayReceiverSessionViewAreaUpdate): [] when the FULL-screen area is
 * current, [0] when a smaller area is current.  iOS keeps
 * CARScreenInfo.adjacentViewArea SEPARATE from currentViewArea, so omitting the
 * key is observably different from sending an empty array.
 *
 * The phone bounds-checks viewAreaIndex against the viewAreas we declared in
 * /info (carEndpoint_validateViewAreaParams: "viewAreaIndex < viewAreas.count"),
 * which is why declaring the areas (va_build) must land first.
 *
 * GATES: MIB3_SHIM_VIEWAREAS=1 (we declared them) AND MIB3_SHIM_CMDS=1 (commands
 * are on) AND MIB3_SHIM_VA_CMD=1 (this one specifically).  A refused or ignored
 * switch is a LOGGED NO-OP: g_va_current is only updated when the phone's
 * completion says status 0 -- so a failure can never strand the display in a
 * state we believe we are in but the phone is not.
 * ========================================================================*/
static int cluster_send_viewarea(void *session, int index, int dur_ms, const char *why)
{
  CFMutableDictionaryRef p;
  fn_CFArrayCreateMutable mka;
  CFMutableArrayRef adj;
  int rc;
  if (!session || !env_on("MIB3_SHIM_CMDS",1)) return -1;
  if (!env_on("MIB3_SHIM_VIEWAREAS",0)){
    logline("cluster/va/cmd","skipped (MIB3_SHIM_VIEWAREAS=0)");
    return -1;
  }
  if (!env_on("MIB3_SHIM_VA_CMD",1)){
    logline("cluster/va/cmd","skipped (MIB3_SHIM_VA_CMD=0)");
    return -1;
  }
  if (index < 0 || index >= va_count()){
    logkv("cluster/va/cmd","index out of range (declared count)",(u64)(u32)va_count(),0);
    logline("cluster/va/cmd","refused locally -- no command sent");
    return -1;
  }
  if (!g_cluster_uuid_str[0]){ logline("cluster/va/cmd","skipped (no cluster uuid)"); return -1; }
  mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
  if (!mka || !p_kCFTypeArrayCallBacks || !p_CFArrayAppendInt64){
    logline("cluster/va/cmd","skipped (CF helpers missing)");
    return -1;
  }
  p = cmd_params_uuid();
  if (!p) return -1;
  dseti(p, "viewAreaIndex", (s64)index);
  dseti(p, "animationDurationMillis", (s64)(dur_ms > 0 ? dur_ms : 250));
  adj = mka(0, index == 0 ? 0 : 1, p_kCFTypeArrayCallBacks);
  if (!adj){ if (p_CFRelease) p_CFRelease(p); return -1; }
  /* full-screen area current -> []; a smaller area current -> [0] */
  if (index != 0) p_CFArrayAppendInt64(adj, 0);
  dseto(p, "adjacentViewAreas", adj);
  if (p_CFRelease) p_CFRelease(adj);           /* params retains it */

  rc = ship_cmd_i(session, "updateViewArea", p, index);
  logkv("cluster/va/cmd","index shipped",(u64)(u32)index,0);
  logkv("cluster/va/cmd","adjacent (0=[],1=[0])",(u64)(unsigned)(index == 0 ? 0 : 1),0);
  logkv("cluster/va/cmd","updateViewArea rc",(u64)(u32)rc,0);
  logline("cluster/va/cmd/why", why ? why : "?");
  if (p_CFRelease) p_CFRelease(p);
  /* The phone's verdict arrives asynchronously in cmd_completion(); do NOT
   * assume success here. */
  return rc;
}

/* ==========================================================================
 * APPEARANCE (2026-10-19, view-area lane).
 *
 *   {type:"mapAppearanceUpdate"|"uiAppearanceUpdate",
 *    params:{uuid, appearanceMode, appearanceSetting}}
 *
 * Per-SURFACE appearance: finer grained than setNightMode, which is one mode for
 * the whole session.  rgi ships exactly this pair (altscreen_hook.c:2678-2683)
 * with a [target][mode][setting] triple where target 0 = UI, 1 = map.
 *
 * KNOBS: MIB3_SHIM_APPEARANCE=1 arms it; MIB3_SHIM_APPMODE / MIB3_SHIM_APPSETTING
 * give the pair; MIB3_SHIM_APPTARGET=map|ui picks which command.  Nothing is
 * sent unless APPEARANCE is on, so the shipped default is unchanged.  Driven at
 * runtime by the control-inbox line `a <map|ui> <mode> <setting>`.
 *
 * "Drive from the car's theme where possible": NOT POSSIBLE YET -- the only
 * theme-ish signal we already have is g_night_mode (from setNightMode), and
 * there is no shipped RSI/viwi service that publishes the cluster's appearance
 * (see the lane doc, problem A).  dayNight IS exposed on
 * viwi.service.hmiintegration (port 49506, service `hmiintegration`), which is
 * the honest candidate for a future auto-drive; it is not wired here.
 * ========================================================================*/
static void cluster_send_appearance(void *session, int is_map, int mode, int setting)
{
  CFMutableDictionaryRef p;
  int rc;
  if (!session || !env_on("MIB3_SHIM_CMDS",1)) return;
  if (!g_cluster_uuid_str[0]){ logline("cluster/appear","skipped (no cluster uuid)"); return; }
  p = cmd_params_uuid();
  if (!p) return;
  dseti(p, "appearanceMode", (s64)mode);
  dseti(p, "appearanceSetting", (s64)setting);
  rc = ship_cmd(session, is_map ? "mapAppearanceUpdate" : "uiAppearanceUpdate", p);
  logkv("cluster/appear","target (0=ui 1=map)",(u64)(u32)(is_map ? 1 : 0),0);
  logkv("cluster/appear","appearanceMode",(u64)(u32)mode,0);
  logkv("cluster/appear","appearanceSetting",(u64)(u32)setting,0);
  logkv("cluster/appear","rc",(u64)(u32)rc,0);
  if (p_CFRelease) p_CFRelease(p);
}
/* One-shot, at first IDR, when armed. */
static void cluster_send_appearance_armed(void *session)
{
  if (!env_on("MIB3_SHIM_APPEARANCE",0)) return;
  { const char *t = getenv("MIB3_SHIM_APPTARGET");
    int is_map = (t && (t[0]=='m'||t[0]=='M')) ? 1 : 0;
    int mode = (int)env_num("MIB3_SHIM_APPMODE", 0);
    int set  = (int)env_num("MIB3_SHIM_APPSETTING", 0);
    cluster_send_appearance(session, is_map, mode, set); }
}

/* ==========================================================================
 * ZOOM: the STEERING-WHEEL up/down mirror (2026-10-18).
 *
 * WHY THIS EXISTS (see LANE_CLUSTER_ZOOM_2026-10-18.md):
 * The wheel roller does NOT reach CarPlay.  On this unit it goes
 *   cluster (CAN MFL) -> BAP NavigationSd MapScale SetGet -> navsd_app
 *   -> POST {relativeSteps} to /maprenderer/discreteInputs/<renderer>
 *   -> the CAR'S OWN nav zooms -> navsd reports BAP MapScale Status -> the
 *   cluster paints its own scale OSD.
 * Our video replaces the nav's map, so the wheel moved a map nobody sees.
 *
 * The fix is the CarPlay cluster API for exactly this: send
 *   changeMapZoomLevel {uuid: <cluster display uuid>, zoomDirection: 0|1}
 * (0 = zoom IN, non-zero = zoom OUT -- rgi command revert + DashBoard's
 * CRSUIClusterZoomAction).  The phone routes it to the cluster scene, Apple
 * Maps zooms.
 *
 * WHERE THE STEPS COME FROM: the external tool `mib3wheel` (shim/tools/
 * mib3wheel.c) watches the unit's own maprenderer RSI and writes "+1"/"-1"
 * lines into an AF_UNIX socket we own.  This module is the receiving end.
 *
 * WHY A MAILBOX INSTEAD OF SENDING FROM THE LISTENER THREAD: the listener
 * would have to hold the session pointer, i.e. it could send into a torn-down
 * session (a use-after-free inside carplay => reboot).  Instead the listener
 * only QUEUES the step and the CLUSTER THREAD -- which already owns a valid
 * session for the length of that stream -- drains the queue and ships.  No new
 * lifetime contract, no lock.
 * ========================================================================*/
#define ZOOM_Q 16
static volatile int g_zoom_q[ZOOM_Q];
static volatile int g_zoom_qw, g_zoom_qr, g_zoom_qdrop;
static volatile int g_zoom_started;

/* returns 1 queued, 0 dropped (full) */
static int zoom_push(int dir)
{
  int w = g_zoom_qw, n = (w + 1) % ZOOM_Q;
  if (n == g_zoom_qr){ g_zoom_qdrop++; return 0; }     /* full: drop the OLDEST */
  g_zoom_q[w] = dir; g_zoom_qw = n;
  return 1;
}
/* +1 = zoom in, -1 = zoom out, 0 = empty */
static int zoom_pop(void)
{
  int r = g_zoom_qr;
  if (r == g_zoom_qw) return 0;
  { int v = g_zoom_q[r]; g_zoom_qr = (r + 1) % ZOOM_Q; return v; }
}

/* Ship ONE changeMapZoomLevel for the cluster display.  zoomDirection is the
 * rgi/DashBoard wire form: 0 = in, anything else = out. */
static void cluster_send_zoom(void *session, int dir_in)
{
  if (!env_on("MIB3_SHIM_CMDS",1) || !session) return;
  if (!g_cluster_uuid_str[0]){ logline("cluster/zoom","skipped (no cluster uuid)"); return; }
  CFMutableDictionaryRef p = cmd_params_uuid();
  if (!p) return;
  dseti(p, "zoomDirection", (s64)(dir_in ? 0 : 1));
  { int rc = ship_cmd(session, "changeMapZoomLevel", p);
    logkv("cluster/zoom","dir (0=in 1=out)",(u64)(u32)(dir_in ? 0 : 1),0);
    logkv("cluster/zoom","changeMapZoomLevel rc",(u64)(u32)rc,0); }
  if (p_CFRelease) p_CFRelease(p);
}

/* The listener thread: accept wheel steps from mib3wheel, queue them.
 * Never sends anything itself (see the mailbox note above).  It runs for the
 * life of the process; a reboot is the undo. */
/* ---- the control inbox's line grammar -------------------------------------
 * ONE socket carries two logical channels, so a single small tool can drive
 * both without a second listener/thread:
 *   +1 | 1 | in      zoom in        (queued, shipped by the cluster thread)
 *   -1 | out         zoom out
 *   v<N> | view <N>  switch to view-area index N
 *   next | prev      step the view-area index (wrapping over MIB3_SHIM_VA_COUNT)
 * An unknown line is LOGGED and ignored -- never acted on.  Lines are parsed
 * from a small accumulator so a streamed client ("+1-1" with no separators)
 * behaves exactly as it did before this change. */
#define VA_Q 8
static volatile int g_va_q[VA_Q];
static volatile int g_va_qw, g_va_qr, g_va_qdrop;
#define VA_REQ_NEXT  (-2)
#define VA_REQ_PREV  (-3)
static int va_push(int idx)
{
  int w = g_va_qw, n = (w + 1) % VA_Q;
  if (n == g_va_qr){ g_va_qdrop++; return 0; }
  g_va_q[w] = idx; g_va_qw = n;
  return 1;
}
static int va_pop(void)
{
  int r = g_va_qr;
  if (r == g_va_qw) return -9999;
  { int v = g_va_q[r]; g_va_qr = (r + 1) % VA_Q; return v; }
}

/* ---- appearance queue (item 3) -------------------------------------------
 * packed int: (target<<24) | (mode<<12) | setting.  Same lifetime argument as
 * the zoom/va queues -- the listener never sends, the cluster thread does. */
#define AP_Q 4
static volatile int g_ap_q[AP_Q];
static volatile int g_ap_qw, g_ap_qr, g_ap_qdrop;
static int ap_push(int packed)
{
  int w = g_ap_qw, n = (w + 1) % AP_Q;
  if (n == g_ap_qr){ g_ap_qdrop++; return 0; }
  g_ap_q[w] = packed; g_ap_qw = n;
  return 1;
}
static void ctl_push_zoom(int dir)
{
  int d = dir;
  if (env_on("MIB3_SHIM_ZOOM_INVERT",0)) d = -d;
  if (zoom_push(d)) logkv("cluster/zoom","queued dir",(u64)(u32)(d > 0 ? 1 : 0),0);
  else logline("cluster/zoom","queue full -- step dropped");
}
/* case-insensitive equality of a NUL-free token */
static int tok_eq(const char *s, int n, const char *t)
{
  int i;
  for (i = 0; i < n; i++){
    char a = s[i], b = t[i];
    if (!b) return 0;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return 0;
  }
  return t[n] == 0;
}
static void ctl_handle_line(const char *s, int n)
{
  int i = 0, dir = 0, sign = 0, num = 0, hasn = 0, isva = 0;
  if (n <= 0) return;
  /* --- appearance channel: "a <map|ui> <mode> <setting>" ------------------ */
  if (s[0] == 'a' || s[0] == 'A'){
    int target = 0, vals[2], nv = 0, seen = 0;
    i = 1; vals[0] = -1; vals[1] = -1;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < n && (s[i] == 'm' || s[i] == 'M')){ target = 1; while (i < n && s[i] != ' ' && s[i] != '\t') i++; }
    else if (i < n && (s[i] == 'u' || s[i] == 'U')){ target = 0; while (i < n && s[i] != ' ' && s[i] != '\t') i++; }
    while (i < n && nv < 2){
      while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) i++;
      if (i >= n) break;
      if (s[i] < '0' || s[i] > '9'){ i++; continue; }
      { int v = 0; while (i < n && s[i] >= '0' && s[i] <= '9'){ v = v*10 + (s[i] - '0'); i++; } vals[nv++] = v; seen = 1; }
    }
    if (!seen){ logline("cluster/appear/req","no mode/setting -- ignored"); return; }
    if (vals[1] < 0) vals[1] = 0;
    { int packed = ((target & 0xff) << 24) | ((vals[0] & 0xfff) << 12) | (vals[1] & 0xfff);
      if (ap_push(packed)) logkv("cluster/appear/req","queued (target<<24|mode<<12|setting)",(u64)(u32)packed,0);
      else logline("cluster/appear/req","queue full -- dropped"); }
    return;
  }
  /* --- view-area channel -------------------------------------------------- */
  if (s[0] == 'v' || s[0] == 'V'){ isva = 1; i = 1; if (n > 1 && (s[1]=='i'||s[1]=='I')) i = 4; }
  else if (tok_eq(s, n, "next") || tok_eq(s, n, "prev")){ isva = 1; i = 0; }
  if (isva){
    if (tok_eq(s, n, "next")){ if (va_push(VA_REQ_NEXT)) logline("cluster/va/req","next"); else logline("cluster/va/req","queue full"); return; }
    if (tok_eq(s, n, "prev")){ if (va_push(VA_REQ_PREV)) logline("cluster/va/req","prev"); else logline("cluster/va/req","queue full"); return; }
    for (; i < n; i++){
      char c = s[i];
      if (c == ' ' || c == '\t') continue;
      if (c >= '0' && c <= '9'){ num = num * 10 + (c - '0'); hasn = 1; continue; }
      logkv("cluster/va/req","ignored byte",(u64)(unsigned)(c & 0xff),0);
    }
    if (hasn){ if (va_push(num)) logkv("cluster/va/req","set index",(u64)(u32)num,0); else logline("cluster/va/req","queue full"); }
    else logline("cluster/va/req","no index and no next/prev -- ignored");
    return;
  }
  /* --- zoom channel ------------------------------------------------------- */
  for (; i < n; i++){
    char c = s[i];
    if (c == ' ' || c == '\t') continue;
    if (c == 'i' || c == 'I'){ dir = 1;  continue; }
    if (c == 'o' || c == 'O'){ dir = -1; continue; }
    if (c == '+'){ if (!sign) sign = 1; continue; }
    if (c == '-'){ sign = -1; continue; }
    if (c >= '0' && c <= '9'){ hasn = 1; continue; }   /* magnitude: always 1 step */
    logkv("cluster/zoom","ignored byte",(u64)(unsigned)(c & 0xff),0);
  }
  if (dir) ctl_push_zoom(dir);
  else if (sign) ctl_push_zoom(sign);
  else if (hasn) ctl_push_zoom(1);
  /* a bare "-" with nothing after it: ignored, as before */
}

static void *zoom_listen_thread(void *arg)
{
  extern int socket(int,int,int);
  extern int bind(int,const void*,unsigned int);
  extern int listen(int,int);
  extern int accept(int,void*,unsigned int*);
  extern int unlink(const char*);
  extern long read(int,void*,unsigned long);
  extern int close(int);
  const char *path = (const char*)arg;
  struct sun { unsigned short family; char p[108]; };
  struct sun sa;
  int fd, conn, i;
  logline("cluster/zoom","listener starting");
  unlink(path);
  fd = socket(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0);
  if (fd < 0){ logline("cluster/zoom","socket() failed"); return 0; }
  { unsigned char *q = (unsigned char*)&sa;
    for (i = 0; i < (int)sizeof(sa); i++) q[i] = 0;
    sa.family = 1;
    { unsigned long n = 0; while (path[n] && n < sizeof(sa.p)-1){ sa.p[n] = path[n]; n++; } }
  }
  if (bind(fd, &sa, (unsigned int)sizeof(sa)) != 0){
    logline("cluster/zoom","bind failed"); close(fd); return 0;
  }
  if (listen(fd, 4) != 0){ logline("cluster/zoom","listen failed"); close(fd); return 0; }
  logline("cluster/zoom/listen", path);
  /* MULTI-CLIENT.  mib3wheel keeps its connection open for the whole session, so
   * a single-client accept loop leaves every one-shot (mib3ctl / viewarea.sh /
   * wheel_zoom) sitting unaccepted in the backlog: the command is written and
   * silently never runs.  Found by reading this loop, 2026-10-19. */
  {
    struct { int fd; char lb[64]; int ln; } cl[8];
    int i;
    for (i = 0; i < 8; i++){ cl[i].fd = -1; cl[i].ln = 0; }
    for (;;){
      struct { int fd; short events; short revents; } pfd[9];
      int n = 1, ci[9];
      extern int poll(void*, unsigned long, int);
      pfd[0].fd = fd; pfd[0].events = 1; pfd[0].revents = 0; ci[0] = -1;
      for (i = 0; i < 8; i++) if (cl[i].fd >= 0){
        pfd[n].fd = cl[i].fd; pfd[n].events = 1; pfd[n].revents = 0; ci[n] = i; n++;
      }
      if (poll(pfd, (unsigned long)n, -1) < 0) continue;
      if (pfd[0].revents & 1){
        conn = accept(fd, 0, 0);
        if (conn >= 0){
          int slot = -1;
          for (i = 0; i < 8; i++) if (cl[i].fd < 0){ slot = i; break; }
          if (slot >= 0){
            cl[slot].fd = conn; cl[slot].ln = 0;
            logkv("cluster/zoom","peer fd",(u64)(u32)conn,0);
          } else close(conn);                 /* table full: drop this client */
        }
      }
      for (i = 1; i < n; i++) if (pfd[i].revents & 1){
        int j = ci[i];
        char buf[64]; long r, k;
        if (cl[j].fd < 0) continue;
        r = read(cl[j].fd, buf, sizeof(buf));
        if (r <= 0){
          /* Peer gone: flush the last UNTERMINATED line once, then close.  A
           * line that never got its '\n' must still be acted on, but exactly
           * once -- not re-flushed on a later read. */
          if (cl[j].ln > 0){ ctl_handle_line(cl[j].lb, cl[j].ln); cl[j].ln = 0; }
          logline("cluster/zoom","peer gone");
          close(cl[j].fd); cl[j].fd = -1; cl[j].ln = 0;
          continue;
        }
        for (k = 0; k < r; k++){
          char c = buf[k];
          if (c == '\n' || c == '\r'){
            if (cl[j].ln > 0){ ctl_handle_line(cl[j].lb, cl[j].ln); cl[j].ln = 0; }
            continue;
          }
          if (cl[j].ln < (int)sizeof(cl[j].lb) - 1) cl[j].lb[cl[j].ln++] = c;
          else { ctl_handle_line(cl[j].lb, cl[j].ln); cl[j].ln = 0; }  /* full: flush, keep going */
        }
        /* NO end-of-read flush.  It is what made a token split across two reads
         * ("next" then "\n", which mib3ctl sends as TWO write()s) dispatch TWICE:
         * once as a partial at end-of-read, once when the terminator arrived.
         * Newline-only (plus flush-on-disconnect and flush-on-full) is the
         * correct, single-dispatch contract. */
      }
    }
  }
  return 0;   /* unreachable */
}

/* Idempotent: start the listener once, with the cluster uuid + session globals
 * primed (the exported wrapper below is what the harness drives). */
static int zoom_listen_start(void)
{
  extern int setenv(const char*,const char*,int);
  static char path[128];
  static int once = 0;
  void *th = 0;
  const char *p;
  if (g_zoom_started) return 0;
  if (!env_on("MIB3_SHIM_ZOOM",1)){ logline("cluster/zoom","disabled (MIB3_SHIM_ZOOM=0)"); return 0; }
  if (!p_pthreadCreate){ logline("cluster/zoom","no pthread -- disabled"); return 0; }
  p = getenv("MIB3_SHIM_ZOOM_SOCK");
  if (!p || !p[0]) p = "/dev/shm/mib3zoom.sock";
  { unsigned long n = 0; while (p[n] && n < sizeof(path)-1){ path[n] = p[n]; n++; } path[n] = 0; }
  (void)setenv;                                  /* (kept for future knobs) */
  if (once) return 0;
  once = 1;
  if (p_pthreadCreate(&th, 0, zoom_listen_thread, path) != 0){
    logline("cluster/zoom","pthread_create failed"); return -1;
  }
  g_zoom_started = 1;
  return 0;
}

/* ---- test/bench surface (mirrors mib3shim_cluster_setup's role) ----------- */
/* Prime the uuid + session and start the listener.  Production calls
 * zoom_listen_start() from cluster_setup (uuid already primed by the advert);
 * the harness has no advert, so it primes them here. */
int mib3shim_zoom_listen(void *session, const char *uuid, const char *sock)
{
  resolve();
  if (uuid && uuid[0]){
    unsigned long n = 0;
    while (uuid[n] && n < sizeof(g_cluster_uuid_str)-1){ g_cluster_uuid_str[n] = uuid[n]; n++; }
    g_cluster_uuid_str[n] = 0;
  }
  if (sock && sock[0]){
    extern int setenv(const char*,const char*,int);
    setenv("MIB3_SHIM_ZOOM_SOCK", sock, 1);
  }
  g_cmd_session = session;
  g_cmd_gen++;
  return zoom_listen_start();
}
/* Drain up to `max` queued steps and ship them on `session` (0 -> g_cmd_session).
 * Production calls this at the top of the cluster thread's loop; the harness
 * calls it directly.  Returns the number shipped. */
int mib3shim_zoom_pump(void *session, int max)
{
  int k, sent = 0;
  void *s = session ? session : g_cmd_session;
  if (max <= 0) max = 4;
  for (k = 0; k < max; k++){
    int d = zoom_pop();
    if (!d) break;
    cluster_send_zoom(s, d > 0);
    sent++;
  }
  if (g_zoom_qdrop){ logkv("cluster/zoom","dropped total",(u64)g_zoom_qdrop,0); g_zoom_qdrop = 0; }
  return sent;
}

/* Drain up to `max` queued view-area requests and ship updateViewArea on
 * `session` (0 -> g_cmd_session).  Production calls this from the cluster thread
 * next to the zoom pump, so that the command is issued on the thread that owns a
 * valid session (same lifetime argument as the ZOOM block).  Returns the number
 * of commands actually shipped. */
int mib3shim_viewarea_pump(void *session, int max)
{
  int k, sent = 0;
  void *s = session ? session : g_cmd_session;
  if (max <= 0) max = 1;
  for (k = 0; k < max; k++){
    int r = va_pop();
    int idx;
    if (r == -9999) break;
    if (r == VA_REQ_NEXT)      idx = (g_va_current + 1) % va_count();
    else if (r == VA_REQ_PREV) idx = (g_va_current + va_count() - 1) % va_count();
    else                       idx = r;
    if (cluster_send_viewarea(s, idx, 250, "inbox") == 0) sent++;
  }
  if (g_va_qdrop){ logkv("cluster/va/req","dropped total",(u64)g_va_qdrop,0); g_va_qdrop = 0; }
  return sent;
}
/* Bench/test entry point: ship one updateViewArea for `index` directly. */
int mib3shim_viewarea_send(void *session, int index)
{
  resolve();
  return cluster_send_viewarea(session ? session : g_cmd_session, index, 250, "harness");
}

/* Drain queued appearance updates for the cluster thread. */
int mib3shim_appear_pump(void *session, int max)
{
  int k, sent = 0;
  void *s = session ? session : g_cmd_session;
  if (max <= 0) max = 1;
  for (k = 0; k < max; k++){
    int r = g_ap_qr, packed;
    if (r == g_ap_qw) break;
    packed = g_ap_q[r]; g_ap_qr = (r + 1) % AP_Q;
    cluster_send_appearance(s, (packed >> 24) & 0xff, (packed >> 12) & 0xfff, packed & 0xfff);
    sent++;
  }
  if (g_ap_qdrop){ logkv("cluster/appear/req","dropped total",(u64)g_ap_qdrop,0); g_ap_qdrop = 0; }
  return sent;
}
/* Bench entry point: parse one control line and run its effect path. */
int mib3shim_ctl_line(const char *line)
{
  int n = 0;
  resolve();
  if (!line) return -1;
  while (line[n]) n++;
  ctl_handle_line(line, n);
  return 0;
}

/* ---- AVCC <-> Annex-B (the refs' proven helpers) ---------------------------
 * opcode-1 bodies are unencrypted avcC record configs; opcode-0 bodies are
 * AVCC access units (u32-be length-prefixed NALs at lengthSize ls).  The
 * decoder side (Cinemo/OMX) wants Annex-B start codes.  Mirrors rgi
 * avcc_config.h / avcc_frame.h (themselves live-proven on a stock HU). */
static unsigned char g_cl_cfg_annexb[4096];  /* SPS/PPS as Annex-B (from opcode-1) */
static unsigned long  g_cl_cfg_len;
static unsigned char  g_cl_ls = 4;           /* NAL lengthSize (from avcC) */

/* avcC -> Annex-B SPS/PPS.  Returns the lengthSize (1..4), 0 = unusable. */
static unsigned avcc_config_to_annexb(const unsigned char *a, unsigned len,
                                      unsigned char *out, unsigned cap, unsigned *outlen)
{
  unsigned i = 6, o = 0, group, n, count; unsigned char ls;
  *outlen = 0;
  if (!a || !out || len < 7 || a[0] != 1) return 0;
  ls = (unsigned char)((a[4] & 3) + 1);
  count = a[5] & 31;
  for (group = 0; group < 2; group++){
    if (!count) return 0;
    for (n = 0; n < count; n++){
      unsigned size;
      if (len - i < 2) return 0;
      size = ((unsigned)a[i] << 8) | a[i+1]; i += 2;
      if (!size || size > len - i || cap - o < 4 || size > cap - o - 4) return 0;
      out[o] = out[o+1] = out[o+2] = 0; out[o+3] = 1;
      { unsigned k; for (k = 0; k < size; k++) out[o+4+k] = a[i+k]; }
      o += 4 + size; i += size;
    }
    if (group == 0){ if (i == len) return 0; count = a[i++]; }
  }
  *outlen = o;
  return ls;
}
/* whole AVCC access unit -> classify; when ls==4 rewrite the length prefixes
 * into Annex-B start codes IN PLACE (no copy).  Returns 0 ok, -1 malformed.
 * *is_idr set when any NAL is type 5 (IDR). */
static int avcc_prepare(unsigned char *p, unsigned long len, unsigned char ls, int *is_idr)
{
  unsigned long i = 0, total = 0; int key = 0;
  *is_idr = 0;
  if (!p || !len || len > (8u<<20) || ls < 1 || ls > 4) return -1;
  while (len - i >= ls){
    unsigned long n = 0, k, prefix = i;
    for (k = 0; k < ls; k++) n = (n << 8) | p[i+k];
    i += ls;
    if (!n || n > len - i || n > (8u<<20) - 4 || total > (8u<<20) - 4 - n) return -1;
    if ((p[i] & 31u) == 5u) key = 1;
    if (ls == 4){ p[prefix]=0; p[prefix+1]=0; p[prefix+2]=0; p[prefix+3]=1; }
    total += 4 + n; i += n;
  }
  if (i != len) return -1;
  *is_idr = key;
  return 0;
}

/* per-setup context: the servicing thread reads ONLY this, never a shared
 * global that a later setup could overwrite (that was the UAF/corruption path). */
typedef struct {
  void *screen; int lsock; int lsockgen; void *session; int flags; void *stream;
  unsigned char key[32]; int keylen; int cipher; long counter;   /* B: stream crypto */
  long night; int aes_inited;
} cl_ctx;

/* The type-111 listener we currently own, plus a GENERATION token.  A newer
 * SETUP supersedes both; see the listener-ownership note in cluster_thread.
 * The token, not the fd number, is the ownership test: own_listen closes the old
 * fd and immediately socket()s a new one, which the kernel hands back with the
 * SAME number -- so an fd comparison would let a stale receiver thread accept on
 * the NEW listener and service the new phone stream with an OLD key/session. */
static volatile int g_lsock = -1;
static volatile int g_lsock_gen = 0;

/* Which cluster thread owns the helper (AU) socket.  One phone session gets one
 * cluster setup and therefore one thread, but the unit measured 8 connects / 7
 * disconnects in a single run, so a newer thread can exist while an older one is
 * still unwinding.  Only the newest is allowed to close the helper socket (see
 * the stream-closed path).
 * [integration note: this is a SECOND, independent generation token; the one
 * above owns the listener, this one owns the helper/AU socket.  They are not
 * interchangeable -- a thread can be newest for one and not the other.] */
static volatile unsigned cl_thread_gen = 0;

static void *cluster_thread(void *arg){
  extern long read(int, void*, unsigned long);
  cl_ctx *c = (cl_ctx*)arg;
  unsigned cl_my_gen = ++cl_thread_gen;
  int connfd = -1;
  logline("cluster/thread","started (self-contained receiver)");
  if (!c) return 0;
  extern int accept(int, void*, unsigned int*);
  logkv("cluster/thread","listening fd",(u64)(u32)c->lsock,0);
  logline("cluster/thread","waiting for the phone to connect");
  /* ---- LISTENER OWNERSHIP (stability, 2026-10-18) -------------------------
   * The old code did ONE blocking accept() with no timeout.  If the phone never
   * opened the type-111 stream for this session (it aborted the stream, or its
   * SYN was dropped), this thread stayed in accept() forever and the listener
   * kept port 5000 bound for the life of the process.  The NEXT type-111 SETUP
   * then failed to bind 5000 (SO_REUSEADDR does not permit a second live
   * listener) and own_listen() fell through to 6031 -- a port the unit's own
   * firewall drops for the phone's interface (lge/app_ro/network: only
   * "-i ncm0 --dport 5000:5001/6030/7000" are allowed).  The phone would then
   * dial a black hole that WE advertised: the documented libairplay outcome is
   * "the phone gave up on an advertised but unserviced cluster" (-6722/-6753,
   * 23 s sessions).  refs/rgi hit the same class and fixed it the same way
   * (SESSION_LIFECYCLE.md defect 4: "infinite 111 accept ... later 111 SETUP
   * fails -6728" -> one-second accept tick rechecking the stop flag).
   * So: accept with a short tick, and abandon the listener as soon as a newer
   * SETUP has superseded it (g_lsock no longer points at our fd). */
  while (c->lsock >= 0 && connfd < 0){
    struct pollfd { int fd; short events; short revents; } pfd;
    extern int poll(void*, unsigned long, int);
    pfd.fd = c->lsock; pfd.events = 1 /*POLLIN*/; pfd.revents = 0;
    { int pr = poll(&pfd, 1, 250);
      if (g_lsock_gen != c->lsockgen){ logline("cluster/thread","listener superseded -- abandoning it"); break; }
      if (pr < 0) continue;
      if (pr > 0 && (pfd.revents & 1)) connfd = accept(c->lsock, 0, 0);
      /* POLLNVAL/HUP means the listener went away under us. */
      if (pr > 0 && (pfd.revents & ~1) && connfd < 0) break;
    }
  }
  logkv("cluster/thread","conn fd",(u64)(u32)connfd,0);
  if (connfd >= 0){
    if (p_close){ p_close(c->lsock); }
    if (g_lsock_gen == c->lsockgen){ g_lsock = -1; g_lsock_gen++; }
    c->lsock = -1;
    logline("cluster/thread","stream connected");
    /* THE SINK STANDS UP HERE -- on this thread, NOT in the SETUP seam.  A
     * failing/blocking sink can no longer stall CarPlay session setup.  In the
     * default (off) and dry modes this touches no display. */
    { int cw  = (int)env_num("MIB3_SHIM_W",1280);
      int chp = (int)env_num("MIB3_SHIM_H",480);
      unsigned iviid = (unsigned)env_num("MIB3_SHIM_IVIID",0x4d4233); /* 'MB3' */
      sink_start(cw, chp, iviid); }
    /* THE PHONE IS ON THE WIRE. Per the refs, the phone now sends opcode-1
     * (avcC) + opcode-5 (telemetry) but NO video until we drive the scene:
     * forceKeyFrame at stream start (rgi alt_recv_loop: "A decoder cannot
     * start mid-GOP. Ask for a cluster-targeted IDR"), and showUI so Maps
     * renders the cluster scene (the paired/chacha rgi build sends showUI at
     * stream-connected; the AES build at first IDR -- we do both, cheap). */
    cluster_send_forcekeyframe(c->session, "stream-start");
    cluster_send_showui(c->session, "stream-connected");
    cluster_send_nightmode(c->session);
    {
      unsigned char hdr[128];
      unsigned char *rbuf = 0; unsigned long rcap = 0;
      unsigned char cxbuf[512];
      long frames = 0, configs = 0, others = 0, idrs = 0;
      int stop = 0, have_cfg = 0, first_idr_sent = 0;
      unsigned char ls = g_cl_ls;
      logkv("cluster/thread","cipher",(u64)(u32)c->cipher,0);
      while (!stop){
        long got = 0, n;
        /* Steering-wheel zoom steps queued by the listener thread: ship them
         * HERE, on the thread that owns a valid session (see the ZOOM block).
         * At most 4 per iteration so a burst cannot starve the video path. */
        { int z = mib3shim_zoom_pump(c->session, 4);
          if (z > 0) logkv("cluster/zoom","steps shipped",(u64)(u32)z,0); }
        /* View-area switches queued by the same inbox (view/next/prev). */
        mib3shim_viewarea_pump(c->session, 1);
        /* Appearance updates queued by the same inbox ("a map|ui mode setting"). */
        mib3shim_appear_pump(c->session, 1);
        /* 128-byte screen header (bodySize u32 LE @0, opcode @4) */
        while (got < 128){ n = read(connfd, hdr + got, 128 - got); if (n <= 0){ logkv("cluster/thread","hdr read ended (0=EOF,1=err)",(u64)(n==0?0:1),0); logkv("cluster/thread","  bytes got",(u64)got,0); stop = 1; break; } got += n; }
        if (stop) break;
        {
          unsigned bs = (unsigned)(unsigned char)hdr[0] | ((unsigned)(unsigned char)hdr[1] << 8) |
                        ((unsigned)(unsigned char)hdr[2] << 16) | ((unsigned)(unsigned char)hdr[3] << 24);
          unsigned op = (unsigned char)hdr[4];
          if (bs > (8u << 20)){ logkv("cluster/thread","bad bodySize",(u64)bs,0); break; }
          /* grow the body buffer to the frame size (the OLD code read into a
           * static 64 KiB array -> any IDR > 64 KiB overran static memory) */
          if (bs > rcap){
            if (p_malloc){
              unsigned long newsz = rcap ? rcap : 65536;
              while (newsz < bs) newsz <<= 1;
              unsigned char *nb2 = (unsigned char*)p_malloc(newsz);
              if (!nb2){ logline("cluster/thread","body alloc failed"); break; }
              { unsigned long k; for (k = 0; k < rcap; k++) nb2[k] = rbuf[k]; }
              if (p_free && rbuf) p_free(rbuf);
              rbuf = nb2; rcap = newsz;
            } else { logline("cluster/thread","no malloc"); break; }
          }
          got = 0;
          while (got < (long)bs){ n = read(connfd, rbuf + got, (long)bs - got); if (n <= 0){ logkv("cluster/thread","body read ended (0=EOF,1=err)",(u64)(n==0?0:1),0); logkv("cluster/thread","  bodySize",(u64)bs,0); logkv("cluster/thread","  bytes got",(u64)got,0); stop = 1; break; } got += n; }
          if (stop) break;
          if (op == 1){
            /* codec config: UNENCRYPTED avcC (rgi avcc_config.h; the phone also
             * attaches screen-state flags -- hdr[6] -- which we log) */
            configs++;
            logkv("cluster/thread","codec-config body",(u64)bs,0);
            logkv("cluster/thread","config flags",(u64)(unsigned)hdr[6],0);
            { float w = 0, h = 0;   /* sender's encoded size @ hdr+16/+20 (rgi) */
              unsigned char *pw = (unsigned char*)&w, *ph = (unsigned char*)&h;
              int k;
              for (k = 0; k < 4; k++){ pw[k] = hdr[16+k]; ph[k] = hdr[20+k]; }
              logkv("cluster/thread","encoded size w",(u64)(u32)(w>0?((unsigned)(w+0.5f)):0),0);
              logkv("cluster/thread","encoded size h",(u64)(u32)(h>0?((unsigned)(h+0.5f)):0),0); }
            { unsigned char cfgout[4096]; unsigned cfglen = 0;
              unsigned nls = avcc_config_to_annexb(rbuf, bs, cfgout, sizeof(cfgout), &cfglen);
              if (nls){
                ls = (unsigned char)nls; g_cl_ls = ls; have_cfg = 1;
                { unsigned long k; for (k = 0; k < cfglen && k < sizeof(g_cl_cfg_annexb); k++)
                    g_cl_cfg_annexb[k] = cfgout[k]; }
                g_cl_cfg_len = cfglen < sizeof(g_cl_cfg_annexb) ? cfglen : sizeof(g_cl_cfg_annexb);
                logkv("cluster/thread","avcC -> Annex-B SPS/PPS bytes",(u64)g_cl_cfg_len,0);
                logkv("cluster/thread","NAL lengthSize",(u64)(unsigned)ls,0);
              } else logline("cluster/thread","opcode-1: not an avcC record (kept, no SPS/PPS)"); }
          }
          else if (op == 0){
            /* video AU -- decrypt per the stream's security.  THE WIRE FORMAT
             * (corpus ProcessFrames 0x11fbf0, verified): chacha bodies are
             * ciphertext||16-byte Poly1305 tag, AAD = the 128-byte header,
             * nonce = the 8-byte LE frame counter (init_64x64 reads 8 bytes);
             * verify() MUST be called and n+m must equal bodySize-16.  AES-CTR
             * is a PERSISTENT keystream (Init ONCE, Update per frame). */
            long pl = 0; int dec_ok = 0;
            if (c->cipher == 2 && p_cx_init && p_cx_aad && p_cx_dec){
              if (bs >= 16){
                unsigned char nonce8[8]; int k; unsigned long long ctr = (unsigned long long)c->counter;
                for (k = 0; k < 8; k++) nonce8[k] = (unsigned char)(ctr >> (k * 8));
                p_cx_init(cxbuf, c->key, nonce8);
                p_cx_aad(cxbuf, hdr, 128);
                { long dn = p_cx_dec(cxbuf, rbuf, (long)bs - 16, rbuf);   /* in place, tag excluded */
                  int okflag = -1; long vm = 0;
                  if (p_cx_ver) vm = p_cx_ver(cxbuf, rbuf + dn, rbuf + (long)bs - 16, &okflag);
                  if (okflag == 0 && dn + vm == (long)bs - 16){ pl = (long)bs - 16; dec_ok = 1; }
                  else { logkv("cluster/thread","chacha verify FAILED ok",(u64)(u32)okflag,0);
                         logkv("cluster/thread","  dn",(u64)(u64)dn,0); logkv("cluster/thread","  vm",(u64)(u64)vm,0); }
                }
              } else logline("cluster/thread","chacha: body < 16 (no tag) -- dropped");
            } else if (c->cipher == 1 && p_ctr_init && p_ctr_upd){
              if (!c->aes_inited){        /* ONE init: the keystream persists across frames */
                p_ctr_init(cxbuf, c->key, c->key + 16);
                c->aes_inited = 1;
              }
              p_ctr_upd(cxbuf, rbuf, (long)bs, rbuf); pl = (long)bs; dec_ok = 1;
            } else { pl = (long)bs; dec_ok = (c->cipher == 0); }  /* no key: pass-through (diagnosis only) */
            c->counter++; frames++;
            if (dec_ok && pl > 0){
              int is_idr = 0;
              if (avcc_prepare(rbuf, (unsigned long)pl, ls, &is_idr) == 0){
                if (is_idr){
                  idrs++;
                  if (!first_idr_sent){
                    first_idr_sent = 1;
                    logkv("cluster/thread","FIRST IDR at frame",(u64)frames,0);
                    /* rgi production timing: showUI + night + IDR refresh at the
                     * first decodable frame (the stream is now a real canvas) */
                    cluster_send_showui(c->session, "first-IDR");
                    cluster_send_nightmode(c->session);
                    cluster_send_forcekeyframe(c->session, "first-IDR");
                    cluster_send_appearance_armed(c->session);   /* no-op unless MIB3_SHIM_APPEARANCE=1 */
                  }
                }
                /* The receiver hands us SLICES ONLY -- the parameter sets arrive
                 * out of band (the opcode-1 avcC record handled above) and are
                 * NOT present in the media AUs.  A decoder fed bare slices with
                 * no SPS/PPS decodes nothing; it conceals to flat black (that is
                 * exactly the "decodes but the surface is black" symptom).  So
                 * stamp the SPS/PPS onto EVERY AU: ~32 bytes, and it makes each
                 * AU self-contained, which also makes the helper robust across
                 * reconnects. */
                if (g_cl_cfg_len > 0) sink_feed(g_cl_cfg_annexb, g_cl_cfg_len, 0);
                { int was_conn = mib3shim_helper_connected();
                  sink_feed(rbuf, (unsigned long)pl, 0);
                  /* A freshly (re)connected helper has no reference frames, so
                   * ask the phone for a keyframe and a real IDR will follow. */
                  if (!was_conn && mib3shim_helper_connected()){
                    /* UNCONDITIONAL -- this is the one request that must never be
                     * rate-limited.  It fires at most once per helper connection
                     * (guarded by !was_conn), and it is what makes a freshly
                     * attached decoder start on an IDR instead of mid-GOP.
                     * The storm limiter added on 2026-10-18 suppressed it whenever
                     * a stream-start request was <5 s old -- i.e. on every
                     * reconnect.  Measured 2026-10-19: forceKeyFrame rc=0 at
                     * stream-start, then "rate-limited (s since last)=1" for
                     * helper-connect, decoder got 77 P-slices, qdrop=279, frozen.
                     * The 2026-10-18 working build sent it unconditionally. */
                    cluster_send_forcekeyframe(c->session, "helper-connect"); } }
              } else logline("cluster/thread","malformed AU (avcc_prepare) -- dropped");
            }
            if ((frames & 0x3f) == 1){
              char hs[40]; int k; const char *H = "0123456789abcdef";
              unsigned lim = (bs < 16) ? (unsigned)bs : 16u;
              for (k = 0; k < (int)lim; k++){ unsigned char cc = (unsigned char)rbuf[k]; hs[k*2]=H[cc>>4]; hs[k*2+1]=H[cc&15]; }
              hs[lim*2] = 0;
              logkv("cluster/thread","frame",(u64)frames,0);
              logkv("cluster/thread","body",(u64)bs,0);
              logkv("cluster/thread","plain",(u64)pl,0);
              logline("cluster/thread/plain-head", hs);   /* want 00000001 (Annex-B NAL) */
            }
          } else {
            others++;   /* opcodes 2/4/5: keepalive/misc -- previously ignored.
                         * Navigation start drops the whole session, and control
                         * traffic is the prime suspect, so LOG ALL OF IT.
                         *
                         * NAMED 2026-10-19 (view-area lane) from the SHIPPED
                         * binary -- corpus libAirPlay.so decompiled.c
                         * AirPlayReceiverSessionScreen_ProcessFrames (0x11fbf0):
                         *   opcode 0 -> video AU (decrypt, ScreenStreamProcessData)
                         *   opcode 1 -> avcC + SetWidthHeight + SetAVCC
                         *   opcode 2 -> goto "done" (local_dc = 0) SILENTLY
                         *   opcode 4 -> same  ((byte)(op-4) < 2)
                         *   opcode 5 -> same
                         *   anything else -> "### Unknown screen opcode: %u" + -6727
                         * So 2/4/5 are INERT BY CONSTRUCTION: the unit's own
                         * receiver discards them with no error and no state
                         * change.  They are not a hidden control channel.
                         * NOTE: requestUI is NOT a screen opcode at all -- it is
                         * an RTSP /command type on the phone->host direction
                         * (LIVI stack.rs:1604, PlayPort AirPlaySession.kt:541);
                         * see the lane doc for why no seam reaches it today. */
            { const char *H = "0123456789abcdef"; int k; char hs[96]; char bh[192];
              unsigned lim = 48, bl = (bs < 64u) ? (unsigned)bs : 64u;
              for (k = 0; k < (int)lim; k++){ unsigned char cc = hdr[k]; hs[k*2] = H[cc>>4]; hs[k*2+1] = H[cc&15]; }
              hs[lim*2] = 0;
              for (k = 0; k < (int)bl; k++){ unsigned char cc = rbuf[k]; bh[k*2] = H[cc>>4]; bh[k*2+1] = H[cc&15]; }
              bh[bl*2] = 0;
              logkv("cluster/ctl","opcode",(u64)op,0);
              logkv("cluster/ctl","bodySize",(u64)bs,0);
              if (op == 2)      logline("cluster/ctl/meaning","opcode 2 = stock-ignored keepalive/misc (no error, no state change)");
              else if (op == 4) logline("cluster/ctl/meaning","opcode 4 = stock-ignored keepalive/misc (no error, no state change)");
              else if (op == 5) logline("cluster/ctl/meaning","opcode 5 = stock-ignored telemetry/keepalive (no error, no state change)");
              else              logline("cluster/ctl/meaning","UNKNOWN opcode (stock returns -6727 '### Unknown screen opcode')");
              logline("cluster/ctl/hdr-head", hs);
              logline("cluster/ctl/body-head", bh); } }
        }
      }
      logkv("cluster/thread","frames total",(u64)frames,0);
      logkv("cluster/thread","idrs total",(u64)idrs,0);
      logkv("cluster/thread","configs",(u64)configs,0);
      logkv("cluster/thread","other-opcodes",(u64)others,0);
      if (p_free && rbuf) p_free(rbuf);
      logline("cluster/thread","stream closed");
      /* TELL THE DECODER THE STREAM IS OVER.  Nothing downstream can see the
       * phone hang up otherwise: the AU socket stays open, so v4l2_seq's read()
       * never returns 0, it keeps running, and the presenter keeps re-committing
       * its LAST frame -- which is exactly why the cluster shows a map of a road
       * that no longer exists, forever (LANE_CLUSTER_TRANSITION_2026-10-18).
       * Closing the helper socket IS the EOF the decoder already handles
       * (`online: shim gone` -> re-accept), so no decoder change is needed.
       * GUARD: only the NEWEST cluster thread may close it -- an older session's
       * thread exiting must not tear down a newer session's AU channel (the unit
       * measured 8 connects / 7 disconnects in one run). */
      if (cl_thread_gen == cl_my_gen)
        { helper_close(); logline("cluster/thread","helper socket closed (EOF for the decoder)"); }
      else
        logline("cluster/thread","a newer cluster thread owns the helper socket -- not closing it");
    }
    if (p_close) p_close(connfd);
  } else {
    logline("cluster/thread","accept failed or none");
  }
  logline("cluster/thread","exit");
  if (p_free) p_free(c);
  return 0;
}

/* ---- our own listener (exactly what LIVI and PlayPort do, plus the port
 * chain the unit's own FIREWALL forces -- see the redteam block above).
 * Both working receivers bind a WILDCARD TCP listener and advertise whatever
 * port it got.  On THIS unit the phone-facing interfaces (ncm0 for wired USB,
 * wlap* for wireless) have an iptables/ip6tables default-DENY allowlist whose
 * only carplay TCP ports are 5000:5001 / 6030 / 7000 (lge/app_ro/network's
 * embedded template, section "## carplay for ncm0 (usb interface)").  The
 * stock event listener owns 5001 and the stock screen owns 6030, so the only
 * firewall-legal free port is 5000.  6031 (the MHI3-native alt port) and any
 * ephemeral port are DROPPED by the firewall -- the phone's SYN never arrives
 * -- so they are fallbacks that require live_kit/cluster_port_fw.sh to insert
 * the RAM-only allow rule first. */
static int own_listen(int *out_port, int *out_gen){
  extern int socket(int, int, int);
  extern int setsockopt(int, int, int, const void*, unsigned int);
  extern int bind(int, const void*, unsigned int);
  extern int listen(int, int);
  extern int getsockname(int, void*, unsigned int*);
  extern int close(int);
  struct in6 { unsigned char b[16]; };
  struct sa6 { unsigned short family, port; unsigned int flow; struct in6 addr; unsigned int scope; };
  int fd, one = 1;
  struct sa6 sa, got; unsigned int glen = sizeof(got), i;
  unsigned char *p;
  /* port chain: explicit knob -> 5000 (fw-allowlisted, stock-observed free) ->
   * 6031 (MHI3-native alt; fw-DROPPED, needs cluster_port_fw.sh) -> ephemeral
   * (fw-DROPPED, needs cluster_port_fw.sh).  Log every attempt so the log
   * shows exactly which link of the chain won. */
  unsigned long chain[4]; int nc = 0;
  { char *pe = getenv("MIB3_SHIM_PORT");
    unsigned long pv = 0;
    if (pe && pe[0] >= '0' && pe[0] <= '9'){ while (*pe >= '0' && *pe <= '9'){ pv = pv*10 + (*pe - '0'); pe++; } }
    if (pv) chain[nc++] = pv; }
  chain[nc++] = 5000;
  chain[nc++] = 6031;
  chain[nc++] = 0;   /* ephemeral */
  /* SUPERSEDE THE PREVIOUS LISTENER FIRST (stability, 2026-10-18).
   * A listener left over from an earlier type-111 SETUP holds 5000, and
   * SO_REUSEADDR does NOT permit a second live listener on the same port, so
   * without this the bind would fail and we would fall through to 6031 -- a
   * port the phone cannot reach (see the listener-ownership note in
   * cluster_thread).  Closing it here also unsticks the old thread's poll. */
  if (g_lsock >= 0){
    int old = g_lsock; g_lsock = -1; g_lsock_gen++; close(old);
    logkv("cluster/listen","superseded old listener fd",(u64)(u32)old,0);
    logkv("cluster/listen","new listener generation",(u64)(u32)g_lsock_gen,0);
  }
  fd = socket(10 /*AF_INET6*/, 1 /*SOCK_STREAM*/, 0);
  if (fd < 0) return -1;
  setsockopt(fd, 1 /*SOL_SOCKET*/, 2 /*SO_REUSEADDR*/, &one, sizeof(one));
  { int zero = 0;
    int r = setsockopt(fd, 41 /*IPPROTO_IPV6*/, 26 /*IPV6_V6ONLY*/, &zero, sizeof(zero));
    logkv("cluster/listen","IPV6_V6ONLY=0 rc",(u64)(u32)r,0); }
  p = (unsigned char*)&sa; for (i = 0; i < sizeof(sa); i++) p[i] = 0;
  sa.family = 10;
  { int ci; int bound = 0;
    for (ci = 0; ci < nc && !bound; ci++){
      unsigned long pt = chain[ci];
      sa.port = (unsigned short)((pt << 8) | (pt >> 8));   /* network byte order */
      if (bind(fd, &sa, sizeof(sa)) == 0) bound = 1;
      logkv("cluster/listen","try port",(u64)pt,0);
      if (!bound) logkv("cluster/listen","bind failed port",(u64)pt,0);
    }
    if (!bound){ close(fd); return -1; }
  }
  if (listen(fd, 8) != 0){ close(fd); return -1; }
  g_lsock = fd;                 /* we own it until a newer SETUP supersedes it */
  g_lsock_gen++;
  if (out_gen) *out_gen = g_lsock_gen;
  *out_port = 0;
  if (getsockname(fd, &got, &glen) == 0){
    *out_port = (int)(((got.port & 0xff) << 8) | (got.port >> 8));   /* ntohs */
    logkv("cluster/listen","bound family",(u64)(u32)got.family,0);
    logkv("cluster/listen","bound port",(u64)(u32)(*out_port),0);
    { const char *fw = (*out_port == 5000 || *out_port == 5001) ? "FW-ALLOWED (ncm0 5000:5001)"
                      : (*out_port == 6030 || *out_port == 7000) ? "FW-ALLOWED but stock-owned"
                      : "FW-DROPPED (needs live_kit/cluster_port_fw.sh)";
      logline("cluster/listen/fw", fw); }
  }
  return fd;
}

/* Set up a second screen for a type-111 stream dict and append
 * {type:111, dataPort} to response["streams"].  Returns 0 on success. */
static int cluster_setup(void *session, void *streamDict, void *response){
  if (!session || !streamDict || !response){ logline("cluster","bad args"); return -1; }
  /* SELF-CONTAINED RECEIVER -- no stock ScreenStream.
   * A stock alt screen binds the MAIN display and starts a second in-dio OMX
   * decoder on this unit: black main screen + decoder crash (the reference
   * receiver luka-dev/mib2q-carplay-rgi hit the identical wall and abandoned the
   * stock screen entirely).  So we own the listener and service the stream
   * ourselves; nothing here touches the unit's screen/stream machinery. */
  long connID = (long)dgeti64k((CFDictionaryRef)streamDict, "streamConnectionID", 0);
  logkv("cluster","streamConnectionID",(u64)connID,0);

  /* Stream key: mirror stock _ScreenSetup (corpus libAirPlay case 0x6e) --
   * AES when session+0x478==0, else chacha via the unit's FUN_001163d8.
   * AES: 16-byte key at [0..16) + 16-byte IV at [16..32). */
  unsigned char skey[32]; int keylen = 0, cipher = 0, i;
  for (i = 0; i < 32; i++) skey[i] = 0;
  if (*(void**)((char*)session + 0x478) == 0){
    if (p_DeriveAES){ p_DeriveAES((char*)session + 0x268, 0x10, connID, skey, skey + 16);
                      keylen = 16; cipher = 1; logline("cluster","key: AES-CTR"); }
    else logline("cluster","no AES derive");
  } else {
    fn_chachaDerive ch = g_libap_base ? (fn_chachaDerive)(g_libap_base + CHACHA_DERIVE_RVA) : 0;
    if (ch && ch((char*)session + 0x478, connID, 0, 0, skey) == 0){
      keylen = 32; cipher = 2; logline("cluster","key: chacha20-poly1305"); }
    else logline("cluster","chacha derive failed");
  }

  int port = 0, lgen = 0, lsock = own_listen(&port, &lgen);
  if (lsock < 0){ logline("cluster","own listener failed"); goto fail_sock; }
  logkv("cluster","dataPort",(u64)(u32)port,0);

  /* append {type:111, dataPort:port} to response["streams"].  Mirrors the unit's
   * _AddResponseStream (which CFReleases the array after inserting it). */
  if (p_CFDictionaryCreateMutable && p_CFDictionarySetInt64 && p_CFArrayAppendValue &&
      p_CFDictionaryGetValue && p_CFDictionarySetValue && p_kCFLDictKeyCB && p_kCFLDictValCB){
    CFMutableDictionaryRef desc = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
    if (desc){
      dseti(desc, "type", MIB3_CLUSTER_TYPE);
      dseti(desc, "dataPort", (s64)port);
      /* rgi's paired/authenticated build also echoes the request's
       * streamConnectionID in the response descriptor (altscreen_hook.c:1780).
       * Our stock 110 descriptor proves it is not REQUIRED on this unit
       * (stock sends only {type,dataPort} and the phone connects), so this is
       * a belt knob: MIB3_SHIM_CONNID=1. */
      if (env_on("MIB3_SHIM_CONNID",0) && connID)
        dseti(desc, "streamConnectionID", connID);
      CFMutableArrayRef streams = 0;
      { CFStringRef ks = sstr("streams");
        if (ks){ streams = (CFMutableArrayRef)p_CFDictionaryGetValue((CFDictionaryRef)response, ks);
                 if (p_CFRelease) p_CFRelease(ks); } }
      int created = 0;
      if (!streams){
        fn_CFArrayCreateMutable mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
        if (mka && p_kCFTypeArrayCallBacks){
          streams = mka(0,0,p_kCFTypeArrayCallBacks);
          if (streams){ dseto((CFMutableDictionaryRef)response, "streams", streams); created = 1; }
        }
      }
      if (streams) p_CFArrayAppendValue(streams, desc);
      /* dump exactly what the response's streams array now holds */
      if (streams && p_CFArrayGetCount && p_CFArrayGetValueAtIndex && p_CFGetTypeID && p_CFDictionaryGetTypeID){
        CFIndex j, m = p_CFArrayGetCount(streams);
        logkv("cluster","response streams count",(u64)(u32)m,0);
        for (j = 0; j < m; j++){
          void *e = (void*)p_CFArrayGetValueAtIndex(streams, j);
          if (e && p_CFGetTypeID(e) == p_CFDictionaryGetTypeID()){
            s64 et = dgeti64k((CFDictionaryRef)e, "type", -1);
            s64 ep = dgeti64k((CFDictionaryRef)e, "dataPort", -1);
            logkv("cluster","  stream type",(u64)et,0);
            logkv("cluster","  stream dataPort",(u64)ep,0);
          }
        }
      }
      if (p_CFRelease) p_CFRelease(desc);         /* array retains it */
      if (created && p_CFRelease) p_CFRelease(streams); /* dict retains it */
      logline("cluster","response entry {type:111,dataPort} appended");
    }
  }

  /* ROUTE-B sink.* THE SINK IS **DEFERRED** TO THE RECEIVER THREAD (see
   * cluster_thread -> sink_start) and is NOT created here.
   *
   * Doing display-pipeline work inside THIS seam (the CarPlay SETUP path) is
   * exactly what blacked the centre screen and reset the unit on 2026-10-16:
   * a blocking/rupturing Cinemo or EGL call here stalls CarPlay session setup,
   * and carplay.service is FailureAction=reboot-immediate.  The setup seam must
   * return the response immediately and touch nothing.  So we only RESOLVE the
   * mode here (and log it); the actual sink stands up on our own thread after
   * the phone connects.  See the SINK DISPATCHER comment for the modes. */
  sink_resolve();
  { char m[96]; buf b = { m, sizeof(m)-2, 0 };
    bs(&b,"mode="); bu64(&b,(u64)(u32)g_sink,10,0);
    bs(&b," local="); bu64(&b,(u64)(u32)g_sink_local,10,0);
    bs(&b," (deferred to cluster/thread)"); m[b.n]=0;
    logline("cluster/sink", m); }

  /* spawn the servicing thread with its OWN heap context (no shared mutable
   * state a later setup could clobber -> no cross-thread UAF/corruption). */
  if (!p_malloc || !p_pthreadCreate){
    logline("cluster","no malloc/pthread -> cannot service stream");
    goto fail_sock;
  }
  cl_ctx *c = (cl_ctx*)p_malloc(sizeof(cl_ctx));
  if (!c){ logline("cluster","ctx alloc failed"); goto fail_sock; }
  c->screen = 0; c->lsock = lsock; c->lsockgen = lgen; c->session = session; c->stream = 0;
  { int j; for (j = 0; j < 32; j++) c->key[j] = skey[j]; }
  c->keylen = keylen; c->cipher = cipher; c->counter = 0;
  c->night = g_night_mode; c->aes_inited = 0;
  g_cmd_session = session;   /* the command target once the stream connects */
  g_cmd_gen++;               /* invalidate completions from the previous SETUP */
  /* the wheel->CarPlay zoom path (LANE_CLUSTER_ZOOM_2026-10-18): one listener
   * per process, started here so it outlives per-connection cluster threads.
   * It only QUEUES steps; the cluster thread ships them (no session lifetime
   * contract is added). */
  (void)zoom_listen_start();
  {
    char *cfg = (char*)(*(void**)((char*)session + 0x10));
    c->flags = cfg ? *(int*)(cfg + 0xac) : 0;
  }
  void *th = 0;
  if (p_pthreadCreate(&th, 0, cluster_thread, c) != 0){
    logline("cluster","pthread_create failed");
    if (p_free) p_free(c);
    goto fail_sock;
  }
  return 0;

fail_sock:
  if (g_lsock >= 0 && g_lsock == lsock){ g_lsock = -1; g_lsock_gen++; }
  if (p_close && lsock >= 0) p_close(lsock);
  return -1;
}

/* exported for the harness: drive the cluster path directly. */
int mib3shim_cluster_setup(void *session, void *streamDict, void *response){
  resolve();
  return cluster_setup(session, streamDict, response);
}

/* ==========================================================================
 * ROUTE-B: the shim's OWN cluster surface (Wayland/ivi + EGL/GLES).
 *
 * The CarPlay media engine is a one-window singleton (see
 * LANE_SURFACE_BINDING_DESK_2026-10-09.md): every stream's decoded frames land
 * in the SAME surface.  So the cluster stream cannot get its own surface from
 * the framework; the shim must create one and paint the cluster frames into it.
 *
 * The call sequence mirrors the unit's own window/EGL device exactly (grounded
 * in corpus/libNmeVideoDevice.so):
 *   NmeWindowWayland (FUN_0013b180):  wl_display_connect -> get_registry ->
 *       registry listener binds {wl_compositor, ivi_application} ->
 *       wl_display_roundtrip -> wl_compositor_create_surface ->
 *       wl_egl_window_create -> ivi_application_surface_create
 *   NmeDeviceEGL (CreateContext):     eglGetDisplay -> eglInitialize ->
 *       eglBindAPI(OPENGL_ES) -> eglChooseConfig -> eglCreateWindowSurface ->
 *       eglCreateContext -> eglMakeCurrent
 *   frame write (NmeVideoDevice render): glViewport -> glClear ->
 *       glBindTexture -> glTexSubImage2D -> glDrawArrays -> eglSwapBuffers
 *
 * All symbols are dlsym'd; on the desk the bench preloads a stub for the
 * Adreno-locked graphics stack.  Nothing here changes the stock seams: the
 * surface path is additive and env-gated (MIB3_SHIM_SURFACE).
 * ========================================================================*/
static int streq_(const char *a, const char *b){ while(*a && *a==*b){a++;b++;} return *a==*b; }

typedef struct {
  int   resolved, ready;
  void *display, *registry, *compositor, *ivi_app, *surface, *eglwin, *ivi_surf;
  void *egl_dpy, *egl_cfg, *egl_surf, *egl_ctx;
  unsigned tex;
  int   w, h;
} gfx_t;
static gfx_t g_gfx;

static void resolve_gfx(void){
  if (g_gfx.resolved) return;
  g_gfx.resolved = 1;
  p_wl_display_connect   = (fn_wl_connect)       R("wl_display_connect");
  p_wl_display_disconnect= (fn_wl_disp_void)     R("wl_display_disconnect");
  p_wl_display_get_fd    = (fn_wl_disp_int)      R("wl_display_get_fd");
  p_wl_display_roundtrip = (fn_wl_disp_int)      R("wl_display_roundtrip");
  p_wl_display_dispatch_pending = (fn_wl_disp_int)R("wl_display_dispatch_pending");
  p_wl_display_flush     = (fn_wl_disp_int)      R("wl_display_flush");
  p_wl_pmc               = (fn_wl_pmc)           R("wl_proxy_marshal_constructor");
  p_wl_pmcv              = (fn_wl_pmcv)          R("wl_proxy_marshal_constructor_versioned");
  p_wl_proxy_add_listener= (fn_wl_add_listener)  R("wl_proxy_add_listener");
  p_wl_proxy_destroy     = (fn_wl_destroy)       R("wl_proxy_destroy");
  p_wl_egl_window_create = (fn_wl_eglwin_create) R("wl_egl_window_create");
  p_wl_egl_window_destroy= (fn_wl_eglwin_destroy)R("wl_egl_window_destroy");
  p_eglGetDisplay        = (fn_eglGetDisplay)    R("eglGetDisplay");
  p_eglInitialize        = (fn_eglInitialize)    R("eglInitialize");
  p_eglBindAPI           = (fn_eglBindAPI)       R("eglBindAPI");
  p_eglChooseConfig      = (fn_eglChooseConfig)  R("eglChooseConfig");
  p_eglCreateWindowSurface=(fn_eglCreateWindowSurface)R("eglCreateWindowSurface");
  p_eglCreateContext     = (fn_eglCreateContext) R("eglCreateContext");
  p_eglMakeCurrent       = (fn_eglMakeCurrent)   R("eglMakeCurrent");
  p_eglSwapBuffers       = (fn_eglSwapBuffers)   R("eglSwapBuffers");
  p_glClear              = (fn_gl_void1)         R("glClear");
  p_glViewport           = (fn_gl_void4)         R("glViewport");
  p_glGenTextures        = (fn_gl_gen)           R("glGenTextures");
  p_glBindTexture        = (fn_gl_bind)          R("glBindTexture");
  p_glTexImage2D         = (fn_gl_tex)           R("glTexImage2D");
  p_glTexSubImage2D      = (fn_gl_tex)           R("glTexSubImage2D");
  p_glDrawArrays         = (fn_gl_draw)          R("glDrawArrays");
  p_wl_registry_interface      = R("wl_registry_interface");
  p_wl_compositor_interface    = R("wl_compositor_interface");
  p_wl_surface_interface       = R("wl_surface_interface");
  p_ivi_application_interface  = R("ivi_application_interface");
  p_ivi_surface_interface      = R("ivi_surface_interface");
  logline("gfx","resolved");
  logkv("gfx","wl_display_connect",(u64)p_wl_display_connect,1);
  logkv("gfx","eglGetDisplay",(u64)p_eglGetDisplay,1);
  logkv("gfx","eglCreateWindowSurface",(u64)p_eglCreateWindowSurface,1);
  logkv("gfx","glDrawArrays",(u64)p_glDrawArrays,1);
}

/* Wayland registry listener: bind the globals the shim needs (compositor +
 * ivi_application).  Bound via wl_proxy_marshal_constructor_versioned
 * (the exported form of the wl_registry_bind() inline). */
static void reg_global(void *data, void *reg, u32 name, const char *iface, u32 version){
  gfx_t *g = (gfx_t*)data;
  if (!iface || !p_wl_pmcv) return;
  if (streq_(iface,"wl_compositor") && p_wl_compositor_interface){
    g->compositor = p_wl_pmcv(reg, 0, p_wl_compositor_interface, version, name, iface, version, (void*)0);
    logline("surface","bind wl_compositor");
  } else if (streq_(iface,"ivi_application") && p_ivi_application_interface){
    g->ivi_app = p_wl_pmcv(reg, 0, p_ivi_application_interface, version, name, iface, version, (void*)0);
    logline("surface","bind ivi_application");
  }
}
static void reg_global_remove(void *data, void *reg, u32 name){ (void)data;(void)reg;(void)name; }
static void *g_reg_listener[2];

static int surface_create(int w, int h, unsigned ivi_id){
  resolve_gfx();
  if (!p_wl_display_connect){ logline("surface","FAIL: no wl_display_connect"); return -1; }
  if (g_gfx.ready) { logline("surface","already ready"); return 0; }
  g_gfx.w = w; g_gfx.h = h;
  logline("surface","-- create begin --");

  g_gfx.display = p_wl_display_connect(0);   /* NULL -> $WAYLAND_DISPLAY / default */
  if (!g_gfx.display){ logline("surface","FAIL: wl_display_connect"); return -1; }
  logline("surface","wl_display_connect ok");

  if (!p_wl_pmc || !p_wl_registry_interface){ logline("surface","FAIL: no marshal/registry iface"); return -1; }
  g_gfx.registry = p_wl_pmc(g_gfx.display, 1 /*WL_DISPLAY_GET_REGISTRY*/, p_wl_registry_interface, (void*)0);
  if (!g_gfx.registry){ logline("surface","FAIL: wl_display_get_registry"); return -1; }
  logline("surface","wl_display_get_registry ok");

  g_reg_listener[0] = (void*)reg_global;
  g_reg_listener[1] = (void*)reg_global_remove;
  if (p_wl_proxy_add_listener) p_wl_proxy_add_listener(g_gfx.registry, g_reg_listener, &g_gfx);
  if (p_wl_display_roundtrip) p_wl_display_roundtrip(g_gfx.display);
  if (!g_gfx.compositor){ logline("surface","FAIL: no wl_compositor bound"); return -1; }
  if (!g_gfx.ivi_app)   logline("surface","warn: no ivi_application bound (ivi surface skipped)");

  g_gfx.surface = p_wl_pmc(g_gfx.compositor, 0 /*WL_COMPOSITOR_CREATE_SURFACE*/, p_wl_surface_interface, (void*)0);
  if (!g_gfx.surface){ logline("surface","FAIL: wl_compositor_create_surface"); return -1; }
  logline("surface","wl_surface created");

  if (!p_wl_egl_window_create){ logline("surface","FAIL: no wl_egl_window_create"); return -1; }
  g_gfx.eglwin = p_wl_egl_window_create(g_gfx.surface, w, h);
  if (!g_gfx.eglwin){ logline("surface","FAIL: wl_egl_window_create"); return -1; }
  logline("surface","wl_egl_window created");

  /* ivi_surface (the car's surface-id identity) */
  if (g_gfx.ivi_app && p_ivi_surface_interface)
    g_gfx.ivi_surf = p_wl_pmc(g_gfx.ivi_app, 0 /*IVI_APPLICATION_SURFACE_CREATE*/, p_ivi_surface_interface,
                              ivi_id, g_gfx.surface, (void*)0);
  logkv("surface","ivi_surface_id",(u64)ivi_id,0);
  if (g_gfx.ivi_surf) logline("surface","ivi_surface created");

  /* EGL (grounded in NmeDeviceEGL::CreateContext) */
  if (!p_eglGetDisplay){ logline("surface","FAIL: no eglGetDisplay"); return -1; }
  g_gfx.egl_dpy = p_eglGetDisplay(0);        /* EGL_DEFAULT_DISPLAY */
  if (!g_gfx.egl_dpy){ logline("surface","FAIL: eglGetDisplay"); return -1; }
  if (!p_eglInitialize || p_eglInitialize(g_gfx.egl_dpy,0,0)==0){ logline("surface","FAIL: eglInitialize"); return -1; }
  if (p_eglBindAPI) p_eglBindAPI(0x30a0);    /* EGL_OPENGL_ES_API */
  {
    int n = 0; void *cfg = 0; int cfgattr[1]; cfgattr[0]=0x3038; /* EGL_NONE */
    if (!p_eglChooseConfig || p_eglChooseConfig(g_gfx.egl_dpy,cfgattr,&cfg,1,&n)==0 || !cfg){
      logline("surface","FAIL: eglChooseConfig"); return -1; }
    g_gfx.egl_cfg = cfg;
  }
  if (!p_eglCreateWindowSurface){ logline("surface","FAIL: no eglCreateWindowSurface"); return -1; }
  g_gfx.egl_surf = p_eglCreateWindowSurface(g_gfx.egl_dpy, g_gfx.egl_cfg, g_gfx.eglwin, (void*)0);
  if (!g_gfx.egl_surf){ logline("surface","FAIL: eglCreateWindowSurface"); return -1; }
  if (!p_eglCreateContext){ logline("surface","FAIL: no eglCreateContext"); return -1; }
  g_gfx.egl_ctx = p_eglCreateContext(g_gfx.egl_dpy, g_gfx.egl_cfg, 0, (void*)0);
  if (!g_gfx.egl_ctx){ logline("surface","FAIL: eglCreateContext"); return -1; }
  if (!p_eglMakeCurrent || p_eglMakeCurrent(g_gfx.egl_dpy, g_gfx.egl_surf, g_gfx.egl_surf, g_gfx.egl_ctx)==0){
    logline("surface","FAIL: eglMakeCurrent"); return -1; }
  logline("surface","EGL context current");

  /* a texture for the cluster frames */
  if (p_glGenTextures){ p_glGenTextures(1,&g_gfx.tex);
    if (p_glBindTexture) p_glBindTexture(0xde1 /*GL_TEXTURE_2D*/, g_gfx.tex);
    if (p_glTexImage2D) p_glTexImage2D(0xde1,0,0x1908 /*RGBA*/,w,h,0,0x1908,0x1401 /*UNSIGNED_BYTE*/,0); }

  g_gfx.ready = 1;
  logline("surface","-- create done (ready) --");
  return 0;
}

static int surface_write_frame(const void *px, int w, int h){
  if (!g_gfx.ready){ logline("frame","no surface ready"); return -1; }
  if (p_eglMakeCurrent) p_eglMakeCurrent(g_gfx.egl_dpy, g_gfx.egl_surf, g_gfx.egl_surf, g_gfx.egl_ctx);
  if (p_glViewport) p_glViewport(0,0,w,h);
  if (p_glClear)    p_glClear(0x4000 /*GL_COLOR_BUFFER_BIT*/);
  if (p_glBindTexture) p_glBindTexture(0xde1, g_gfx.tex);
  if (p_glTexSubImage2D && px)
    p_glTexSubImage2D(0xde1,0,0,0,w,h,0x1908,0x1401,px);
  if (p_glDrawArrays) p_glDrawArrays(6 /*GL_TRIANGLE_FAN*/,0,4);
  if (p_eglSwapBuffers) p_eglSwapBuffers(g_gfx.egl_dpy, g_gfx.egl_surf);
  return 0;
}

static const char *surf_name(void){
  char *v = getenv("MIB3_SHIM_SURFNAME");
  return (v && v[0]) ? v : "mib3-cluster-surface";
}

/* ---- exports for the harness -------------------------------------------- */
int  mib3shim_surface_create(int w, int h, unsigned ivi_id){ resolve(); return surface_create(w,h,ivi_id); }
int  mib3shim_surface_write_frame(const void *px, int w, int h){ resolve(); return surface_write_frame(px,w,h); }
int  mib3shim_surface_ready(void){ return g_gfx.ready; }
const char *mib3shim_surface_name(void){ return surf_name(); }
/* runtime-gate test hooks */
int  mib3shim_marker_present(void){ resolve(); return marker_present(); }
int  mib3shim_gate_active(void){ resolve(); return gate_active(); }
int  mib3shim_cluster_advertised(void){ return g_cluster_advertised; }

/* ==========================================================================
 * The display seam.
 * ========================================================================*/
/* ---- run banner ----------------------------------------------------------
 * Visual discriminator for the APPEND-only log: no more "read the tail and
 * guess where the run starts".  Printed as the first thing on the session-start
 * seam, so every phone plug is unambiguously bracketed with its own config.
 * Time is epoch % 86400 -> HH:MM:SS; the unit runs UTC. */
static long g_run_no = 0;
static void run_banner(void){
  extern long time(long *);
  extern int  getpid(void);
  long t = time(0);
  int  tzh = (int)env_num("MIB3_SHIM_TZ", 12);           /* hours WEST of the unit clock (tuned: unit clock is +4h) */
  long sod = (t - (long)tzh * 3600) % 86400; if (sod < 0) sod += 86400;
  int hh = (int)(sod / 3600), mm = (int)((sod % 3600) / 60), ss = (int)(sod % 60);
  char mem[768]; buf b = { mem, sizeof(mem)-2, 0 };
  bs(&b,"\n[mib3shim] ================================================================\n");
  bs(&b,"[mib3shim] ====== RUN "); bu64(&b,(u64)(++g_run_no),10,0);
  bs(&b,"  pid="); bu64(&b,(u64)getpid(),10,0);
  bs(&b,"  "); bu64(&b,(u64)hh,10,2); bs(&b,":"); bu64(&b,(u64)mm,10,2);
  bs(&b,":"); bu64(&b,(u64)ss,10,2); bs(&b," PST");
  bs(&b," ======\n");
  bs(&b,"[mib3shim] ====== knobs:  ADD="); bu64(&b,(u64)env_on("MIB3_SHIM_ADD",1),10,0);
  bs(&b,"  INFO="); bu64(&b,(u64)env_on("MIB3_SHIM_INFO",1),10,0);
  bs(&b,"  ENABLEDFEAT="); bu64(&b,(u64)env_on("MIB3_SHIM_ENABLEDFEAT",1),10,0);
  bs(&b,"  GATE="); bu64(&b,(u64)gate_active(),10,0);
  /* Re-read the sink knobs on EVERY plug.  target_env.py sets them LIVE, between
   * plugs, so a resolution cached from a previous plug would silently ignore the
   * change (the 2026-10-16 "banner still reads SURFACE=0" class of bug).  Reset
   * the breadcrumb fd too so the stick path is re-discovered if it just mounted. */
  g_sink = -1; bc_fd = -2;
  bs(&b,"  SURFACE="); bu64(&b,(u64)env_on("MIB3_SHIM_SURFACE",0),10,0);
  bs(&b,"  SINK="); sink_resolve(); bu64(&b,(u64)(u32)g_sink,10,0);
  bs(&b,"  LOCAL="); bu64(&b,(u64)(u32)g_sink_local,10,0);
  bs(&b,"  ACK="); bu64(&b,(u64)env_on("MIB3_SHIM_LOCAL_ACK",0),10,0);
  /* view-area / command-completion knobs (2026-10-19) */
  bs(&b,"  CMDACK="); bu64(&b,(u64)env_on("MIB3_SHIM_CMDACK",1),10,0);
  bs(&b,"  VIEWAREAS="); bu64(&b,(u64)env_on("MIB3_SHIM_VIEWAREAS",0),10,0);
  bs(&b,"  VA_COUNT="); bu64(&b,(u64)(unsigned)va_count(),10,0);
  bs(&b,"  VA_PRESET="); bu64(&b,(u64)env_on("MIB3_SHIM_VA_PRESET",1),10,0);
  bs(&b,"  VA_COUNT="); bu64(&b,(u64)(u32)va_count(),10,0);
  bs(&b,"  VA_CMD="); bu64(&b,(u64)env_on("MIB3_SHIM_VA_CMD",1),10,0);
  bs(&b,"  VA_VISIBLE="); bu64(&b,(u64)env_on("MIB3_SHIM_VA_VISIBLE",0),10,0);
  bs(&b,"  APPEARANCE="); bu64(&b,(u64)env_on("MIB3_SHIM_APPEARANCE",0),10,0);
  /* command-channel health, cumulative since injection: /0 accepted, /refused,
   * /stale.  A non-zero REFUSED or STALE here is the first thing to read when a
   * command "did nothing". */
  bs(&b,"  ACKOK="); bu64(&b,(u64)g_ack_ok,10,0);
  bs(&b,"  ACKREF="); bu64(&b,(u64)g_ack_refused,10,0);
  bs(&b,"  ACKSTALE="); bu64(&b,(u64)g_ack_stale,10,0);
  bs(&b," ======\n");
  bs(&b,"[mib3shim] ================================================================\n");
  mem[b.n]=0;
  log_open();
  if (logfd >= 0) write(logfd, mem, b.n);
}

int AirPlayInfoArrayAddScreenDisplay(CFMutableArrayRef *inArray,
                                     CFStringRef uuid,
                                     int features,
                                     int primaryInputDevice,
                                     int maxFPS,
                                     int widthPixels,
                                     int heightPixels,
                                     int widthPhysical,
                                     int heightPhysical)
{
  hit("ADD\n", 4);                       /* prove entry, independent of everything */
  run_banner();                          /* ==== RUN n / knobs ==== : per-plug discriminator */
  logline("AddScreenDisplay","ENTERED");  /* before resolve(): resolve() may fail */
  resolve();
  logline("AddScreenDisplay","CALL");
  log_uuid("AddScreenDisplay/uuid", uuid);
  logkv("AddScreenDisplay","features",(u64)(u32)features,1);
  logkv("AddScreenDisplay","primaryInputDevice",(u64)(u32)primaryInputDevice,1);
  logkv("AddScreenDisplay","maxFPS",(u64)(u32)maxFPS,0);
  logkv("AddScreenDisplay","widthPixels",(u64)(u32)widthPixels,0);
  logkv("AddScreenDisplay","heightPixels",(u64)(u32)heightPixels,0);
  logkv("AddScreenDisplay","widthPhysical",(u64)(u32)widthPhysical,0);
  logkv("AddScreenDisplay","heightPhysical",(u64)(u32)heightPhysical,0);

  if (!real_AddScreenDisplay) { logline("AddScreenDisplay","no real fn!"); return -1; }

  /* 1) the original main-screen call, exactly as LG would */
  int rc = real_AddScreenDisplay(inArray, uuid, features, primaryInputDevice,
                                 maxFPS, widthPixels, heightPixels, widthPhysical, heightPhysical);

  /* INSTRUMENTATION 2026-10-15: the cluster advertisement is gated on four
   * conditions. Log all of them so a silent skip becomes visible. */
  logkv("AddScreenDisplay","real rc",(u64)(u32)rc,1);
  logkv("AddScreenDisplay","saw_main",(u64)(u32)saw_main_display,1);
  logkv("AddScreenDisplay","ADD_env",(u64)(u32)env_on("MIB3_SHIM_ADD",1),1);
  logkv("AddScreenDisplay","gate",(u64)(u32)gate_active(),1);

  /* 2) advertise the cluster for THIS display list, after the main display.
   *    RUNTIME GATE: only when the stick marker is present (gate_active()).
   *    IDEMPOTENT PER LIST (not one-shot per process): add the cluster iff the
   *    list does not already carry a type-111 entry.  LG rebuilds `displays`
   *    every session, so the old one-shot latch meant only the FIRST phone plug
   *    after injection advertised -- later plugs still claimed altScreen with
   *    one display (contradiction -> session killed). */
  if (rc == 0 && env_on("MIB3_SHIM_ADD",1) && gate_active()) {
    if (array_has_cluster(inArray)) {
      g_cluster_advertised = 1;   /* present this session -> keep the claim consistent */
      logline("AddScreenDisplay","cluster already in displays -- no re-add");
    } else {
    saw_main_display = 1;
    char *cu_env = getenv("MIB3_SHIM_CUUID");
    const char *cu_s = (cu_env && cu_env[0]) ? cu_env : "4d494233-0000-4000-8000-000000000001";
    /* remember the EXACT cluster uuid string: the /info advert and the
     * showUI/forceKeyFrame/setNightMode command params must name the SAME display */
    { unsigned long n = 0; while (cu_s[n] && n < sizeof(g_cluster_uuid_str)-1){ g_cluster_uuid_str[n] = cu_s[n]; n++; }
      g_cluster_uuid_str[n] = 0; }
    /* the cluster scene url: used for BOTH the /info initialURL and showUI */
    { char *u = getenv("MIB3_SHIM_URL");
      const char *us = (u && u[0]) ? u : CLUSTER_URL_DEFAULT;
      unsigned long n = 0; while (us[n] && n < sizeof(g_cluster_url)-1){ g_cluster_url[n] = us[n]; n++; }
      g_cluster_url[n] = 0; }
    CFStringRef cu = p_CFStringCreateWithCString ? p_CFStringCreateWithCString(0,cu_s,K128) : 0;
    int cw   = (int)env_num("MIB3_SHIM_W", 1280);
    int chp  = (int)env_num("MIB3_SHIM_H", 480);   /* grounded cluster default: 1280x480 (LANE_SURFACE_BINDING_DESK §3a) */
    int cfps = (int)env_num("MIB3_SHIM_FPS", 30);  /* refs: rgi/PlayPort advertise 30 (LIVI 60) */
    logline("AddScreenDisplay","ADDING cluster display");
    log_uuid("AddScreenDisplay/cluster-uuid", cu);
    logkv("AddScreenDisplay","cluster widthPixels",(u64)(u32)cw,0);
    logkv("AddScreenDisplay","cluster heightPixels",(u64)(u32)chp,0);
    logkv("AddScreenDisplay","cluster maxFPS",(u64)(u32)cfps,0);
    /* cluster features/inputDevice: PlayPort's cluster config uses 0/0;
     * LIVI's working stack uses features=0x0A, primaryInputDevice=3. Env-tunable. */
    int cfeat  = (int)env_num("MIB3_SHIM_CFEAT", 0x0A);  /* LIVI/PlayPort: HIGH_FIDELITY_TOUCH|KNOBS = 0x0A */
    int cindev = (int)env_num("MIB3_SHIM_CINDEV", 0);
    /* real PHYSICAL size in mm, NOT the pixel size (LIVI mm_at_dpi / rgi 292x110) */
    int cwph = (int)env_num("MIB3_SHIM_CWPHY", 292);
    int chph = (int)env_num("MIB3_SHIM_CHPHY", 110);
    int rc2 = real_AddScreenDisplay(inArray, cu, cfeat, cindev, cfps, cw, chp, cwph, chph);
    logkv("AddScreenDisplay","cluster rc",(u64)(u32)rc2,0);
    if (rc2 == 0){ add_cluster_fields(inArray, cw, chp); g_cluster_advertised = 1; }
    logkv("AddScreenDisplay","cluster_advertised",(u64)(u32)g_cluster_advertised,0);
    if (env_on("MIB3_SHIM_CFSHOW",0) && p_CFShow && inArray && *inArray) { logline("AddScreenDisplay","CFShow(displays) ->"); p_CFShow(*inArray); }
    }
  }
  return rc;
}

/* ==========================================================================
 * The session-setup seam.
 * ========================================================================*/
int AirPlayReceiverSessionSetup(void *session, void *request, void **responseOut, int a4)
{
  hit("SETUP\n", 6);                      /* prove entry, independent of everything */
  logline("SessionSetup","ENTERED");       /* before resolve(): resolve() may fail */
  resolve();
  logline("SessionSetup","CALL");
  logkv("SessionSetup","session",(u64)session,1);
  logkv("SessionSetup","request",(u64)request,1);
  logkv("SessionSetup","responseOut",(u64)responseOut,1);

  int ret = 0, handled111 = 0;
  if (real_SessionSetup && !env_on("MIB3_SHIM_SKIP_REAL",0))
    ret = real_SessionSetup(session, request, responseOut, a4);   /* propagate the real status */
  logkv("SessionSetup","real ret",(u64)(u32)ret,1);

  if (responseOut && *responseOut){
    logline("SessionSetup","response dict:");
    if (env_on("MIB3_SHIM_CFSHOW",0) && p_CFShow) p_CFShow(*responseOut);
  }

  /* Handle any type-111 (cluster) stream the unit's dispatcher dropped:
   * the real handler above ran case 110 for the main screen and dropped 111.
   * NB: the unit's own dispatcher uses TYPE-CHECKED lookups (CFDictionaryGetTyped-
   * Value / CFArrayGetTypedValueAtIndex); we must too, or a malformed `streams`
   * (a non-array) or a non-dict entry would be iterated as if it were one.
   * RUNTIME GATE: this is the reliable enable point (session setup is well after
   * the stick mount); absent the marker we forward and do nothing. */
  if (env_on("MIB3_SHIM_ADD",1) && gate_active() && session && request && responseOut && *responseOut &&
      p_CFDictionaryGetValue && p_CFArrayGetCount && p_CFArrayGetValueAtIndex){
    CFTypeRef rv = dgetk((CFDictionaryRef)request, "streams");
    int isarr = 0;
    if (rv){
      isarr = 1;
      if (p_CFGetTypeID && p_CFArrayGetTypeID) isarr = (p_CFGetTypeID(rv) == p_CFArrayGetTypeID());
    }
    if (rv && isarr){
      CFArrayRef reqStreams = (CFArrayRef)rv;
      CFIndex n = p_CFArrayGetCount(reqStreams), i;
      logkv("SessionSetup","request streams",(u64)n,0);
      for (i = 0; i < n; i++){
        void *sd = (void*)p_CFArrayGetValueAtIndex(reqStreams, i);
        if (!sd) continue;
        if (p_CFGetTypeID && p_CFDictionaryGetTypeID && p_CFGetTypeID(sd) != p_CFDictionaryGetTypeID()) continue;
        s64 t = dgeti64k((CFDictionaryRef)sd, "type", 0);
        if ((int)t == MIB3_CLUSTER_TYPE){
          logline("SessionSetup","type-111 stream -> cluster_setup");
          if (cluster_setup(session, sd, *responseOut) == 0) handled111 = 1;
        }
      }
    }
  }

  /* Inject enabledFeatures = [...existing, viewAreas, altScreen] -- but ONLY
   * into the CONTROL SETUP response.
   *
   * THE SCOPE FIX (2026-10-16 redteam, rgi-faithful): the phone proposes in
   * the control-SETUP REQUEST under "features" (media/stream SETUPs carry
   * "streams" only -- PlayPort handleSetup splits them exactly this way),
   * and reads the negotiated set from the control-SETUP RESPONSE under
   * "enabledFeatures" (iOS _apsession_ensureControlStreamSetup, verified in
   * the rgi's iOS 26.6 AirPlaySender decompile).  The previous build injected
   * enabledFeatures into EVERY SETUP response -- including the 111 stream
   * SETUP itself, which NO reference does (PlayPort's stream-SETUP response
   * carries ONLY the "streams" key).
   *
   * CONTENT FIX (rgi warning, altscreen_hook.c:1975-1979): "each string in
   * that array flips a sibling endpoint flag (viewAreas+0x3D, altScreen+0x3E,
   * uiContext+0x3F, enhancedSiri+0x39), so NEVER echo the phone's proposal".
   * Seed from the receiver's EXISTING response enabledFeatures (usually
   * absent on this unit) and add ONLY the two contracts we implement:
   * viewAreas and altScreen.  "iAPChannel" (the old default, taken from
   * PlayPort's full-receiver set) is now OPT-IN (MIB3_SHIM_EF_IAPCHANNEL=1):
   * the stock MIB3 has never negotiated it, and the rgi -- the only
   * stock-HU-proven reference -- does not claim it.
   *
   * Coupling unchanged (2026-10-15): only when the cluster was actually
   * advertised (g_cluster_advertised) -- altScreen flag <=> >=2 displays.
   * MIB3_SHIM_ENABLEDFEAT=0 ablates the whole injection at runtime. */
  if (env_on("MIB3_SHIM_ENABLEDFEAT",1) && gate_active() && g_cluster_advertised &&
      responseOut && *responseOut && p_CFDictionarySetValue &&
      p_CFStringCreateWithCString && p_kCFTypeArrayCallBacks &&
      p_CFDictionaryGetValue && session && request) {
    /* control SETUP iff the request carries the "features" proposal key */
    void *kfeat = dgetk((CFDictionaryRef)request, "features");
    int is_control_setup = (kfeat != 0);
    if (is_control_setup){
      /* log the phone's proposal (rgi alt_log_phone_features): the exact set
       * this phone would negotiate -- the diagnostic the old builds never had */
      if (p_CFArrayGetCount && p_CFArrayGetValueAtIndex && p_CFGetTypeID &&
          p_CFArrayGetTypeID && p_CFStringGetTypeID && p_CFStringGetCString &&
          kfeat && p_CFGetTypeID(kfeat) == p_CFArrayGetTypeID()){
        CFIndex j, m = p_CFArrayGetCount((CFArrayRef)kfeat);
        char list[320]; unsigned long lw = 0;
        for (j = 0; j < m && j < 24; j++){
          void *e = (void*)p_CFArrayGetValueAtIndex((CFArrayRef)kfeat, j);
          char tmp[48]; tmp[0] = 0;
          if (e && p_CFGetTypeID(e) == p_CFStringGetTypeID() &&
              p_CFStringGetCString((CFStringRef)e, tmp, sizeof(tmp), K128)){
            const char *sp = " "; const char *pp = sp;
            unsigned long tl = 0; while (tmp[tl]) tl++;
            unsigned long pl = 0; while (pp[pl]) pl++;
            unsigned long k;
            for (k = 0; k < pl && lw + k + tl + 2 < sizeof(list); k++) list[lw+k] = pp[k];
            lw += pl;
            for (k = 0; k < tl && lw + k + 1 < sizeof(list); k++) list[lw+k] = tmp[k];
            lw += tl;
            list[lw] = 0;
          }
        }
        logkv("SessionSetup","phone proposes n",(u64)(u32)m,0);
        logline("SessionSetup/phone-proposes", list);
      }
      CFStringRef k = p_CFStringCreateWithCString(0,"enabledFeatures",K128);
      if (k && (!p_CFDictionaryGetValue(*responseOut,k))) {
        /* build a mutable array seeded with any existing accepted strings
         * (usually none on this unit: stock writes no response enabledFeatures),
         * then add ONLY the two contracts this shim implements */
        fn_CFArrayCreateMutable mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
        CFMutableArrayRef arr = mka ? mka(0,0,p_kCFTypeArrayCallBacks) : 0;
        const char *feats[4]; int nf = 0;
        if (env_on("MIB3_SHIM_EF_IAPCHANNEL",0)) feats[nf++] = "iAPChannel";
        feats[nf++] = "viewAreas";
        feats[nf++] = "altScreen";
        if (env_on("MIB3_SHIM_EF_UICONTEXT",0)) feats[nf++] = "uiContext";
        int i;
        if (arr){
          CFTypeRef cur = p_CFDictionaryGetValue(*responseOut, k);
          (void)cur;   /* absent by the check above; kept for clarity */
          for (i=0;i<nf;i++){
            CFStringRef f = p_CFStringCreateWithCString(0,feats[i],K128);
            if (f){ p_CFArrayAppendValue(arr, f); if (p_CFRelease) p_CFRelease(f); } /* array retains */
          }
          p_CFDictionarySetValue(*responseOut, k, arr);
          if (p_CFRelease) p_CFRelease(arr);      /* dict retained it */
          logline("SessionSetup","injected enabledFeatures (control SETUP only)");
          logkv("SessionSetup","enabledFeatures n",(u64)nf,0);
          if (env_on("MIB3_SHIM_CFSHOW",0) && p_CFShow) p_CFShow(*responseOut);
        } else {
          logline("SessionSetup","enabledFeatures array alloc failed");
        }
        if (p_CFRelease) p_CFRelease(k);        /* dict retained it */
      } else {
        logline("SessionSetup","enabledFeatures already present");
        if (p_CFRelease && k) p_CFRelease(k);   /* not stored -> release our +1 */
      }
    } else {
      logline("SessionSetup","stream/media SETUP -- enabledFeatures NOT injected (scoped)");
    }
  } else if (env_on("MIB3_SHIM_ENABLEDFEAT",1) && gate_active() && !g_cluster_advertised) {
    /* the cluster was NOT advertised this process: claim nothing (consistency). */
    logline("SessionSetup","altScreen not claimed (cluster not advertised)");
  }
  if (handled111 && ret != 0){ logkv("SessionSetup","forcing ret 0 (real was)",(u64)(u32)ret,0); ret = 0; }
  if (env_on("MIB3_SHIM_DUMP",1) && responseOut && *responseOut){
    logline("dump/setup","-------- SETUP response we hand the phone --------");
    dump_cf("dump/setup", (CFTypeRef)*responseOut, 0);
  }
  return ret;      /* the seam ABI returns int (setup status); propagate stock's result */
}

/* ==========================================================================
 * The /info seam.
 *
 * Interposes AirPlayCopyServerInfo -- the function that BUILDS the /info dict
 * the phone reads (displays, extendedFeatures, audioFormats, ...).  The unit's
 * own /info HTTP handler (libAirPlay 0xcc98) calls it via the PLT stub at
 * 0x9640 and serialises the returned dict with CFBinaryPlistV0Create.
 *
 * Why here and not the display-array seam: `displayPanels` is a DIFFERENT
 * TOP-LEVEL key of the /info dictionary.  AirPlayInfoArrayAddScreenDisplay only
 * reaches the `displays` array, so it can never add a sibling top-level key.
 * This seam hands us the whole mutable /info dict, so it is the only place the
 * modern schema can be completed.
 *
 * MIB3_SHIM_INFO=0 ablates it at runtime (target_env.py).  When the gate is off
 * or the cluster was never advertised, the returned dict is byte-identical to
 * stock (info_complete publishes nothing without a type-111 entry).
 * ========================================================================*/
void *AirPlayCopyServerInfo(void *a0, void *a1, void *a2)
{
  hit("INFO\n", 5);                       /* prove entry, independent of everything */
  resolve();
  /* count every /info the phone asks for: the documented flow has a SECOND /info
   * (the follow-up GetInfo carrying altScreenURLs) before the 111 stream. */
  { static int info_calls = 0; info_calls++; logkv("CopyServerInfo","CALL #",(u64)info_calls,0); }
  void *info = 0;
  if (real_CopyServerInfo)
    info = real_CopyServerInfo(a0, a1, a2);
  else
    logline("CopyServerInfo","no real fn!");
  if (info && env_on("MIB3_SHIM_INFO",1) && gate_active()){
    int done = info_complete((CFMutableDictionaryRef)info);
    g_info_completed = done;
  }
  /* MIB3_SHIM_FEATURES=<dec> overrides the top-level /info `features` capability
   * mask (the stock LG value differs from the working receivers' 0x615653aee2). */
  { unsigned long long f = (unsigned long long)env_num("MIB3_SHIM_FEATURES", 0);
    if (f && info && p_CFDictionarySetInt64){
      CFStringRef k = sstr("features");
      if (k){ p_CFDictionarySetInt64((CFMutableDictionaryRef)info, k, (s64)f);
              if (p_CFRelease) p_CFRelease(k);
              logkv("info","features override",f,0); }
    } }
  if (env_on("MIB3_SHIM_DUMP",1) && info){
    logline("dump/info","========== /info dict we hand the phone ==========");
    dump_cf("dump/info", (CFTypeRef)info, 0);
  }
  return info;
}

/* exported for the harness: drive the /info completion on a synthesised dict. */
int  mib3shim_complete_info(void *info){ resolve(); return info_complete((CFMutableDictionaryRef)info); }
int  mib3shim_info_completed(void){ return g_info_completed; }

/* ==========================================================================
 * HALF-2 diagnostic: observe the CarPlay OUTPUT surface (the delivery input).
 *
 * The carplay binary calls CarPlayAppFrameworkSetSurface(fw, surface, id) to
 * select which surface the framework paints CarPlay into (id = the rendering
 * surface id -- see LANE_CLUSTER_DELIVERY_2026-10-09.md).  Interposing it logs,
 * at runtime, which surface CarPlay targets -- the missing fact the delivery
 * decision needs.  The LOG is opt-in (MIB3_SHIM_FRAMEDBG=1); the interposition
 * itself is always active (LD_PRELOAD), so the ABI is matched exactly:
 *   int CarPlayAppFrameworkSetSurface(CarPlayAppFrameworkPrivate*, void*, u32)
 * When logging is off this is a pure forwarder (behaviour identical to stock).
 * ========================================================================*/
int CarPlayAppFrameworkSetSurface(void *fw, void *surface, unsigned id)
    __asm__("_Z29CarPlayAppFrameworkSetSurfaceP26CarPlayAppFrameworkPrivatePvj");
int CarPlayAppFrameworkSetSurface(void *fw, void *surface, unsigned id){
  resolve();
  if (env_on("MIB3_SHIM_FRAMEDBG",0)){
    logline("SetSurface","CALL");
    logkv("SetSurface","fw",(u64)fw,1);
    logkv("SetSurface","surface",(u64)surface,1);
    logkv("SetSurface","surfaceId",(u64)id,0);
  }
  if (p_realSetSurface) return p_realSetSurface(fw, surface, id);
  return -1;
}

/* ==========================================================================
 * FRAME INTERCEPTION (route B) -- the cluster stream's frame SOURCE.
 *
 * THE PROBLEM.  The CarPlay media engine is a one-window singleton
 * (CinemoMediaEngineUtils::getInstance()): every screen stream's decoded frames
 * funnel through the SAME `writeVideo` into the SAME projection/surface
 * (LANE_SURFACE_BINDING_DESK_2026-10-09.md).  So the shim's own cluster surface
 * (surface_create/surface_write_frame) has no frame source, and the singleton
 * cannot tell the streams apart.
 *
 * THE INTERCEPTION POINT.  The only stage that carries BOTH the frame payload
 * AND a per-stream identity is the framework's ScreenStream entry:
 *
 *   libAirPlay.so  AirPlayReceiverSessionScreen_ProcessFrames  (0x11fbf0)
 *     decrypt (AES-CTR / chacha) the received packet
 *     -> ScreenStreamProcessData(screen->stream [screen+0x328], buf, len, ts,0,0,0)
 *          [libCarPlayMediaFramework.so, FUNC GLOBAL DEFAULT @ 0x1e000]
 *        -> CarPlayScreen::getInstanceInScreenStream(stream) -> per-stream CarPlayScreen
 *        -> CarPlayScreen::processData(...)   [parse NAL units]
 *        -> CinemoMediaEngineUtils::writeVideo(SINGLETON, buf, len, ts)   <-- shared, no id
 *
 * `ScreenStreamProcessData`'s FIRST ARG is the per-stream ScreenStreamRef, so it
 * is the discriminator.  `CinemoMediaEngineUtils::writeVideo` is the WRONG point:
 * it is the singleton and has no stream identity (this is the "separation" crux).
 *
 * SEPARATION.  Each cluster screen thread registers ITS OWN stream's
 * ScreenStreamRef (read from its screen+0x328 right after StartSession) into a
 * small registry, and unregisters it when the stream ends.  In the interposer:
 *   - stream in the registry -> CLUSTER: divert into the shim's own sink and
 *                               do NOT forward (the singleton never renders it,
 *                               so the MAIN screen is undisturbed);
 *   - otherwise              -> MAIN (or any other): forward unchanged.
 * A single-slot global was wrong: a reconnect / second type-111 stream would
 * overwrite it and the older stream's frames would be forwarded to the main
 * surface (and one thread would clobber another's state).
 *
 * PAYLOAD FORMAT (honest limit).  At this stage the payload is the cluster
 * stream's DECRYPTED but still COMPRESSED H.264 access unit (processData parses
 * NAL units / start codes).  Decoded RGBA pixels exist only INSIDE Cinemo
 * (libNmeSDK), which the framework never exposes.  So a decode bridge
 * (H.264 -> RGBA) is required to feed the RGBA surface path on the live unit;
 * the desk bench feeds a synthetic buffer through this same seam.  See
 * LANE_FRAME_INTERCEPT_2026-10-09.md.
 * ========================================================================*/
static int cluster_on_frame(const void *data, unsigned long len, unsigned long ts){
  unsigned long n = ++g_cluster_frames;
  /* rate-limited: the first frame + every 300th (a 60fps log would flood tmpfs). */
  if (n == 1 || (n % 300) == 0){ logkv("frame/cluster","n",n,0); logkv("frame/cluster","len",len,0); }
  /* Route through the ONE sink dispatcher (off/dry/helper/local).  The helper
   * (out-of-process) mode touches no display here; the local mode is ACK-gated. */
  return sink_feed(data, len, ts);
}

int ScreenStreamProcessData(void *stream, const void *data, unsigned long len,
                            unsigned long ts, void *dict, void *completion, void *ctx)
{
  resolve();
  if (cl_stream_has(stream)){
    /* CLUSTER stream: divert to the shim's sink; do not forward (separation). */
    if (env_on("MIB3_SHIM_FRAMEDBG",0)) logline("SSPD","cluster stream -> shim surface (diverted)");
    cluster_on_frame(data, len, ts);
    if (completion) ((void(*)(void*))completion)(ctx);
    return 0;
  }
  /* MAIN (or any other) stream: forward unchanged -> singleton -> main surface. */
  if (env_on("MIB3_SHIM_DBG",0)) logline("SSPD","non-cluster stream -> forwarded");
  if (p_realSSPD) return p_realSSPD(stream, data, len, ts, dict, completion, ctx);
  return 0;
}

/* ---- harness hooks (frame interception) --------------------------------- */
void          mib3shim_cluster_stream_set(void *stream){
  resolve();
  { int i; for (i=0;i<MIB3_MAX_CL_STREAMS;i++) g_cl_streams[i]=0; }  /* reset */
  if (stream) cl_stream_add(stream);
}
int           mib3shim_cluster_stream_add(void *stream){ resolve(); return cl_stream_add(stream); }
unsigned long mib3shim_cluster_frames(void){ return g_cluster_frames; }
int           mib3shim_cluster_stream_count(void){
  int i,n=0; for (i=0;i<MIB3_MAX_CL_STREAMS;i++) if (g_cl_streams[i]) n++; return n;
}

/* ==========================================================================
 * DECODE BRIDGE (route B) -- the shim's OWN Cinemo pipeline.
 *
 * THE GAP THIS CLOSES.  The cluster frames arrive COMPRESSED (H.264 access
 * units).  The shim's surface path uploads RGBA, so it cannot display them.
 * The decoded pixels exist only INSIDE Cinemo (libNmeSDK).  The bridge hands the
 * compressed frames to a SECOND Cinemo projection the shim creates itself, so
 * Cinemo decodes + renders them into the shim's ivi surface.
 *
 * THE VERDICT (see LANE_DECODE_BRIDGE_2026-10-09.md).  The Cinemo SDK exposes
 * `extern "C"` FACTORY functions (CinemoCreateConfig / Playlist / Window /
 * Projection) that malloc a fresh object each call -- NO global one-shot guard
 * anywhere in the chain -- so a second pipeline is CONSTRUCTIBLE at the API
 * level.  BUT: the unit's own code only ever makes one (CinemoMediaEngineUtils
 * is a one-shot singleton), the decode/render is hardware (OMX/Adreno), and the
 * video-device id is a shared config value -- so acceptance is RUNTIME-ONLY.
 * The bridge is therefore defensive: every step is checked, any failure tears
 * down and leaves the fallback surface path in charge (no crash).
 *
 * The sequence + vtable byte-offsets are copied from the unit's own
 * CinemoMediaEngineUtils::screenSetup() (0x120ac4), which is proven on the unit:
 *   cfg   = CinemoCreateConfig()
 *   pl    = CinemoCreatePlaylist()
 *   pl->[0x128](pl,&pb)          ; create playback
 *   pb->[0x28](pb,ctx,handler)   ; set event handler
 *   cfg->[0x30](cfg,VIDEO_DEVICE_ID,&dev)   ; get the video-device descriptor
 *   dev->[0xa0](dev,"windowpos"|"windowsize"|"ivi-app-id",value)
 *   dev->[0x58](dev) -> devname
 *   win   = CinemoCreateWindow(devname)
 *   proj  = CinemoCreateProjection("?disable_scaling=1")
 *   pl->[0xd8](pl,proj,0)        ; bind projection to the playlist
 *   feed: proj->[0x30](proj,data,len,pts,flag)   ; WriteVideo (decode+render)
 * ========================================================================*/
typedef struct {
  int resolved, active;
  void *cfg, *playlist, *playback, *dev, *window, *projection;
  int w, h; unsigned ivi_id;
  unsigned long feed_count, fail_count;
  volatile int ready;   /* playback cue received (CinemoEvent type 4) */
} bridge_t;
static bridge_t g_br;
extern int usleep(unsigned);
/* Stub playback event handler.  The framework passes a real one
 * (CinemoMediaEngineUtils::handlePlaybackEvents); our bridge passed 0.  If the
 * device delivers its "decoded/please render" notification to that handler and
 * nobody is listening, it has nowhere to go -- which would explain a fully set-up
 * EGL context (trace: eglCreateWindowSurface + eglMakeCurrent) and ZERO swaps. */
static unsigned long g_br_events;
/* A2: the Painter swap-chain calls a CLIENT callback with each frame.  The device
 * reads `painter-callback` / `painter-cookie` from its own name via
 * NmeString::get_url_parameter_ptr (a real pointer, carried as text).  We pass
 * our own function so the decoded frames come to us instead of nowhere. */
static volatile unsigned long g_painter_calls;
static long painter_cb(void *a, void *b, void *c, void *d, void *e, void *f){
  (void)b;(void)c;(void)d;(void)e;(void)f;
  g_painter_calls++;
  if (g_painter_calls <= 4){
    logline("bridge","PAINTER CB");
    logkv("bridge","pcb arg0",(u64)(unsigned long)a,1);
    logkv("bridge","pcb arg1",(u64)(unsigned long)b,1);
    logkv("bridge","pcb arg2",(u64)(unsigned long)c,1);
  }
  return 0;
}
static void fmt_hexp(char *out, unsigned long v){
  const char *h = "0123456789abcdef"; char t[18]; int k=0, i=0;
  out[i++]='0'; out[i++]='x';
  if (!v){ out[i++]='0'; out[i]=0; return; }
  while (v){ t[k++] = h[v & 0xf]; v >>= 4; }
  while (k) out[i++] = t[--k];
  out[i]=0;
}static long bridge_event_handler(void *self, void *ev){
  (void)self;
  /* The framework's handler (CinemoMediaEngineUtils::handlePlaybackEvents) reads
   * the event's first int and, on type 4 (CINEMO_EC_CUE), marks the playback
   * ready -- waitForReady() blocks on exactly that before any video is written.
   * Our old handler ignored it, so the device's "ready/please render" cue went
   * nowhere.  Read the type, log it (rate-limited), and set ready on the cue. */
  { int t = ev ? *(int*)ev : -1;
    g_br_events++;
    if (g_br_events <= 8 || (g_br_events % 100) == 0) logkv("bridge","event type",(u64)(unsigned)t,0);
    if (t == 4 && !g_br.ready){ g_br.ready = 1; logline("bridge","READY cue (event type 4)"); } }
  return 0;
}

static void *vslot(void *obj, unsigned off){
  if (!obj) return 0;
  { void *vt = *(void**)obj; if (!vt) return 0; return *(void**)((char*)vt + off); }
}
static void fmt_u(char *out, unsigned v){
  char t[16]; int k=0; if(!v) t[k++]='0';
  while (v){ t[k++]=(char)('0'+v%10); v/=10; }
  { int j=0; while (k) out[j++]=t[--k]; out[j]=0; }
}
static void fmt_2int(char *out, int a, int b){
  /* 2026-10-16 ROOT CAUSE FIX: the separator MUST be 'x', not ','.  libNmeVideoDevice parses
   * windowpos/windowsize with sscanf("%dx%d") (corpus libNmeVideoDevice/decompiled.c:15358,21579)
   * and rejects anything else ("invalid resolution").  With a comma the device never got a drawable
   * size, so the ivi surface was created but never allocated a buffer (ILM scene: original size 0x0),
   * ivi_share had nothing to hand multi-screen-server, MS-S never received `configure`, and the
   * encoder never emitted video.  The working framework passes the literal "x" (string table byte
   * immediately before "windowpos" in libCarPlayMediaFramework). */
  int n=0; fmt_u(out,(unsigned)a); while(out[n]) n++; out[n++]='x'; fmt_u(out+n,(unsigned)b);
}
/* A REAL (non-null) log callback.  NmeLogSetTarget(target=FILE,path,cb,user)
 * with cb==0 segfaulted when the Cinemo logging was first enabled on the desk
 * bench (2026-10-17); pass a valid sink so the engine can call it safely and we
 * can see the graph/swap-chain decisions it otherwise keeps to itself. */
static unsigned long g_nme_logs = 0;
static void bridge_log_sink(void *user, const void *item){
  (void)user; (void)item;      /* item is a CinemoLogItem&; layout unknown -- do not deref */
  g_nme_logs++;
}
static void resolve_bridge(void){
  if (g_br.resolved) return;
  g_br.resolved = 1;
  p_CinemoCreateConfig     = (fn_CinemoCreate)    R("CinemoCreateConfig");
  p_CinemoCreatePlaylist   = (fn_CinemoCreate)    R("CinemoCreatePlaylist");
  p_CinemoCreateWindow     = (fn_CinemoCreateStr) R("CinemoCreateWindow");
  p_CinemoCreateProjection = (fn_CinemoCreateStr) R("CinemoCreateProjection");
  p_opt_video_device_id    = R("CINEMO_OPTION_VIDEO_DEVICE_ID");
  p_iid_cinemo_option      = R("_ZN13ICinemoOption3iidE");
  p_opt_aspect             = R("CINEMO_OPTION_VIDEO_ASPECT");
  p_opt_max_res            = R("CINEMO_OPTION_VIDEO_MAX_RESOLUTION");
  p_opt_frame_drop         = R("CINEMO_OPTION_VIDEO_FRAME_DROP_THRESHOLD");
  p_opt_max_fps            = R("CINEMO_OPTION_VIDEO_MAX_RENDERED_FPS");
  p_opt_dist_en            = R("CINEMO_OPTION_DISTRIBUTED_ENABLE");
  p_opt_dist_delay         = R("CINEMO_OPTION_DISTRIBUTED_DELAY_MS");
  p_opt_hw_decode          = R("CINEMO_OPTION_VIDEO_HARDWARE_DECODE");
  logline("bridge","resolved");
  logkv("bridge","CinemoCreateConfig",(u64)p_CinemoCreateConfig,1);
  logkv("bridge","CinemoCreatePlaylist",(u64)p_CinemoCreatePlaylist,1);
  logkv("bridge","CinemoCreateWindow",(u64)p_CinemoCreateWindow,1);
  logkv("bridge","CinemoCreateProjection",(u64)p_CinemoCreateProjection,1);
  logkv("bridge","CINEMO_OPTION_VIDEO_DEVICE_ID",(u64)p_opt_video_device_id,1);
  /* Cinemo log on (device/swap-chain decisions are otherwise silent).  FILE target
   * (1) + Debug level (1: Verbose=-1, Trace=0, Debug=1).  MIB3_SHIM_BRIDGE_LOG=0 off. */
  p_NmeLogSetLevel  = (int (*)(int)) R("_Z14NmeLogSetLevel15CINEMO_LOGLEVEL");
  p_NmeLogSetTarget = (int (*)(int,const char*,void*,void*)) R("_Z15NmeLogSetTarget16CINEMO_LOGTARGETPKcPFvPvRK13CinemoLogItemES2_");
  if (env_on("MIB3_SHIM_BRIDGE_LOG",0) && p_NmeLogSetLevel){
    const char *lg = getenv("MIB3_SHIM_BRIDGE_LOGPATH");
    if (!lg || !lg[0]) lg = "/tmp/nme_bridge.log";
    if (p_NmeLogSetTarget){ int rt = p_NmeLogSetTarget(1, lg, (void*)&bridge_log_sink, 0);
      logkv("bridge","NmeLogSetTarget rc",(u64)(unsigned)rt,0); }
    { int rl = p_NmeLogSetLevel(1);
      logkv("bridge","NmeLogSetLevel rc",(u64)(unsigned)rl,0); }
    logline("bridge","cinemo log enabled"); }
}
static void bridge_release(void *obj){
  if (!obj) return;
  { fn_vc0 rel = (fn_vc0)vslot(obj, 0x8); if (rel) rel(obj); }
}
static void bridge_destroy(void){
  if (g_br.projection){ bridge_release(g_br.projection); g_br.projection = 0; }
  if (g_br.window){ bridge_release(g_br.window); g_br.window = 0; }
  if (g_br.playback){ bridge_release(g_br.playback); g_br.playback = 0; }
  if (g_br.dev){ bridge_release(g_br.dev); g_br.dev = 0; }
  if (g_br.playlist){ bridge_release(g_br.playlist); g_br.playlist = 0; }
  if (g_br.cfg){ bridge_release(g_br.cfg); g_br.cfg = 0; }
  if (g_br.active) logline("bridge","destroyed");
  g_br.active = 0;
}
static int bridge_active(void){ return g_br.active; }

static int bridge_create(int w, int h, unsigned ivi_id){
  resolve_bridge();
  if (g_br.active){ logline("bridge","already active"); return 0; }
  if (!p_CinemoCreateConfig || !p_CinemoCreatePlaylist || !p_CinemoCreateWindow ||
      !p_CinemoCreateProjection || !p_opt_video_device_id){
    logline("bridge","Cinemo SDK symbols unavailable -> bridge inert (fallback)");
    return -1;
  }
  g_br.cfg = g_br.playlist = g_br.playback = g_br.dev = g_br.window = g_br.projection = 0;
  g_br.w = w; g_br.h = h; g_br.ivi_id = ivi_id;
  logline("bridge","-- create begin --");

  if (p_CinemoCreateConfig(&g_br.cfg) != 0 || !g_br.cfg){ logline("bridge","FAIL: CinemoCreateConfig"); return -1; }
  /* CONFIG INIT -- this is the step that was missing (found live 2026-10-16).
   * CinemoMediaEngineUtils::init() (corpus libCarPlayMediaFramework.so
   * decompiled.c 15269-15336) runs, on the config, +0x90 (GetVersion) then
   * +0xb0 (SetPluginDirectory) BEFORE screenSetup ever calls
   * config->[0x30] GetVideoDevice.  WITHOUT the plugin directory the device
   * plugins never load, so GetVideoDevice returns non-zero -- exactly the wall
   * the out-of-process helper hit on the car.  The framework's own fallback dir
   * is /usr/lib64/cinemo; the configured dir is /lge/app_rw/lib64/cinemo.
   * MIB3_SHIM_CINEMO_DIR overrides. */
  { fn_vc1 f = (fn_vc1)vslot(g_br.cfg, 0x90);
    if (f){ unsigned char ver[32]; unsigned i; for (i=0;i<sizeof(ver);i++) ver[i]=0; f(g_br.cfg, ver); } }
  { char *pd = getenv("MIB3_SHIM_CINEMO_DIR"); int rc;
    fn_vc1 f = (fn_vc1)vslot(g_br.cfg, 0xb0);
    if (!f){ logline("bridge","FAIL: config->SetPluginDirectory missing"); goto fail; }
    if (!pd || !pd[0]) pd = "/lge/app_rw/lib64/cinemo";
    logline("bridge/cinemo-dir", pd);
    rc = f(g_br.cfg, pd);
    logkv("bridge","cfg->[0xb0] rc",(u64)(unsigned)rc,0);
    if (rc != 0){
      logline("bridge/cinemo-dir","fallback /usr/lib64/cinemo");
      rc = f(g_br.cfg, "/usr/lib64/cinemo");
      logkv("bridge","cfg->[0xb0] rc2",(u64)(unsigned)rc,0);
      if (rc != 0){ logline("bridge","FAIL: config->SetPluginDirectory"); goto fail; }
    } }
  if (p_CinemoCreatePlaylist(&g_br.playlist) != 0 || !g_br.playlist){ logline("bridge","FAIL: CinemoCreatePlaylist"); goto fail; }
  { fn_vc1 f = (fn_vc1)vslot(g_br.playlist, 0x128);
    if (!f || f(g_br.playlist, &g_br.playback) != 0 || !g_br.playback){ logline("bridge","FAIL: playlist->CreatePlayback"); goto fail; } }
  { fn_vc2 f = (fn_vc2)vslot(g_br.playback, 0x28);
    /* (playback, self, handler) -- the framework passes all three; we passed (0,0).
     * MIB3_SHIM_BRIDGE_EVH=0 ablates. */
    if (f){
      if (env_on("MIB3_SHIM_BRIDGE_EVH",1)) f(g_br.playback, 0, (void*)bridge_event_handler);
      else f(g_br.playback, 0, 0);
    } }
  /* ---- THE OPTIONS BLOCK (2026-10-17) ------------------------------------
   * The framework's CinemoMediaEngineUtils::screenSetup() sets these on the playback's
   * ICinemoOption (playback->[0x10](pb,&opt,ICinemoOption::iid), then opt->[0x28] per key)
   * BEFORE it calls GetVideoDevice.  Our bridge skipped them entirely.  Without
   * VIDEO_HARDWARE_DECODE="TRUE" the pipeline can end up on the "Dummy"/"Painter"
   * swap chain (NmeSwapChain::Create, corpus libNmeVideoDevice 0x124be0) which produces
   * NO real surface buffer -- exactly the observed "decodes, but the ivi surface stays
   * 0x0" wall.  Values are read out of the framework's own rodata pool
   * (libCarPlayMediaFramework.so, imagebase 0x100000): "Native" / "1920x1080" / "50" /
   * "60000" / "FALSE" / "0" / "TRUE".  Single change; MIB3_SHIM_BRIDGE_OPTS=0 ablates it. */
  if (env_on("MIB3_SHIM_BRIDGE_OPTS",1) && p_iid_cinemo_option){
    void *opts = 0;
    fn_vc2 getopt = (fn_vc2)vslot(g_br.playback, 0x10);
    void *iid = *(void**)p_iid_cinemo_option;
    int rc = getopt ? (int)getopt(g_br.playback, &opts, iid) : -1;
    logkv("bridge","playback->[0x10] options rc",(u64)(unsigned)rc,0);
    if (rc == 0 && opts){
      fn_vc2 setopt = (fn_vc2)vslot(opts, 0x28);
      if (setopt){
        struct { void *sym; const char *val; } o[7];
        int k, okc = 0, lastrc = 0;
        o[0].sym = p_opt_aspect;     o[0].val = "Native";
        o[1].sym = p_opt_max_res;    o[1].val = "1920x1080";
        o[2].sym = p_opt_frame_drop; o[2].val = "50";
        o[3].sym = p_opt_max_fps;    o[3].val = "60000";
        o[4].sym = p_opt_dist_en;    o[4].val = "FALSE";
        o[5].sym = p_opt_dist_delay; o[5].val = "0";
        /* 2026-10-17: the vendor framework passes "TRUE" here, and the HW
         * decoder is the fast path -- but on the DESK BENCH (and as a live
         * probe) we need the SOFTWARE codec plugin (libNmeVideoSW.so,
         * Cinemo/NmeVideo/Codec/NvdecSW): the HW plugin (libNmeVideoOMX.so)
         * never even loads without the SoC's vidc node, so no frames exist and
         * the render stage is never reached.  MIB3_SHIM_BRIDGE_HWDEC=FALSE
         * selects the SW codec.  Default stays "TRUE" (vendor parity). */
        o[6].sym = p_opt_hw_decode;
        { const char *hw = getenv("MIB3_SHIM_BRIDGE_HWDEC");
          o[6].val = (hw && hw[0]) ? hw : "TRUE"; }
        for (k = 0; k < 7; k++){
          if (!o[k].sym) continue;
          lastrc = (int)setopt(opts, *(void**)o[k].sym, (void*)o[k].val);
          if (lastrc == 0) okc++;
          /* per-option result: with 7 options the vendor sets, one of them has
           * been returning non-zero since the bridge was written and it was
           * never identified -- it may be the codec SELECTION (hw vs sw). */
          else { logkv("bridge","optfail idx",(u64)k,0);
                 logline("bridge/optfail-name", *(char**)o[k].sym); }
        }
        logkv("bridge","options set ok",(u64)okc,0);
        logkv("bridge","options lastrc",(u64)(unsigned)lastrc,0);
      } else logline("bridge","FAIL: options->[0x28] missing");
    } else if (rc != 0) logline("bridge","FAIL: playback->[0x10] options");
  }
  { fn_vc2 f = (fn_vc2)vslot(g_br.cfg, 0x30);
    /* the framework LOADS the option id from the symbol (it dereferences the GOT
     * entry: `uVar3 = *(undefined8 *)PTR_CINEMO_OPTION_VIDEO_DEVICE_ID`) -- the
     * symbol is an 8-byte OBJECT holding a pointer.  Passing the symbol's
     * ADDRESS (dlsym's return) is wrong: GetVideoDevice then gets a garbage id
     * and fails.  Dereference, exactly like the framework. */
    void *optid = p_opt_video_device_id ? *(void**)p_opt_video_device_id : 0;
    logkv("bridge","GetVideoDevice optid",(u64)optid,1);
    if (!f || f(g_br.cfg, optid, &g_br.dev) != 0 || !g_br.dev){ logline("bridge","FAIL: config->GetVideoDevice"); goto fail; } }
  { fn_vc2 f = (fn_vc2)vslot(g_br.dev, 0xa0);
    char pos[24], size[24], idbuf[16];
    fmt_2int(pos,0,0); fmt_2int(size,w,h); fmt_u(idbuf,ivi_id);
    if (!f){ logline("bridge","FAIL: dev->SetProperty missing"); goto fail; }
    if (f(g_br.dev,"windowpos",pos)  != 0){ logline("bridge","FAIL: windowpos");  goto fail; }
    if (f(g_br.dev,"windowsize",size)!= 0){ logline("bridge","FAIL: windowsize"); goto fail; }
    if (f(g_br.dev,"ivi-app-id",idbuf)!=0){ logline("bridge","FAIL: ivi-app-id"); goto fail; }
    /* The device takes the SIZE from its own `resolution` / `surface_width`/`surface_height` url
     * params (see ParseCommonParameters: "invalid resolution: %ux%u"), separately from the drawable
     * `windowsize`.  ParseCommonParameters runs at device creation and rejects bad values, so pass
     * them explicitly in the same "WxH"/plain-int form the parser expects. */
    { char res[24]; fmt_2int(res,w,h);
      if (f(g_br.dev,"resolution",res) != 0) logline("bridge","WARN: resolution rejected");
      { char sw[16]; fmt_u(sw,(unsigned)w);
        if (f(g_br.dev,"surface_width",sw) != 0) logline("bridge","WARN: surface_width rejected"); }
      { char sh[16]; fmt_u(sh,(unsigned)h);
        if (f(g_br.dev,"surface_height",sh) != 0) logline("bridge","WARN: surface_height rejected"); }
    }
    logkv("bridge","ivi_id",(u64)ivi_id,0); }
  /* A2 probe: force a client-visible swap-chain backend.  The device reads the
   * `swap-chain` URL parameter from its own name (NmeSwapChain::Create). */
  { const char *sc = getenv("MIB3_SHIM_BRIDGE_SC");
    if (sc && sc[0]){ fn_vc2 sf = (fn_vc2)vslot(g_br.dev, 0xa0);
      int r = sf ? (int)sf(g_br.dev, (void*)"swap-chain", (void*)sc) : -1;
      logkv("bridge","swap-chain set rc",(u64)(unsigned)r,0); } }
  { const char *pp = getenv("MIB3_SHIM_BRIDGE_PAINTER");
    if (pp && pp[0] && pp[0] != '0'){
      fn_vc2 sf = (fn_vc2)vslot(g_br.dev, 0xa0);
      char cb[24], ck[24];
      fmt_hexp(cb, (unsigned long)(unsigned long)&painter_cb);
      fmt_hexp(ck, (unsigned long)(unsigned long)&g_br);
      logline("bridge","painter-callback set"); logline("bridge/pcb", cb);
      if (sf){
        logkv("bridge","pcb rc sc",(u64)(unsigned)(sf(g_br.dev,(void*)"swap-chain",(void*)"painter")),0);
        logkv("bridge","pcb rc cb",(u64)(unsigned)(sf(g_br.dev,(void*)"painter-callback",(void*)cb)),0);
        logkv("bridge","pcb rc ck",(u64)(unsigned)(sf(g_br.dev,(void*)"painter-cookie",(void*)ck)),0);
      } } }
  { fn_vc0 f = (fn_vc0)vslot(g_br.dev, 0x58);
    const char *dn = f ? (const char*)f(g_br.dev) : 0;
    if (!dn){ logline("bridge","FAIL: dev->GetName"); goto fail; }
    logline("bridge/devname", dn);
    if (p_CinemoCreateWindow(dn, &g_br.window) != 0 || !g_br.window){ logline("bridge","FAIL: CinemoCreateWindow"); goto fail; }
    /* Framework parity (CinemoMediaEngineUtils::setVideoParams, corpus libCarPlayMediaFramework
     * 0x120150): the framework does NOT call window->[0x58].  It calls
     *     window->[0x60](window, buf)          -> the video.device_name
     *     playback->[0x58](playback, devname)  -> ATTACH the video device to the playback
     * The second call is the one our bridge was missing entirely: without the playback bound
     * to the device the decoder decodes (WriteVideo returns OK) but has no render target, so
     * it never attaches a buffer to the ivi surface (ILM: original size 0x0).  Single change;
     * MIB3_SHIM_BRIDGE_BIND=0 ablates it. */
    if (env_on("MIB3_SHIM_BRIDGE_BIND",1)){
      char wdn[300];
      { int i; for (i = 0; i < 300; i++) wdn[i] = 0; }
      { fn_vc1 g = (fn_vc1)vslot(g_br.window, 0x60);
        int r = g ? (int)g(g_br.window, wdn) : -1;
        logkv("bridge","window->[0x60] devname rc",(u64)(unsigned)r,0);
        if (r == 0) logline("bridge/win-devname", wdn); }
      { const char *use = wdn[0] ? wdn : dn;
        fn_vc1 h = (fn_vc1)vslot(g_br.playback, 0x58);
        int r = h ? (int)h(g_br.playback, (void*)use) : -1;
        logkv("bridge","playback->[0x58] bind-device rc",(u64)(unsigned)r,0); }
    }
    /* Legacy call: bind the window to the device by NAME.  It has always
     * returned rc=0, but the vendor framework NEVER makes this call --
     * setVideoParams only does window->[0x60] + playback->[0x58].  If slot
     * [0x58] on the WINDOW is not "set device name" (e.g. it is a
     * Show/Hide/SetOutput), this call could be what stops the device from ever
     * painting.
     * DEFAULT = ON: this is the configuration that reached `bridge_create rc=0`
     * on the car (2026-10-17), so the shipped behaviour is unchanged.
     * MIB3_SHIM_BRIDGE_LEGACYBIND=0 skips it -- the vendor framework never makes
     * this call, and the desk bench shows no difference either way, so it is the
     * first thing to ablate if the device still refuses to paint. */
    if (env_on("MIB3_SHIM_BRIDGE_LEGACYBIND",1)){
      fn_vc1 g = (fn_vc1)vslot(g_br.window, 0x58);
      int brc = g ? g(g_br.window, (void*)dn) : -1;
      logkv("bridge","window->[0x58] bind-device rc",(u64)(unsigned)brc,0); } }
  if (p_CinemoCreateProjection("?disable_scaling=1", &g_br.projection) != 0 || !g_br.projection){
    logline("bridge","FAIL: CinemoCreateProjection"); goto fail; }
  { fn_vc2 f = (fn_vc2)vslot(g_br.playlist, 0xd8);
    if (!f || f(g_br.playlist, g_br.projection, 0) != 0){ logline("bridge","FAIL: playlist->SetProjection"); goto fail; } }
  /* Framework parity (CinemoMediaEngineUtils::writeVideoFormat): tell the decoder the output format
   * BEFORE feeding.  0xa0-byte descriptor; fields per corpus decompiled.c:14590. */
  { fn_vc1 f = (fn_vc1)vslot(g_br.projection, 0x28);
    unsigned char d[0xa0]; unsigned i; int rc;
    for (i=0;i<(unsigned)sizeof(d);i++) d[i]=0;
    *(unsigned*)(d+0x00)=1u; *(unsigned*)(d+0x04)=0x2002u; *(unsigned*)(d+0x08)=1u;
    *(unsigned*)(d+0x0c)=(unsigned)w; *(unsigned*)(d+0x10)=(unsigned)h;
    d[0x2c]=0x10; d[0x39]=1;
    rc = f ? f(g_br.projection, (void*)d) : -1;
    logkv("bridge","writeVideoFormat rc",(u64)(unsigned)rc,0); }
  /* NB (2026-10-16): a `projection->[0x28](proj, &writeVideoFormatDesc)` call was added here,
   * reconstructed from CinemoMediaEngineUtils::writeVideoFormat — the descriptor field offsets were
   * derived by hand and it went onto live hardware in a batch with other changes; the next plug
   * rebooted the unit.  Removed.  Re-add ONLY as a single change, desk-verified, with stick logs. */

  /* ---- PLAY (2026-10-17): THE MISSING START ------------------------------
   * CinemoMediaEngineUtils::play() (corpus libCarPlayMediaFramework 0x121e94) ends with
   *     uVar8 = playlist->[0x90](playlist, 1);
   *     playback->[0x98](playback, uVar8);
   * Our bridge built the whole pipeline (config, playlist, playback, device, window,
   * projection, bind) and then NEVER CALLED PLAY.  Evidence from the live tracer: the
   * device set up EGL (window surface + context + MakeCurrent) and then did NOTHING --
   * it never opened a video decoder node and never made a single GL call, so the surface
   * stayed empty.  The pipeline was constructed but never started.
   * MIB3_SHIM_BRIDGE_PLAY=0 ablates this single change. */
  if (env_on("MIB3_SHIM_BRIDGE_PLAY",1)){
    unsigned long id = 0;
    { fn_vc1 g = (fn_vc1)vslot(g_br.playlist, 0x90);
      id = g ? (unsigned long)g(g_br.playlist, (void*)1UL) : 0;
      logkv("bridge","playlist->[0x90] id",(u64)id,0); }
    { fn_vc1 p = (fn_vc1)vslot(g_br.playback, 0x98);
      int rc = p ? (int)p(g_br.playback, (void*)id) : -1;
      logkv("bridge","playback->[0x98] PLAY rc",(u64)(unsigned)rc,0); }
  }

  g_br.active = 1;
  /* Wait (bounded) for the playback READY cue before any frame is written.  The
   * vendor framework does exactly this (waitForReady, gated on event type 4);
   * feeding before ready is the leading candidate for "decodes but never
   * renders".  Feeding still proceeds after the timeout so decode is not lost. */
  { int t; for (t = 0; t < 3000 && !g_br.ready; t += 20) usleep(20000); }
  logkv("bridge","ready",(u64)(unsigned)g_br.ready,0);
  logline("bridge","-- create done (active) --");
  return 0;
fail:
  bridge_destroy();
  return -1;
}

static int bridge_feed(const void *data, unsigned long len, unsigned long ts){
  if (!g_br.active || !g_br.projection) return -1;
  if (!data || len == 0){ g_br.fail_count++; return -1; }   /* malformed guard */
  /* upper bound: an access unit off the wire is small; a pathological len would
   * make Cinemo's WriteVideo read past the buffer.  Reject absurd values. */
  if (len > 0x1000000UL /*16 MiB*/){ g_br.fail_count++; return -1; }
  { fn_vc4 f = (fn_vc4)vslot(g_br.projection, 0x30);
    unsigned long pts;
    if (!f){ g_br.fail_count++; return -1; }
    /* pts70mhz, exactly as CinemoMediaEngineUtils::writeVideo computes it */
    pts = (ts % 1000000000UL) * 0x1b9 / 0x186a + (ts / 1000000000UL) * 0x434a900UL;
    if (f(g_br.projection, (void*)data, (void*)len, (void*)pts, (void*)(u64)(ts != 0)) != 0){
      g_br.fail_count++;
      if ((g_br.fail_count % 100ul) == 1ul) logkv("bridge","feed FAIL count",(u64)g_br.fail_count,0);
      return -1;
    }
    g_br.feed_count++;
    if (g_br.feed_count == 1ul) logline("bridge","feed: first WriteVideo OK (decoder took the AU)");
    else if ((g_br.feed_count % 100ul) == 0ul) logkv("bridge","feed ok count",(u64)g_br.feed_count,0);
  }
  return 0;
}

/* ---- harness hooks (decode bridge) -------------------------------------- */
int  mib3shim_bridge_create(int w, int h, unsigned ivi_id){ return bridge_create(w,h,ivi_id); }
int  mib3shim_bridge_feed(const void *data, unsigned long len, unsigned long ts){ return bridge_feed(data,len,ts); }
int  mib3shim_bridge_active(void){ return g_br.active; }
void mib3shim_bridge_destroy(void){ bridge_destroy(); }
unsigned long mib3shim_bridge_feeds(void){ return g_br.feed_count; }
unsigned long mib3shim_bridge_fails(void){ return g_br.fail_count; }

/* ==========================================================================
 * THE SINK DISPATCHER (2026-10-16) -- the ONE place cluster frames may reach a
 * display, and the reason the 2026-10-16 black-screen/reset cannot repeat.
 *
 * BACKGROUND.  On 2026-10-16 the sink was enabled live (MIB3_SHIM_SURFACE=1 +
 * MIB3_SHIM_BRIDGE=1) and the centre screen went black as CarPlay started and
 * the unit reset (LANE_CLUSTER_LIVE_2026-10-16.md S5b).  Two things were wrong:
 *   (1) the sink was CREATED INSIDE the CarPlay SETUP seam, so a
 *       blocking/rupturing Cinemo or EGL call there stalls session setup -- and
 *       carplay.service is FailureAction=reboot-immediate; and
 *   (2) BOTH display paths ran IN-PROCESS, sharing the process's
 *       Adreno/OMX/ILM/EGL state with CarPlay's own rendering.  carplay links
 *       libNmeSDK.so directly, so the bridge's Cinemo factories resolve and a
 *       real second Adreno/OMX/ILM window is made -- the same class of failure
 *       the shim's own note records for the stock alt-screen path: "black main
 *       screen + decoder crash".
 *
 * WHAT CHANGED.
 *   * Default mode is OFF -- no display-touching call at all.
 *   * The sink is created on the RECEIVER THREAD, never in the SETUP seam.
 *   * A DRY-RUN mode logs the exact planned sequence and creates NOTHING.
 *   * The SAFE mode forwards frames OUT OF PROCESS (helper): carplay makes no
 *       display call, so a helper fault cannot touch the centre screen or the
 *       CarPlay process.  See mib3clusterhelper.c.
 *   * The legacy IN-PROCESS mode is retained but must be EXPLICITLY armed with
 *       MIB3_SHIM_LOCAL_ACK=1, and its two halves (bridge/surface) are separately
 *       selectable so each can be tested ALONE.
 *
 * MODES (MIB3_SHIM_SINK): off (default) | dry | helper | local.
 * Back-compat: MIB3_SHIM_SINK unset + legacy SURFACE/BRIDGE set -> `local` ONLY
 * when MIB3_SHIM_LOCAL_ACK=1, else OFF with a loud warning -- the guard that
 * makes the 2026-10-16 accident impossible to re-arm by a stale knob.
 *
 * LOCAL SELECTION (MIB3_SHIM_LOCAL): `bridge` (default) | `surface` | `both`.
 * Bridge->surface fallback happens ONLY with MIB3_SHIM_LOCAL_FALLBACK=1 (else
 * the halves would not be independently testable).
 * ========================================================================*/
static void sink_resolve(void){
  char *v;
  if (g_sink >= 0) return;
  g_sink = SINK_OFF;
  v = getenv("MIB3_SHIM_LOCAL");
  if (v && v[0]=='s') g_sink_local = 1;
  else if (v && v[0]=='b' && v[1]=='o') g_sink_local = 2;
  else g_sink_local = 0;
  g_sink_lf = env_on("MIB3_SHIM_LOCAL_FALLBACK",0);

  v = getenv("MIB3_SHIM_SINK");
  if (v && v[0]){
    if      (v[0]=='d') g_sink = SINK_DRY;
    else if (v[0]=='h') g_sink = SINK_HELPER;
    else if (v[0]=='l') g_sink = SINK_LOCAL;
    else                g_sink = SINK_OFF;
  } else if (env_on("MIB3_SHIM_SURFACE",0) || env_on("MIB3_SHIM_BRIDGE",0)){
    if (env_on("MIB3_SHIM_LOCAL_ACK",0)) g_sink = SINK_LOCAL;
    else {
      g_sink = SINK_OFF;
      logline("sink","LEGACY SURFACE/BRIDGE KNOBS SET BUT NOT ARMED: needs MIB3_SHIM_LOCAL_ACK=1 (the 2026-10-16 black-screen guard). Staying OFF.");
    }
  }
  if (g_sink == SINK_LOCAL && !env_on("MIB3_SHIM_LOCAL_ACK",0)){
    logline("sink","MIB3_SHIM_SINK=local REFUSED: set MIB3_SHIM_LOCAL_ACK=1 to accept the in-process black-screen risk (or use =helper).");
    g_sink = SINK_OFF;
  }
  { char m[110]; buf b={m,sizeof(m)-2,0};
    bs(&b,"mode="); bu64(&b,(u64)(u32)g_sink,10,0);
    bs(&b," local="); bu64(&b,(u64)(u32)g_sink_local,10,0);
    bs(&b," fallback="); bu64(&b,(u64)(u32)g_sink_lf,10,0);
    m[b.n]=0; logline("sink/resolve", m); }
}

static int sink_local_up = 0;    /* the local sink was successfully created */

static int sink_start(int w, int h, unsigned ivi_id){
  sink_resolve();
  bc("sink:start");
  if (g_sink == SINK_OFF){ logline("sink","off: no display-touching call"); bc("sink:off"); return 0; }
  if (g_sink == SINK_DRY){
    char m[170]; buf b={m,sizeof(m)-2,0};
    bs(&b,"DRY-RUN plan: local="); bu64(&b,(u64)(u32)g_sink_local,10,0);
    bs(&b," w="); bu64(&b,(u64)(u32)w,10,0); bs(&b," h="); bu64(&b,(u64)(u32)h,10,0);
    bs(&b," ivi="); bhex(&b,(u64)ivi_id); bs(&b," -- CREATING NOTHING");
    m[b.n]=0; logline("sink", m); bc("sink:dry:plan-logged");
    return 0;
  }
  if (g_sink == SINK_HELPER){
    bc("sink:helper:connect");
    if (helper_connect() == 0){
      logline("sink","helper connected -- frames forwarded OUT OF PROCESS (no display call in carplay)");
      bc("sink:helper:connected");
    } else {
      logline("sink","helper absent -- frames will be dropped (safe); retried per frame");
      bc("sink:helper:absent");
    }
    return 0;
  }
  /* SINK_LOCAL -- the ACK was already verified in sink_resolve() */
  if (g_sink_local == 0 || g_sink_local == 2){
    bc("sink:local:bridge:create");
    if (bridge_create(w, h, ivi_id) == 0){
      logline("sink","LOCAL bridge active (Cinemo decodes+renders in-carplay)");
      logline("cluster/surfname", surf_name());
      bc("sink:local:bridge:ok"); sink_local_up = 1; return 0;
    }
    bc("sink:local:bridge:fail");
    if (!(g_sink_lf || g_sink_local == 2)){
      logline("sink","bridge failed; surface fallback disabled (MIB3_SHIM_LOCAL_FALLBACK=0) -- no sink");
      return -1;
    }
  }
  if (g_sink_local == 1 || g_sink_local == 2 || g_sink_lf){
    bc("sink:local:surface:create");
    if (surface_create(w, h, ivi_id) == 0){
      logline("sink","LOCAL surface active (fallback, no decode)");
      logline("cluster/surfname", surf_name());
      bc("sink:local:surface:ok"); sink_local_up = 1; return 0;
    }
    bc("sink:local:surface:fail");
  }
  logline("sink","LOCAL sink unavailable (stream still serviced)");
  return -1;
}

static int sink_feed(const void *data, unsigned long len, unsigned long ts){
  sink_resolve();
  if (g_sink == SINK_HELPER) return helper_feed(data, len, ts);
  if (g_sink == SINK_LOCAL){
    if ((g_sink_local == 0 || g_sink_local == 2) && bridge_active()) return bridge_feed(data, len, ts);
    if ((g_sink_local == 1 || g_sink_local == 2 || g_sink_lf) && g_gfx.ready && g_gfx.w > 0 && g_gfx.h > 0){
      unsigned long need = (unsigned long)g_gfx.w * (unsigned long)g_gfx.h * 4UL;
      if (len >= need) return surface_write_frame(data, g_gfx.w, g_gfx.h);
    }
    return 0;
  }
  return 0;   /* off / dry: dropped */
}

/* ---- harness hooks (sink dispatcher + out-of-process helper) ------------ */
int  mib3shim_sink_mode(void){ sink_resolve(); return g_sink; }
int  mib3shim_sink_local(void){ sink_resolve(); return g_sink_local; }
int  mib3shim_sink_start(int w,int h,unsigned ivi){ sink_resolve(); return sink_start(w,h,ivi); }
int  mib3shim_sink_feed(const void *d,unsigned long n,unsigned long ts){ return sink_feed(d,n,ts); }
int  mib3shim_sink_local_up(void){ return sink_local_up; }
int  mib3shim_helper_connect(void){ return helper_connect(); }
int  mib3shim_helper_feed(const void *d,unsigned long n,unsigned long ts){ return helper_feed(d,n,ts); }
int  mib3shim_helper_connected(void){ return hlp_fd >= 0; }
unsigned long mib3shim_helper_sent(void){ return hlp_sent; }
unsigned long mib3shim_helper_dropped(void){ return hlp_drop; }
const char *mib3shim_bc_path(void){ if (bc_fd == -2){ bc_pick_path(); } return bc_path; }

/* ==========================================================================
 * THE CRASH-GUARD SEAM (2026-10-15/16)
 *
 * The unit's stock fault: on teardown (systemctl stop/restart carplay), the
 * CCarSSWRsiClient destructor frees the wrapped RSI client and sends its
 * unsubscribes; the `{type:"unsubscribe"}` ack comes back through librsic's
 * dispatch (WsClientImpl::broadcastOnMessage -> WsSubscribeClient::onMessage
 * -> WsSubscribeClient::broadcastMessage) and is delivered to the FREED
 * callback -- FUN_00392778 dereferences NULL+0x10 (si_addr 0x10), SIGSEGV,
 * and LG's app-crash reset reboots the whole unit.
 *
 * VERIFIED facts (see REPORT_REDTEAM_CRASH_2026-10-15.md):
 *  - all these librsic symbols are EXPORTED and every relevant internal call
 *    goes through the PLT (inside-librsic calls at e.g. 0x3533c, 0x62e98) --
 *    so LD_PRELOAD interposition intercepts them.
 *  - carplay's own teardown calls these via cross-DSO PLT too.
 *
 * THE GUARD: a process-global flag set by ANY unsubscribe call; while set,
 * broadcastOnMessage / broadcastMessage drop message dispatches (return
 * silently). No messages reach the freed callback. A process restart resets
 * the flag trivially (new address space).
 *
 * Gate: armed only when the stick marker is present (gate_active()), same as
 * the other seams; MIB3_SHIM_GUARD=0 overrides. Delegation to the real code
 * via R( <mangled name>) on first use.
 *
 * KNOWN LIMITS (deliberate, benign): the flag is all-or-nothing for the rest
 * of the process lifetime; a single mid-session unsubscribe would mute all
 * subsequent dispatches. On this build unsubscribes happen only at teardown,
 * so the window is exactly the dangerous one.
 * ======================================================================== */
static int g_guard_teardown = 0;         /* 0 = normal dispatch, 1 = dropping */
static unsigned long g_guard_set_count = 0, g_guard_drop_count = 0;

/* gate discipline: the guard only arms while the stick marker is present --
 * same contract as every other seam (pull the stick => next sessions inert). */
static int guard_armed(void){
  if (!env_on("MIB3_SHIM_GUARD", 1)) return 0;
  resolve();
  return gate_active();
}

typedef void (*fn_guard_v2)(void*, void*);                  /* (this, msg&) */
typedef void (*fn_guard_unsub_str_b)(void*, void*, u8);     /* (this, const string&, bool) */
typedef void (*fn_guard_unsub_cond_b)(void*, void*, u8);    /* (this, const WsSubscribeCondition&, bool) */
typedef void (*fn_guard_unsub_all)(void*);                  /* (this) */
typedef void (*fn_guard_unsub_str)(void*, void*);           /* WsClientImpl::unsubscribe(string) */

static void *guard_real(const char *mangled){
  /* RTLD_NEXT ONLY -- deliberately NOT the two-tier R().
   *
   * The guard seams only ever sit BEFORE librsic in the search order (LD_PRELOAD
   * is first).  RTLD_NEXT is the correct "next object after us" lookup there.
   * RTLD_DEFAULT is POISON for these symbols: when the real symbol is absent
   * (e.g. the desk bench, which has no librsic) dlsym(RTLD_DEFAULT, mangled)
   * finds THIS shim's own interposer, so the seam delegates to itself and
   * recurses/hangs.  (Found 2026-10-15: it made driver_guard hang on the desk for
   * all three shim builds.)  RTLD_NEXT returns NULL instead, which is exactly
   * "no real function to forward to". */
  return dlsym(RTLD_NEXT, mangled);
}

/* The exported wrappers. Names are the EXACT mangled identity of the librsic
 * symbols (verified via nm -D); the asm-alias carries the name, the C ABI
 * carries the argument registers. Return type: all are void-returning per the
 * callers' disassembly (no x0 use after the call at any call site window). */

static void guard_note_set(const char *what){
  if (!g_guard_teardown) logline("guard","teardown detected -- dropping dispatches");
  g_guard_teardown = 1;
  g_guard_set_count++;
  logkv("guard",what,(u64)g_guard_set_count,0);
}

static void guard_maybe_drop(void){
  if (g_guard_teardown){
    g_guard_drop_count++;
    logkv("guard","dropped dispatch",(u64)g_guard_drop_count,0);
  }
}
/* pass-through when the guard is not armed: delegate ONLY (no flag side-effects) */
static int guard_passthrough(void){ return !guard_armed(); }

/* --- WsSubscribeClient::broadcastMessage(const WsClientMessage&) -----------
 * The fatal seam: called via PLT from WsSubscribeClient::onMessage (0x62e98). */
void _ZN3SAI17WsSubscribeClient16broadcastMessageERKNS_15WsClientMessageE(void *self, void *msg)
  __asm__("_ZN3SAI17WsSubscribeClient16broadcastMessageERKNS_15WsClientMessageE");
void _ZN3SAI17WsSubscribeClient16broadcastMessageERKNS_15WsClientMessageE(void *self, void *msg){
  static void *real; static int tried;
  if (!tried){ real = guard_real("_ZN3SAI17WsSubscribeClient16broadcastMessageERKNS_15WsClientMessageE"); tried = 1; }
  if (guard_passthrough()){ if (real) ((fn_guard_v2)real)(self, msg); return; }
  guard_maybe_drop();
  if (g_guard_teardown) return;               /* the freed callback never sees this */
  if (real) ((fn_guard_v2)real)(self, msg);
}

/* --- WsClientImpl::broadcastOnMessage(const WsClientMessage&) --------------
 * The parent seam; dropping here also suffices (called via PLT at 0x3533c). */
void _ZN3SAI12WsClientImpl18broadcastOnMessageERKNS_15WsClientMessageE(void *self, void *msg)
  __asm__("_ZN3SAI12WsClientImpl18broadcastOnMessageERKNS_15WsClientMessageE");
void _ZN3SAI12WsClientImpl18broadcastOnMessageERKNS_15WsClientMessageE(void *self, void *msg){
  static void *real; static int tried;
  if (!tried){ real = guard_real("_ZN3SAI12WsClientImpl18broadcastOnMessageERKNS_15WsClientMessageE"); tried = 1; }
  if (guard_passthrough()){ if (real) ((fn_guard_v2)real)(self, msg); return; }
  guard_maybe_drop();
  if (g_guard_teardown) return;
  if (real) ((fn_guard_v2)real)(self, msg);
}

/* --- the unsubscribe family: flag setters --------------------------------- */
void _ZN3SAI17WsSubscribeClient11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEb(void *self, void *a, u8 b)
  __asm__("_ZN3SAI17WsSubscribeClient11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEb");
void _ZN3SAI17WsSubscribeClient11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEb(void *self, void *a, u8 b){
  static void *real; static int tried;
  if (!tried){ real = guard_real("_ZN3SAI17WsSubscribeClient11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEb"); tried = 1; }
  if (!guard_passthrough()) guard_note_set("WsSubscribeClient::unsubscribe(string,bool)");
  if (real) ((fn_guard_unsub_str_b)real)(self, a, b);
}

void _ZN3SAI17WsSubscribeClient11unsubscribeERKNS_20WsSubscribeConditionEb(void *self, void *a, u8 b)
  __asm__("_ZN3SAI17WsSubscribeClient11unsubscribeERKNS_20WsSubscribeConditionEb");
void _ZN3SAI17WsSubscribeClient11unsubscribeERKNS_20WsSubscribeConditionEb(void *self, void *a, u8 b){
  static void *real; static int tried;
  if (!tried){ real = guard_real("_ZN3SAI17WsSubscribeClient11unsubscribeERKNS_20WsSubscribeConditionEb"); tried = 1; }
  if (!guard_passthrough()) guard_note_set("WsSubscribeClient::unsubscribe(cond,bool)");
  if (real) ((fn_guard_unsub_cond_b)real)(self, a, b);
}

void _ZN3SAI17WsSubscribeClient14unsubscribeAllEv(void *self)
  __asm__("_ZN3SAI17WsSubscribeClient14unsubscribeAllEv");
void _ZN3SAI17WsSubscribeClient14unsubscribeAllEv(void *self){
  static void *real; static int tried;
  if (!tried){ real = guard_real("_ZN3SAI17WsSubscribeClient14unsubscribeAllEv"); tried = 1; }
  if (!guard_passthrough()) guard_note_set("WsSubscribeClient::unsubscribeAll()");
  if (real) ((fn_guard_unsub_all)real)(self);
}

void _ZN3SAI12WsClientImpl11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(void *self, void *a)
  __asm__("_ZN3SAI12WsClientImpl11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
void _ZN3SAI12WsClientImpl11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(void *self, void *a){
  static void *real; static int tried;
  if (!tried){ real = guard_real("_ZN3SAI12WsClientImpl11unsubscribeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE"); tried = 1; }
  if (!guard_passthrough()) guard_note_set("WsClientImpl::unsubscribe(string)");
  if (real) ((fn_guard_unsub_str)real)(self, a);
}

/* --- guard status probes for the harness/kit ------------------------------ */
int  mib3shim_guard_teardown(void){ return g_guard_teardown; }
unsigned long mib3shim_guard_sets(void){ return g_guard_set_count; }
unsigned long mib3shim_guard_drops(void){ return g_guard_drop_count; }
