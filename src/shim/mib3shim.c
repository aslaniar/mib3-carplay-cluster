/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ============================================================================
 * mib3shim.c -- the shim injected into the LG MIB3 CarPlay stack (R8830L).
 *
 * Job: make the head unit advertise a SECOND (instrument-cluster) display to the
 * iPhone, so iOS opens the type-111 Apple Maps cluster stream -- then hand the
 * phone's own H.264 for it to the Path B sender.  Nothing here decodes or draws.
 *
 * Built with NO libc dependency (see build.sh): every symbol it needs (libc +
 * CoreFoundation + the real plugin functions) is resolved at runtime via
 * dlsym() from the host process, which is why it links against the unit's
 * glibc 2.23 without a matching toolchain.
 *
 * THREE SEAMS, and every feature below hangs off one of them (inject/
 * inject_shim.py patches exactly these three GOT slots):
 *   AirPlayInfoArrayAddScreenDisplay  -- LG calls it once (main screen);
 *                                        we capture the args and call it AGAIN
 *                                        with cluster geometry.
 *   AirPlayReceiverSessionSetup       -- log, optionally inject the
 *                                        "enabledFeatures" array into the
 *                                        CONTROL SETUP response, and own the
 *                                        type-111 stream.
 *   AirPlayCopyServerInfo             -- complete the modern CarKit /info schema
 *                                        (uid/properties/initialVideoStreams +
 *                                        top-level displayPanels/extendedFeatures)
 *                                        on the mutable /info dict the unit is
 *                                        about to serialise.
 *
 * Logs to /tmp/mib3shim.log (world-writable tmpfs; SMACK `_`).
 * Env knobs (the load-bearing ones; the RUN banner prints the live set):
 *   MIB3_SHIM_ADD=0/1         advertise the cluster display (default 1)
 *   MIB3_SHIM_W/H/FPS         cluster geometry (default 1280x480, FPS 60 -- FPS is
 *                             the advertised maxFPS and the phone uses it as its
 *                             RATE, so 60 delivers 26.6 fps; see the banner note)
 *   MIB3_SHIM_INFO=0/1        complete the modern /info schema (default 1)
 *   MIB3_SHIM_ENABLEDFEAT=0/1 inject enabledFeatures into the CONTROL SETUP (default 1)
 *   MIB3_SHIM_PORT=<n>        cluster data port (default: 5000, then 6031, then
 *                             ephemeral).  5000 is the only one INSIDE the
 *                             unit's firewall allowlist (GENERIC_PORT_IN_FILTER
 *                             "-i ncm0 -p tcp --dport 5000:5001"); 6031 and any
 *                             ephemeral port are FIREWALL-DROPPED on ncm0/wlap
 *                             -- the phone's SYN never reaches us -- and need
 *                             a firewall rule this repo does not ship.
 *   MIB3_SHIM_CMDS=0/1        send forceKeyFrame/showUI/setNightMode to the
 *                             phone once the 111 stream connects (default 1)
 *   MIB3_SHIM_SINK=off|helper where cluster frames go (default off; `helper`
 *                             = the AU socket -> mib3clusterstream.  Anything
 *                             else is refused loudly -- the in-process display
 *                             sink is gone)
 *   MIB3_SHIM_HELPER=<path>   the AU socket path (default /dev/shm/mib3cluster.sock)
 *   MIB3_SHIM_IDR_MS=<n>      recovery-point request cadence (default 500 ms,
 *                             0 = off; the phone honours ~1 in 10)
 *   MIB3_SHIM_NIGHT=0|1       DEBUG force for setNightMode only (unset = follow
 *                             the car: /info mirrors the main display's mode)
 *   MIB3_SHIM_EF_IAPCHANNEL=1 also claim "iAPChannel" in enabledFeatures
 *                             (default 0 -- only altScreen + viewAreas are
 *                             proven on a stock head unit; every string flips
 *                             a phone-side endpoint flag, so claim nothing
 *                             extra)
 *   (view-area / zoom / appearance / telemetry knobs: see the VIEW AREAS,
 *    ZOOM and APPEARANCE blocks; MIB3_SHIM_CTL5_DUMP defaults ON to /dev/shm.)
 * ==========================================================================*/

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long      u64;
typedef long               s64;
static u64 mono_ms(void);          /* defined below; used by the lossless AU hop */
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
extern void  *memset(void *dst, int c, unsigned long n);
extern void  *memcpy(void *dst, const void *src, unsigned long n);
extern int    strcmp(const char *a, const char *b);
extern int    strncmp(const char *a, const char *b, unsigned long n);

#define RTLD_NEXT    ((void *)-1L)
#define RTLD_DEFAULT ((void *)0)

/* ---------------------------------------------------------------------------
 * Two-tier symbol resolution.
 *
 * RTLD_NEXT is correct when the shim is loaded BEFORE the real libraries
 * (DT_NEEDED ordering): it finds the next object after us.  When the shim is
 * injected into a process that is ALREADY RUNNING it is appended LAST, so
 * RTLD_NEXT finds nothing, every pointer came back NULL, and each intercepted
 * call was broken by the shim that meant to forward it.  RTLD_DEFAULT is
 * correct there: the real libraries are in the global scope and the first
 * definition found is the real one.  The fallback only runs when RTLD_NEXT
 * fails, so load-time behaviour is unchanged.
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
#define O_TRUNC  01000

/* ---- minimal string/format helpers (no libc stdio) ----------------------- */

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
static void bhex(buf *b, u64 v){ bs(b,"0x"); bu64(b,v,16,0); }

/* ---- logging -------------------------------------------------------------
 * /tmp is RAM, so the log rotates to /tmp/mib3shim.log.1 at LOG_ROTATE_BYTES.
 * Rotation never closes the fd other threads write through: dup2() swaps the new
 * file in under the same fd number, so a concurrent write lands in one file or
 * the other, never in an fd number something else has reused. */
#define LOG_PATH "/tmp/mib3shim.log"
#define LOG_ROTATE_BYTES (4UL << 20)
extern int rename(const char *from, const char *to);
static int logfd = -1;
static unsigned long log_bytes;          /* size of the current file */
static int log_rotating;
static int (*p_dup2)(int, int);
static void log_open(void){
  if (logfd < 0){
    long (*lsk)(int, long, int) = (long (*)(int, long, int))R("lseek");
    p_dup2 = (int (*)(int, int))R("dup2");
    logfd = open(LOG_PATH, O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (logfd >= 0 && lsk){ long sz = lsk(logfd, 0, 2 /*SEEK_END*/); if (sz > 0) log_bytes = (unsigned long)sz; }
  }
}
static void log_rotate(void){
  int nfd;
  if (__atomic_exchange_n(&log_rotating, 1, __ATOMIC_ACQUIRE)) return;
  /* re-check: another thread may have rotated between our add and here */
  if (__atomic_load_n(&log_bytes, __ATOMIC_RELAXED) >= LOG_ROTATE_BYTES && p_dup2){
    /* no rename (the log was deleted, say): truncate instead, so the bound holds */
    int moved = rename(LOG_PATH, LOG_PATH ".1") == 0;
    nfd = open(LOG_PATH, O_WRONLY|O_CREAT|O_APPEND|(moved ? 0 : O_TRUNC), 0644);
    if ((nfd < 0 || p_dup2(nfd, logfd) < 0) && moved)
      rename(LOG_PATH ".1", LOG_PATH);         /* keep writing where readers look */
    if (nfd >= 0) close(nfd);
    __atomic_store_n(&log_bytes, 0, __ATOMIC_RELAXED);
  }
  __atomic_store_n(&log_rotating, 0, __ATOMIC_RELEASE);
}
static void log_write(const char *mem, unsigned long n){
  log_open();
  if (logfd < 0) return;
  write(logfd, mem, n);
  if (__atomic_add_fetch(&log_bytes, n, __ATOMIC_RELAXED) >= LOG_ROTATE_BYTES) log_rotate();
}
/* ---- KNOB SNAPSHOT --------------------------------------------------------
 * The supervisor can setenv() into carplay while it runs, and glibc's setenv may
 * realloc environ under a concurrent getenv.  So every knob the receiver loop
 * consults is read ONCE, at stream connect (knobs_load, from cluster_thread), and
 * the loop reads this struct instead.  Names and defaults: see knobs_load.  A
 * harness entry point that runs with no stream loads it on first use. */
typedef struct {
  int ready;
  int cmds, cmdgen, cmdalive, cmdack, idr_ms, night_forced;
  int viewareas, va_cmd, va_count;
  int appearance, app_map, app_mode, app_setting;
  long hlp_eagain, hlp_sndbuf;
  unsigned long ctl5_max;
  char helper[108];
} knobs;
static knobs g_kn;
static void knobs_load(void);           /* defined after va_count */
static const knobs *kn(void){ if (!g_kn.ready) knobs_load(); return &g_kn; }
/* ---- opcode-5: the SENDER'S OWN TELEMETRY, not a keepalive (2026-10-20) -----
 * The body is a binary plist of the PHONE's counters -- encoderCurrentFPS,
 * sinkOverflowDropFPS, rttAvg, txUsageAvg -- the only in-band answer to "why does
 * the phone encode ~15 fps when it is allowed 30": its own rate control, a thermal
 * clamp (iOS DBThermalMonitor level 1 == exactly 15), or the sender DROPPING frames
 * because we drain too slowly (sinkOverflowDropFPS).
 *
 *   MIB3_SHIM_CTL5_DUMP=<path>   destination (default /dev/shm/cl_ctl5.bin)
 *   MIB3_SHIM_CTL5_DUMP=off      disable
 *   MIB3_SHIM_CTL5_MAX=8388608   hard size cap, then it stops
 * Record format: <u32 LE length><body>, so a reader can split them.
 * THE LABEL TRAP: never pre-create that file as root -- a root-created file is
 * common::bin and carplay::app cannot write it.  Let the shim create it.
 *
 * Default ON, deliberately: as an opt-in knob it once said NOTHING while 1156
 * records arrived and no file appeared (the getenv never reached the target).  The
 * RUN banner prints CTL5=, and the reason is logged when a record lands. */
static int ctl5_fd = -2;                 /* -2 undecided, -1 disabled */
static int ctl5_want = -1;               /* -1 unknown, 0 off, 1 on */
static const char *ctl5_path = "";       /* destination while enabled */
static const char *ctl5_why = "";        /* why it is off, for the log */
static void logkv(const char *tag, const char *key, u64 v, int hex);   /* defined below */
static void logline(const char *tag, const char *msg);                 /* defined below */
static unsigned long ctl5_written = 0;

static void ctl5_resolve(void)
{
  const char *p;
  if (ctl5_want >= 0) return;
  p = getenv("MIB3_SHIM_CTL5_DUMP");
  if (p && (!p[0] || !strcmp(p, "off") || !strcmp(p, "0"))) {
    ctl5_want = 0; ctl5_why = "MIB3_SHIM_CTL5_DUMP=off"; return;
  }
  if (!p || !p[0]) p = "/dev/shm/cl_ctl5.bin";
  ctl5_want = 1; ctl5_path = p; ctl5_why = "";   /* opened lazily, ONCE */
}

static void ctl5_dump(const unsigned char *body, unsigned long n)
{
  unsigned long cap = kn()->ctl5_max;
  ctl5_resolve();
  if (n == 0 || n > (64UL << 10)) return;          /* sanity: a plist is small */
  if (ctl5_want <= 0){
    if (ctl5_fd == -2){
      ctl5_fd = -1;
      logline("cluster/ctl5","telemetry dump OFF -- ");
      logline("cluster/ctl5", ctl5_why);
    }
    return;
  }
  if (ctl5_fd == -2){
    ctl5_fd = open(ctl5_path, O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (ctl5_fd >= 0){ logline("cluster/ctl5","dump enabled -> "); logline("cluster/ctl5", ctl5_path); }
    else { ctl5_fd = -1;
      logline("cluster/ctl5","dump OFF -- open failed. NEVER pre-create this file as root: "
                             "a root-created file is common::bin and carplay::app cannot write it.");
      return; }
  }
  if (ctl5_fd < 0) return;
  if (ctl5_written >= cap){ close(ctl5_fd); ctl5_fd = -1;
    logkv("cluster/ctl5","dump complete bytes",(u64)ctl5_written,0); return; }
  { unsigned char hdr[4];
    hdr[0]=(unsigned char)(n); hdr[1]=(unsigned char)(n >> 8);
    hdr[2]=(unsigned char)(n >> 16); hdr[3]=(unsigned char)(n >> 24);
    if (write(ctl5_fd, hdr, 4) != 4 || write(ctl5_fd, body, (long)n) != (long)n){
      close(ctl5_fd); ctl5_fd = -1; logline("cluster/ctl5","write failed -> disabled"); return; }
    ctl5_written += 4UL + n; }
  { static unsigned long seen = 0;
    if ((++seen % 200) == 1) logkv("cluster/ctl5","records bytes",(u64)ctl5_written,0); }
}
static void logline(const char *tag, const char *msg){
  char mem[2048]; buf b = { mem, sizeof(mem)-2, 0 };
  bs(&b,"[mib3shim] "); bs(&b,tag); bs(&b,": "); bs(&b,msg); bch(&b,'\n');
  mem[b.n]=0;                       /* b.n <= cap = sizeof-2 -> always in-bounds */
  log_write(mem, b.n);
}

/* ---------------------------------------------------------------------------
 * INSTRUMENTATION: an entry marker that depends on NOTHING -- not resolve(),
 * not the /tmp log path, not any env.  If /dev/shm/mib3shim_hit never appears,
 * the shim was never entered at all and the call never reached us.
 * ------------------------------------------------------------------------- */
static void hit(const char *tag, unsigned long n)
{
  int f = open("/dev/shm/mib3shim_hit", O_WRONLY|O_CREAT|O_TRUNC, 0666);
  if (f >= 0){ write(f, tag, n); close(f); }
}
static void logkv(const char *tag, const char *key, u64 v, int hex){
  char mem[256]; buf b={mem,sizeof(mem)-2,0};
  bs(&b,key); bs(&b,"="); if(hex) bhex(&b,v); else bu64(&b,v,10,0);
  mem[b.n]=0;
  logline(tag, mem);
}

#include "mib3helper.h"

/* forward: the lossless-hop ms clock lives in the command block below (mono_ms);
 * the A5 stall bound needs ms resolution, time(0) only has seconds. */
static u64 mono_ms(void);

/* ==========================================================================
 * BREADCRUMBS -- evidence that SURVIVES A RESET.
 *
 * A reset wipes /tmp and /dev/shm, so the /tmp log and the entry marker both die
 * and the failing step becomes UNDIAGNOSABLE (2026-10-16: the sink attempt that
 * reset the unit).  The only storage that survives is the USB stick, so every
 * risky step is appended with an explicit write()+fsync().
 *
 * The file MUST be created BY THE SHIM: a root-created file carries the wrong
 * SMACK label and carplay's writes vanish silently (THE LABEL TRAP).
 *
 *   MIB3_SHIM_BC=<path>   explicit breadcrumb file
 *   else auto: <stick>/PART1/mib3shim.bc where <stick> is the same /tmp/USB/
 *         PORT<n> scan the activation gate uses; fallback /dev/shm/mib3shim.bc
 *         (RAM: best-effort only -- it does NOT survive a reset).
 * ========================================================================*/
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
 * OUT-OF-PROCESS DELIVERY -- the only sink left (shim side).
 *
 * Forward each decrypted access unit, length-prefixed (mib3helper.h), to a
 * SEPARATE process over an AF_UNIX socket.  carplay performs NO display-touching
 * call at all, so a helper fault cannot black the centre screen or trip
 * carplay.service's FailureAction=reboot-immediate.
 *
 *   MIB3_SHIM_HELPER=<path>   default /dev/shm/mib3cluster.sock
 * ========================================================================*/
extern int socket(int, int, int);
extern int connect(int, const void*, unsigned int);
extern int setsockopt(int, int, int, const void*, unsigned int);
extern int getsockopt(int, int, int, void *, unsigned int *);
extern long send(int, const void*, unsigned long, int);
extern int *__errno_location(void);
extern long time(long *);
static u64 env_num(const char *name, u64 dflt); /* defined after the CF section */
#define SHIM_ERRNO (*__errno_location())
#define O_NONBLOCK_ 04000
#define SOL_SOCKET_ 1
#define SO_SNDTIMEO_ 21
#define SO_SNDBUF_  7
#define SO_KEEPALIVE_  9
#define SOL_TCP_       6
#define TCP_KEEPIDLE_  4
#define TCP_KEEPINTVL_ 5
#define TCP_KEEPCNT_   6
static int hlp_fd = -1;
static unsigned long hlp_seq = 0, hlp_sent = 0, hlp_drop = 0, hlp_conns = 0;
/* the granted SO_SNDBUF of the live helper socket (0 = unknown).  The send-side
 * buffer is what decides whether a decoder that pauses ~100 ms turns into a
 * dropped AU, and it is the one hop with no other counter. */
static int hlp_sndbuf = 0;
/* BOUNDED EAGAIN RECOVERY -- one rule, two bounds (2026-10-19, retimed 2026-10-28).
 * A helper whose peer is gone (nobody reading) makes every send() return EAGAIN
 * with MSG_DONTWAIT, and keeping the socket on EAGAIN held it FOREVER: every AU
 * dropped, the cluster frozen on an old frame while the log said "receiving
 * video".  So track consecutive EAGAINs and the clock of the last good send, and
 * close (the next AU reconnects) when either bound trips.  A slow-but-alive
 * helper resets the counter on every success and never trips it.
 * The bounds are in MILLISECONDS, not attempts: with a bounded wait per failed
 * record a dead consumer burns a full wait per attempt, so an attempt-count
 * bound multiplied the wait into ~2-2.5 s of the cluster receiver thread.  Now
 * MIB3_SHIM_HLP_WAIT default 60 ms plus MIB3_SHIM_HLP_STALL_MS default 600 ms --
 * about one second.  A healthy sender holds <=8 queued AUs and drains at
 * >=1 AU/10 ms, so the shorter wait changes only the failure SHAPE: the post-fix
 * sessions measured zero EAGAINs and zero drops. */
static int  hlp_eagain = 0;          /* consecutive EAGAINs since the last success */
static u64  hlp_last_ok_ms = 0;      /* mono_ms() of the last fully-sent record */
static long hlp_wait_ms(void){       /* the shared per-record WAIT budget */
  static long cached = -1;
  if (cached < 0){ cached = (long)env_num("MIB3_SHIM_HLP_WAIT", 60); if (cached < 0) cached = 0; }
  return cached;
}
static long hlp_stall_ms(void){      /* ms-resolution dead-consumer cap */
  static long cached = -1;
  if (cached < 0){ cached = (long)env_num("MIB3_SHIM_HLP_STALL_MS", 600); if (cached < 0) cached = 0; }
  return cached;
}
static void helper_close(void){
  if (hlp_fd >= 0){ close(hlp_fd); hlp_fd = -1; }
  hlp_eagain = 0;                    /* a fresh socket starts with a clean slate */
  hlp_sndbuf = 0;
}
static const char *helper_path(void){
  return kn()->helper;
}
/* LAZY REATTACH IS A DEFECT ON A STATIC MAP.
 * helper_connect() used to run only from helper_feed(), so when the sender
 * restarted while the map was static -- the phone sends NOTHING for 600-840 ms at
 * a time, and can idle for minutes -- nothing called connect() and the cluster
 * stayed dark until the picture moved.  The attempt now rides the 100 ms clock
 * tick that already exists (cluster_wait_frame's poll), gated to 1/s. */
/* SINK dispatch modes: see THE SINK DISPATCHER at the bottom of this file.
 * MIB3_SHIM_SINK = off (default) | helper (the Path B sender on the AU socket). */
#define SINK_OFF 0
#define SINK_HELPER 2
static int g_sink = -1;          /* -1 = unresolved */
static int helper_connect(void);          /* forward: the pump hands the attempt to it */
static void helper_reattach_pump(void)
{
  static long last_try = -1;
  long now = time(0);
  if (g_sink != SINK_HELPER) return;        /* sink off: no helper to reattach to */
  if (hlp_fd >= 0) return;
  if (now == last_try) return;
  last_try = now;
  (void)helper_connect();
}
static int helper_connect(void){
  struct { unsigned short fam; char path[108]; } sa;
  int fd, i;
  if (hlp_fd >= 0) return 0;
  { const char *p = helper_path();
    memset(sa.path, 0, 108);
    for (i = 0; i < 107 && p[i]; i++) sa.path[i] = p[i]; }
  sa.fam = 1 /*AF_UNIX*/;
  fd = socket(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0);
  if (fd < 0){ logkv("sink","helper socket() errno",(u64)(unsigned)SHIM_ERRNO,0); return -1; }
  if (connect(fd, &sa, sizeof(sa)) != 0){
    /* retried every second while the sender is down: log at most every 10 s */
    static long last_log = -1;
    static unsigned long quiet = 0;
    int e = SHIM_ERRNO;
    long now = time(0);
    if (last_log < 0 || now - last_log >= 10){
      logline("sink/helper-path", helper_path());
      logkv("sink","helper connect() errno",(u64)(unsigned)e,0);
      if (quiet) logkv("sink","helper connect() failures not logged (10 s limit)",(u64)quiet,0);
      last_log = now; quiet = 0;
    } else quiet++;
    close(fd); return -1;
  }
  { struct { long s; long us; } tv; tv.s = 0; tv.us = 200000;
    setsockopt(fd, SOL_SOCKET_, SO_SNDTIMEO_, &tv, sizeof(tv)); }
  /* *** SIZE THE HELPER SOCKET FOR REAL AUs (2026-10-26) ***
   *
   * A DEFAULT AF_UNIX send buffer on this image is 208 KB -- two of the measured
   * 142 KB guidance AUs (amax=141999, 2026-10-24) -- so a decoder that
   * legitimately pauses reading for ~100 ms makes every send return EAGAIN and
   * the AU is DROPPED, silently, on the one hop with no other counter.  On a
   * stream with ~1 keyframe per 88 s a dropped AU is unrecoverable: exactly
   * "blocky flat areas, wrong tints, text stays sharp".
   *
   * So: ask for 4 MB (~28 max-size AUs), READ IT BACK, and say so when the kernel
   * clamped it -- SO_SNDBUF is clamped to 2*net.core.wmem_max, which the deleted
   * bring_up.sh used to raise, so an unraised unit silently caps at 208 KB.  Sends stay
   * non-blocking: no drop policy, timeout or thread changes here. */
  { long want = kn()->hlp_sndbuf;
    if (want > 0){
      int sb = (int)(want > (64u<<20) ? (64u<<20) : want);
      setsockopt(fd, SOL_SOCKET_, SO_SNDBUF_, &sb, sizeof(sb));
    }
    { int got = 0; unsigned int gl = (unsigned int)sizeof(got);
      if (getsockopt(fd, SOL_SOCKET_, SO_SNDBUF_, &got, &gl) == 0){
        hlp_sndbuf = got;
        logkv("sink","helper sndbuf granted",(u64)(u32)got,0);
        if (got < 425984) /* 2*208KB: the pre-bring_up ceiling */
          logline("sink","helper sndbuf CLAMPED -- raise net.core.wmem_max (echo 8388608 > /proc/sys/net/core/wmem_max) or AUs will drop here");
      } }
  }
  hlp_fd = fd; hlp_conns++;
  logkv("sink","helper connect #",(u64)hlp_conns,0);
  return 0;
}
/* MSG_NOSIGNAL: a write to a socket whose peer is gone raises SIGPIPE as well as
 * returning EPIPE, and SIGPIPE's default action is to KILL THE PROCESS.  This
 * shim runs INSIDE carplay, whose failure action on this unit is
 * reboot-immediate, and a helper dying mid-write is routine -- the controller
 * stops our decoder at the end of every session.  Local to these two writes: the
 * process's signal disposition is untouched, so nothing else in carplay changes. */
#define MIB3_MSG_NOSIGNAL 0x4000
/* Non-blocking send: a helper that stops reading (a wedged decoder, measured
 * 2026-10-19 with nfree=0) would otherwise make send() BLOCK inside carplay's
 * cluster receiver thread and stutter the whole session on the main screen.
 * A dropped AU is recoverable; a stalled receiver thread is not. */
#define MIB3_MSG_DONTWAIT 0x40

/* (time() and env_num() are declared at the top of the sink block.) */

/* EVERY DROP ON THIS HOP IS IN THE LOG (2026-10-26): one line, rate-limited to
 * 1/s, with the running totals, so the shim's log and the decoder's
 * qdrop/kdrop/awt can be diffed per window -- the GAP between them is the number
 * of phone frames lost between the two processes.  hlp_drop used to be readable
 * only through the probe export, which is how a whole session could artifact
 * with no counter anywhere explaining it. */
static void hlp_drop_log(const char *why){
  static long last_log = 0;
  long now = time(0);
  if (now == last_log) return;
  last_log = now;
  logline("sink/drop/why", why ? why : "?");
  logkv("sink","helper dropped total",(u64)hlp_drop,0);
  logkv("sink","helper sent total",(u64)hlp_sent,0);
  logkv("sink","helper sndbuf",(u64)(u32)hlp_sndbuf,0);
}

/* LOSSY-BY-DESIGN IS WRONG ON THIS HOP (2026-10-27, measured).
 *
 * helper_feed() used to send with MSG_DONTWAIT and, on EAGAIN, DROP the record.
 * That is right for a live decoder that must never stall carplay, and wrong
 * here: the phone's H.264 has ONE keyframe per 78 s (measured, 2 in 2077 AUs),
 * so a single dropped AU breaks the predictive chain for the rest of the
 * session.  Field run: `helper dropped total=3691` of `36659` sent -- 9 % of the
 * picture thrown away on the one hop with no recovery -- while every synthetic
 * test (file -> sender, no socket) held the picture at 10, 30 and 60 fps, with
 * one keyframe, with gaps, with the phone's exact High/full-range SPS.
 *
 * So: WAIT, bounded, for the consumer instead of discarding.  The wait is capped
 * by MIB3_SHIM_HLP_WAIT ms so a dead consumer still cannot hold the receiver
 * thread forever; an exhausted wait drops and logs, as before.  The record stays
 * all-or-nothing: a partial body still closes the socket. */
static long send_all(int fd, const void *p, unsigned long n, int wait_ms)
{
  const unsigned char *b = (const unsigned char *)p;
  unsigned long off = 0;
  u64 deadline = mono_ms() + (u64)(wait_ms > 0 ? wait_ms : 0);
  int spins = 0;
  /* ITERATION BOUND: the wall clock is the primary bound, but a process with NO
   * clock at all (mono_ms()==0 forever -- the trap that hung the audit bench's
   * hostile-peer run) would never expire the deadline.  Cap the EAGAIN wait at
   * 100 iterations x 20 ms = 2 s hard, independent of the clock, so the receiver
   * thread can never spin forever on this hop. */
  while (off < n){
    long r = send(fd, b + off, n - off, MIB3_MSG_NOSIGNAL|MIB3_MSG_DONTWAIT);
    if (r > 0){ off += (unsigned long)r; continue; }
    if (r < 0 && SHIM_ERRNO == 4 /*EINTR*/){
      /* A signal through carplay is routine; it interrupts nothing.  The old
       * code fell through to the fatal path below and helper_close()d for it --
       * a socket churn on every stray signal. */
      continue;
    }
    if (r < 0 && SHIM_ERRNO == 11 /*EAGAIN*/){
      struct pollfd { int fd; short ev; short rev; } pf;
      extern int poll(void*, unsigned long, int);
      if (mono_ms() >= deadline) return -1;
      if (++spins > 100) return -1;            /* clock-less belt: ~2 s hard cap */
      pf.fd = fd; pf.ev = 4 /*POLLOUT*/; pf.rev = 0;
      poll(&pf, 1, 20);
      continue;
    }
    return -1;
  }
  return (long)n;
}

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
      if (helper_connect() != 0){ hlp_drop++; hlp_drop_log("no decoder attached (helper absent)"); return -1; }
    } else { hlp_drop++; hlp_drop_log("no decoder attached (1/s connect gate)"); return -1; }
  }
  m32 = MIB3_HELP_MAGIC; for (i=0;i<4;i++) hdr[i]   = (unsigned char)((m32 >> (i*8)) & 0xff);
  m32 = (u32)len;        for (i=0;i<4;i++) hdr[4+i] = (unsigned char)((m32 >> (i*8)) & 0xff);
  m64 = ++hlp_seq;       for (i=0;i<8;i++) hdr[8+i] = (unsigned char)((m64 >> (i*8)) & 0xff);
  { long r = send_all(hlp_fd, hdr, MIB3_HELP_HDRLEN, hlp_wait_ms());
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
        u64 now2 = mono_ms();
        long emax = kn()->hlp_eagain;
        hlp_eagain++;
        hlp_drop_log("hdr send EAGAIN (decoder not reading / sndbuf full)");
        if (hlp_eagain >= emax ||
            (hlp_last_ok_ms && now2 - hlp_last_ok_ms > (u64)hlp_stall_ms())){
          logkv("sink","helper EAGAIN bound tripped -- closing",(u64)(u32)hlp_eagain,0);
          helper_close();
        }
      } else {
        hlp_drop_log("hdr send failed (fatal errno / partial)");
        helper_close();
      }
      return -1; }
  }
  { long r = send_all(hlp_fd, data, len, hlp_wait_ms());
    if (r != (long)len){
      /* Partial record = permanent desync; the helper must reconnect. */
      hlp_drop++; helper_close(); hlp_drop_log("body send short/partial (desync)"); return -1; } }
  hlp_eagain = 0;                 /* a complete record went out: reset the bound */
  hlp_last_ok_ms = mono_ms();
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
typedef int  (*fn_pthreadDetach)(void*);
typedef int  (*fn_close)(int);
typedef int  (*fn_chachaDerive)(void*, s64, int, int, void*); /* returns int status (0=ok) */
/* wire crypto (unit's own CFLite/ChaCha primitives) -- see stock FUN_00115800 +
 * AirPlayReceiverSessionScreen_ProcessFrames (0x11fbf0): the per-frame
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
/* ---------------------------------------------------------------------------
 * THE IN-PROCESS DISPLAY PATHS ARE NOT HERE.  A second Cinemo pipeline inside
 * carplay, an EGL/Wayland surface, a local sink, and the ScreenStreamProcessData
 * / CarPlayAppFrameworkSetSurface interposers were the black-screen/reset
 * hazard, and none of them is one of the THREE seams the inject route patches.
 * Path B delivers the phone's H.264 UNDECODED (shim -> AU socket ->
 * mib3clusterstream -> RTP/TS -> ECU), so no surface, GL, Cinemo or OMX ever
 * runs inside carplay: MIB3_SHIM_SINK=helper|off are the only modes.
 * ------------------------------------------------------------------------- */

/* NB: the 7 numeric args are 32-bit (undefined4 in the plugin) -- match the ABI. */
typedef int (*fn_AddScreenDisplay)(CFMutableArrayRef*, CFStringRef,
                                   int, int, int, int, int, int, int);
typedef int  (*fn_SessionSetup)(void*, void*, void**, int);   /* real ABI: returns the setup status, 4 args */
/* AirPlayReceiverSessionSendCommand(session, cmdDict{type,params}, completion, ctx)
 * -> status.  Verified on the unit's own libAirPlay (exported @ 0x17f3c): it
 * dispatch_sync_f's onto the session's own control queue (session+0x448) --
 * safe to call from our receiver thread; the CALLER CFReleases the cmd dict
 * after it returns (the stock AirPlayReceiverSessionForceKeyFrame pattern at
 * 0x1e31c: create mutable dict, SetValue("type","forceKeyFrame"),
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
static fn_CFGetInt64                p_CFGetInt64;
static void *p_kCFTypeArrayCallBacks;
static void *p_kCFLDictKeyCB;
static void *p_kCFLDictValCB;
static void *p_kCFLBooleanTrue;
static void *p_kCFLBooleanFalse;

/* cluster-stream (type 111) helpers + libAirPlay base (for RVA calls) */
static fn_DeriveAES         p_DeriveAES;
static fn_cx_init           p_cx_init;
static fn_cx_aad            p_cx_aad;
static fn_cx_dec            p_cx_dec;
static fn_cx_ver            p_cx_ver;
static fn_ctr_init          p_ctr_init;
static fn_ctr_upd           p_ctr_upd;
static fn_pthreadCreate     p_pthreadCreate;
static fn_pthreadDetach     p_pthreadDetach;
static fn_close             p_close;
typedef long (*fn_syscall)(long, ...);
static fn_syscall           p_syscall;
static long (*p_clock_gettime)(int, void*);
static void *(*p_malloc)(unsigned long);
static void  (*p_free)(void*);
static char                *g_libap_base = 0;

static fn_AddScreenDisplay real_AddScreenDisplay;
static fn_SessionSetup     real_SessionSetup;
static fn_CopyServerInfo   real_CopyServerInfo;
static fn_SendCommand      p_SendCommand;    /* the stock command shipper (showUI/forceKeyFrame/setNightMode) */

/* ==========================================================================
 * RUNTIME GATE -- the physical cure.
 *
 * The shim is loaded on every boot, so the LOAD is not the enable signal.  The
 * signal is a marker on the USB stick, checked HERE at runtime rather than at
 * boot because `sdcm` mounts the stick at multi-user.target, unordered relative
 * to carplay.service, so a boot-time check loses the race:
 *
 *     <stick>/mib3shim.enable      (/tmp/USB/PORTn/PART1/mib3shim.enable)
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

/* the altScreen ENABLEMENT must be consistent with the altScreen ADVERTISEMENT.
 * iOS enforces it: enabledFeatures containing "altScreen" requires >=2 entries in
 * the /info displays array, and its absence requires <=1 (the count test in
 * _carEndpoint_setupStreams).  Claiming an alt screen while advertising
 * ONE display makes iOS refuse the session -- the observed "session does not come
 * up".  So the cluster is one process-wide decision: the display seam sets this
 * flag, and the SETUP seam claims altScreen only when the flag is set.  No flag ->
 * no cluster -> no claim -> the response is byte-identical to stock. */
static volatile int g_cluster_advertised = 0;
static volatile int g_info_completed = 0;   /* set when the /info seam completed a cluster advert */

/* ---- the field findings this build carries --------------------------------
 * 1) THE WALL IS OUR OWN KERNEL FIREWALL.  The unit's `network` daemon applies an
 *    iptables/ip6tables template with base policy INPUT=DROP and a per-interface
 *    allowlist (strings of lge/app_ro/network/network, section commented
 *    "## carplay for ncm0 (usb interface)"):
 *        -A GENERIC_PORT_IN_FILTER -i ncm0 -p tcp --dport 5000:5001 -j ACCEPT
 *        -A GENERIC_PORT_IN_FILTER -i ncm0 -p tcp --dport 6030    -j ACCEPT
 *        -A GENERIC_PORT_IN_FILTER -i ncm0 -p tcp --dport 7000    -j ACCEPT
 *    (plus UDP 5001/5020/5353/6000/6003/6011; the same set for wlap/wlap2/wlap5;
 *    both IPv4 and IPv6; 80/443/49200:49499 are all-interface globals).  NOTHING
 *    allows 6031 or any ephemeral port on the phone-facing interface, so the
 *    phone's SYN to our advertised dataPort was silently DROPPED -- no RST, so it
 *    just times out and the session stays up.  The phone's observed socket set
 *    {7000, 5001, 6030} is EXACTLY that allowlist.
 * 2) enabledFeatures is scoped to the CONTROL SETUP and claims ONLY
 *    altScreen+viewAreas (the proven set).
 * 3) Part B wire format vs the stock receiver (ProcessFrames 0x11fbf0): chacha20
 *    length is bodySize-16 (the last 16 bytes are the Poly1305 tag, NOT
 *    ciphertext) and MUST be followed by chacha20_poly1305_verify(ctx, dst+n,
 *    src+bs-16, &ok) with n+m == bs-16; AES-CTR is a PERSISTENT keystream (Init
 *    once at SetSecurityInfo, Update per frame -- re-init per frame garbles every
 *    frame after the first); the chacha nonce is the 8-byte LE counter
 *    (init_64x64 reads 8 bytes, not 12); AVCC must be rewritten to Annex-B
 *    (4-byte length -> 00 00 00 01) and parsed for SPS/PPS.
 * 4) Bodies go to a HEAP buffer that grows to the frame size: the old receiver
 *    read into a static 64 KiB buffer while bounds-checking 8 MiB, so any IDR over
 *    64 KiB overran static memory.
 * 5) Once connected the phone sends opcode-1 (avcC) + opcode-5 telemetry but NO
 *    video until the CAR drives the scene (see the COMMANDS block below).
 * ------------------------------------------------------------------------ */

/* the cluster uuid we advertised (same string goes into /info and into the
 * showUI/forceKeyFrame/setNightMode command params -- CarKit matches it) */
static char g_cluster_uuid_str[64];
static volatile long g_night_mode = 0;     /* the advertised nightMode (for setNightMode) */
/* NIGHT MODE IS FOLLOW, NEVER ENFORCE.
 * The phone decides day/night and the same mode must apply to BOTH entries --
 * main and cluster -- as it changes; nothing below may invent a mode of its own.
 * The value is FILLED from the car's own published nightMode (what /info arrives
 * with), never overwritten, and MIB3_SHIM_NIGHT is a DEBUG force -- the only way
 * any setNightMode is ever shipped.  There is NO phone-side mode signal: the
 * opcode-1 flags byte flips thousands of times per session and was refuted as a
 * mode bit. */
static volatile long g_night_applied = -2;  /* the mode last sent to the phone */
/* session that owns the live cluster stream (command target for showUI etc.) */
static void *volatile g_cmd_session = 0;
/* COMMAND GENERATION.  Bumped whenever the command target changes
 * (cluster_setup for a new type-111 SETUP, and the harness entry point).  A
 * completion callback compares the generation it was issued under with this
 * value and DROPS a reply that belongs to a superseded session.  Deliberately a
 * THIRD, independent token -- same idea as cl_thread_gen and g_lsock_gen, because
 * it guards a third thing, the RTSP command channel, and the three can disagree. */
static volatile unsigned g_cmd_gen = 0;
/* The view-area index we believe the PHONE has applied, and the index we last
 * REQUESTED.  g_va_current moves only on a status-0 completion (never on send),
 * so a refused switch cannot strand the display in a state we think we are in. */
static volatile int g_va_current = 0;

/* THE CLUSTER SCENE URL: what the phone renders on the cluster.  The bare scene is
 * the field-proven default; MIB3_SHIM_URL overrides the whole string, and the one
 * value goes into /info initialURL AND every showUI.
 *
 * Parameterised forms from omonob/MHI2-Carplay-Maps (navigation_card_scene.h):
 *   pure map, no card:
 *     maps:/car/instrumentcluster/map?showETA=no&maneuverLayout=no&showSpeedLimit=no
 *   card layouts (maneuver card visible):
 *     maps:/car/instrumentcluster?showETA=no&maneuverLayout=leftAligned|rightAligned|topAligned&showSpeedLimit=no
 * Decompiled ground truth in the phone's DBInstrumentClusterURLHandler:
 * DBInstrumentClusterURLHandler parses the query into
 * CRSUIInstrumentClusterSceneSettings -- `.../map` = itemType 1 (map only),
 * showETA/showSpeedLimit/showCompass = yes|no, and maneuverLayout accepts only
 * top|left|rightAligned, so anything else (including "no") is justification 0.
 * The layout parameter decides where the map sits: see va_preset_rect. */
#define CLUSTER_URL_DEFAULT "maps:/car/instrumentcluster/map"   /* proven form; the MHI2 parameterised URL (showETA=/maneuverLayout=/showSpeedLimit=) is an UNTESTED experiment reachable via MIB3_SHIM_URL */
static char g_cluster_url[160];            /* showUI url (== the advertised initialURL) */

#define K128 (0x08000100)   /* kCFStringEncodingUTF8 */

static void resolve(void){
  if (__atomic_load_n(&resolved, __ATOMIC_ACQUIRE)) return;
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
  p_CFGetInt64                = (fn_CFGetInt64)               R("CFGetInt64");
  /* cluster-stream helpers (libAirPlay / libCoreUtils exports) */
  p_DeriveAES         = (fn_DeriveAES)        R("AirPlay_DeriveAESKeySHA512ForScreen");
  p_cx_init           = (fn_cx_init)          R("chacha20_poly1305_init_64x64");
  p_cx_aad            = (fn_cx_aad)           R("chacha20_poly1305_add_aad");
  p_cx_dec            = (fn_cx_dec)           R("chacha20_poly1305_decrypt");
  p_cx_ver            = (fn_cx_ver)           R("chacha20_poly1305_verify");
  p_ctr_init          = (fn_ctr_init)         R("AES_CTR_Init");
  p_ctr_upd           = (fn_ctr_upd)          R("AES_CTR_Update");
  p_pthreadCreate     = (fn_pthreadCreate)    R("pthread_create");
  p_pthreadDetach     = (fn_pthreadDetach)    R("pthread_detach");
  p_close             = (fn_close)            R("close");
  p_malloc            = (void*(*)(unsigned long)) R("malloc");
  p_free              = (void(*)(void*))          R("free");
  p_syscall           = (long(*)(long,...))       R("syscall");
  p_clock_gettime     = (long(*)(int,void*))      R("clock_gettime");
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
  __atomic_store_n(&resolved, 1, __ATOMIC_RELEASE);

  logline("init","resolved");
  logkv("init","p_CFArrayAppendValue",(u64)p_CFArrayAppendValue,1);
  logkv("init","p_kCFTypeArrayCallBacks",(u64)p_kCFTypeArrayCallBacks,1);
  logkv("init","p_CFDictionarySetInt64",(u64)p_CFDictionarySetInt64,1);
  logkv("init","p_CFDictionaryCreateMutable",(u64)p_CFDictionaryCreateMutable,1);
  logkv("init","real_AddScreenDisplay",(u64)real_AddScreenDisplay,1);
  logkv("init","real_SessionSetup",(u64)real_SessionSetup,1);
  logkv("init","real_CopyServerInfo",(u64)real_CopyServerInfo,1);
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
 * The phone additionally needs, on the cluster entry: type=111, initialURL,
 * and a viewAreas[] (with a safeArea).  We add those here, on the mutable dict
 * the real function just appended.
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
 * CLUSTER GEOMETRY -- ONE resolver for the cluster's pixel size, its advertised
 * PHYSICAL size (mm), and the relation between them.
 *
 * The display seam and the view-area preset used to read MIB3_SHIM_W/H separately,
 * and the hand-tuned branch for exactly 1280x480 never set originYPixels for any
 * other panel -- it inherited the previous area's value from the same stack slot --
 * so the safe area ended BELOW the frame:
 *
 *     2560x960:  area1 safe = 883,200,793,855   -> 200+855 = 1055 > 960
 *     1920x720:  area1 safe = 662,150,595,615   -> 150+615 =  765 > 720
 *     1280x480:  area1 safe = 439, 50,397,375   ->  50+375 =  425 <= 480  (ok)
 *
 * An INVALID advert: CarKit validates view areas
 * (CRDisplayScaleInfo.minAcceptableViewAreaSize* / isViewAreaPixelSizeAcceptable,
 * and _CRSizeFromAirPlayDictionary checks "area within limit"), and one
 * unparseable `displays` entry costs the WHOLE array -- session torn down, centre
 * screen black, connect/disconnect storm.  The log hid it because va_log_rect()
 * clamped negatives to 0.
 *
 * KNOBS (every default reproduces the shipped behaviour byte for byte):
 *   MIB3_SHIM_W / MIB3_SHIM_H   cluster pixel size (default 1280x480)
 *   MIB3_SHIM_SCALE=<pct>       multiply the pixel size (100 = off, default),
 *                               rounded UP to a multiple of 8 so neither encoder
 *                               nor decoder sees a ragged size:
 *                               100/150/200 -> 1280x480 / 1920x720 / 2560x960
 *   MIB3_SHIM_CWPHY / CHPHY     advertised physical mm (292x110).  With CHPHY
 *                               unset it is DERIVED from CWPHY and the pixel
 *                               aspect (round(CWPHY*h/w) = 110 at 1x), so the
 *                               advertised mm can never contradict the pixels.
 *   MIB3_SHIM_SCALE_PHY=1       scale the physical size WITH the pixels, so the
 *                               advertised DENSITY (px per mm) is unchanged: 2x
 *                               reads as a 584x219 mm panel at the same 111 dpi.
 *                               This is the density hypothesis's test knob;
 *                               default 0 keeps the real panel size and lets the
 *                               density rise (2x = 222 dpi), which is what a
 *                               genuinely supersampled stream is.
 * ========================================================================*/
typedef struct { int w, h; int phy_w, phy_h; } geom_t;
static int geom_round8(int v){ if (v < 8) return 8; return (v + 7) & ~7; }
static void cluster_geom(geom_t *g)
{
  const char *sph = getenv("MIB3_SHIM_CHPHY");
  long sc = (long)env_num("MIB3_SHIM_SCALE", 100);
  long w  = (long)env_num("MIB3_SHIM_W", 1280);
  long h  = (long)env_num("MIB3_SHIM_H", 480);
  long pw, ph;
  if (sc <= 0) sc = 100;
  if (sc < 25) sc = 25;
  if (sc > 400) sc = 400;
  if (sc != 100){ w = w * sc / 100; h = h * sc / 100; }
  g->w = geom_round8((int)w);
  g->h = geom_round8((int)h);
  if (g->w > 8192) g->w = 8192;
  if (g->h > 8192) g->h = 8192;
  pw = (long)env_num("MIB3_SHIM_CWPHY", 292);
  ph = (sph && sph[0]) ? (long)env_num("MIB3_SHIM_CHPHY", 110) : 0;
  if (env_on("MIB3_SHIM_SCALE_PHY", 0)){
    pw = pw * sc / 100;
    if (sph && sph[0]) ph = ph * sc / 100;
  }
  if (pw < 10) pw = 10;
  if (pw > 4000) pw = 4000;
  /* aspect-honest height when the operator did not pin one: 292*480/1280 = 110 */
  if (!(sph && sph[0])) ph = (pw * (long)g->h + (long)g->w / 2) / (long)g->w;
  if (ph < 10) ph = 10;
  if (ph > 4000) ph = 4000;
  g->phy_w = (int)pw;
  g->phy_h = (int)ph;
}
/* px per mm in thousandths: the density the PHONE sees.  Logged at every advert
 * so a density change is a number in the record instead of an inference. */
static int geom_density_milli(const geom_t *g){
  return (g && g->phy_w > 0) ? (int)((1000L * (long)g->w) / (long)g->phy_w) : 0;
}

/* ==========================================================================
 * VIEW AREAS -- telling the phone how much of the panel the cluster really gives
 * the map.
 *
 * The cluster's VIEW button cycles its layout: full-panel map, or a two-gauge
 * layout with the map in the middle.  In the gauge layout the ECU paints gauges
 * over our video and the PHONE never learns its window shrank -- we advertise one
 * full-panel area and never send updateViewArea -- so Apple Maps draws for the
 * whole panel and much of it hides behind the gauges.
 *
 * The phone needs `viewAreas` (an array of {originXPixels,originYPixels,
 * widthPixels,heightPixels,safeArea?}), `initialViewArea` (an index) and
 * `adjacentViewAreas`; an updateViewArea index is bounds-checked against that
 * array, so a second area must be PREDECLARED before we can switch to it.
 *
 * SHAPE (observed on a real unit): EVERY declared area keeps the FULL visible
 * frame and the areas differ only in the NESTED `safeArea`; an area whose
 * visible frame was reduced got the stream rejected at activation.
 *
 * KNOBS:
 *   MIB3_SHIM_VIEWAREAS=1        declare multiple view areas (default: off ->
 *                                the single full-panel area, unchanged)
 *   MIB3_SHIM_VA_COUNT=n         how many (default 3: this car has three layouts)
 *   MIB3_SHIM_VA_PRESET=1        derive class-safe boxes from CarPlay's classifier
 *                                (default ON; VA_PRESET=0 restores the old
 *                                full-panel/guess behaviour)
 *   MIB3_SHIM_VA<i>="x,y,w,h[,sx,sy,sw,sh]"  explicit rect for area i (wins)
 *   MIB3_SHIM_VA_VISIBLE=1       also shrink the *visible* frame of areas >0 to
 *                                their safe rect (a shape the phone rejects)
 *   MIB3_SHIM_VA_NOOUTSIDE=1     omit drawUIOutsideSafeArea from safeArea
 *   MIB3_SHIM_VA_CMD=0           do not ship updateViewArea on a switch
 * ========================================================================*/
typedef struct { int x, y, w, h; int sx, sy, sw, sh; } va_rect;

/* LEGACY safe rect for the gauge layout (VA_PRESET=0 only; the preset derives a
 * class-safe box from CarPlay's classifier instead).  This one was a GUESS: it
 * is centred -- (1280-480)/2 = 400 and (480-400)/2 = 40 -- and 480 px wide so
 * that CarPlay's CPSInstrumentClusterCardLayout does not classify it "narrow".
 * The documented threshold is ~498 px on a 1440 px panel = 34.6% of the width,
 * and 34.6% of 1280 is 443, so 480 clears it.  400 px tall clears the other
 * documented limit (~290 px for the centred card).  It is NOT measured on the
 * Arteon. */
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
static void knobs_load(void)
{
  knobs k;
  const char *s;
  unsigned long i;
  u64 cap;
  k.cmds        = env_on("MIB3_SHIM_CMDS", 1);
  k.cmdgen      = env_on("MIB3_SHIM_CMDGEN", 1);
  k.cmdalive    = env_on("MIB3_SHIM_CMDALIVE", 1);
  k.cmdack      = env_on("MIB3_SHIM_CMDACK", 1);
  k.idr_ms      = (int)env_num("MIB3_SHIM_IDR_MS", 500);
  k.night_forced = getenv("MIB3_SHIM_NIGHT") != 0;
  k.viewareas   = env_on("MIB3_SHIM_VIEWAREAS", 0);
  k.va_cmd      = env_on("MIB3_SHIM_VA_CMD", 1);
  k.va_count    = va_count();
  k.appearance  = env_on("MIB3_SHIM_APPEARANCE", 0);
  s = getenv("MIB3_SHIM_APPTARGET");
  k.app_map     = (s && (s[0]=='m' || s[0]=='M')) ? 1 : 0;
  k.app_mode    = (int)env_num("MIB3_SHIM_APPMODE", 0);
  k.app_setting = (int)env_num("MIB3_SHIM_APPSETTING", 0);
  k.hlp_eagain  = (long)env_num("MIB3_SHIM_HLP_EAGAIN", 32);
  k.hlp_sndbuf  = (long)env_num("MIB3_SHIM_HLP_SNDBUF", (u64)(4u<<20));
  cap = env_num("MIB3_SHIM_CTL5_MAX", 0);
  k.ctl5_max    = cap ? (unsigned long)cap : 8388608UL;
  s = getenv("MIB3_SHIM_HELPER");
  if (!s || !s[0]) s = MIB3_HELP_SOCK_DEFAULT;
  for (i = 0; i < sizeof(k.helper) - 1 && s[i]; i++) k.helper[i] = s[i];
  k.helper[i] = 0;
  /* these three cache for the process on first use; make that use happen here */
  (void)hlp_wait_ms(); (void)hlp_stall_ms(); ctl5_resolve();
  k.ready = 1;
  g_kn = k;
}
/* ==========================================================================
 * THE CLASSIFIER CarPlay applies to our safe box (reconstructed from
 * -[CPSInstrumentClusterCardViewController
 * initWithSafeAreaFrame:…physicalPixelWidth:] plus the DashBoard scene path).
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
 * For this panel (1280 px at 292 mm) scale = 0.670: wide >= 443 px, centred needs
 * safeH > 257 px.  BOTH cliffs move with the advertised mm (a true 278 mm panel
 * puts wide at >= 465 px), so a preset box keeps a margin instead of sitting on a
 * threshold.
 * ========================================================================*/
static int va_scale_milli(int cw, int phy_mm){
  if (phy_mm < 50 || phy_mm > 2000) phy_mm = 292;   /* absurd -> the default */
  if (cw < 100) cw = 1280;
  return (int)((153L * (long)cw) / (long)phy_mm);
}
static int va_wide_px(int cw, int phy_mm){ return (int)((660L * (long)va_scale_milli(cw, phy_mm)) / 1000L); }
static int va_centre_px(int cw, int phy_mm){ return (int)((383L * (long)va_scale_milli(cw, phy_mm)) / 1000L) + 1; }
/* The ETA threshold is 202 PIXELS AT 1x, i.e. a fixed number of POINTS, so it
 * scales with the advertised density like the other two cliffs.  202 px at
 * 1280 px / 292 mm == 202 * scale/670 px at any other geometry, which is
 * unchanged at 1x (the shipped behaviour) and 404 px at 2x with the real mm. */
static int va_eta_px(int cw, int phy_mm){ return (int)((202L * (long)va_scale_milli(cw, phy_mm)) / 670L); }
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
/* Clamp a rect into its own frame.  Returns a bitmask of what had to move
 * (1 = the visible frame, 4 = the nested safe area) so the caller can WARN.
 *
 * A safe area outside the display is an invalid advert, and an invalid advert
 * costs the whole CarPlay session.  The frame is the hard limit and the
 * class/position are the operator's intent, so the safe box is pulled back inside
 * and the event is LOGGED -- never shipped silently. */
static int va_fit(va_rect *r, int cw, int chp)
{
  int bad = 0;
  if (!r) return 0;
  if (r->w <= 0 || r->w > cw){ r->w = cw; bad |= 1; }
  if (r->h <= 0 || r->h > chp){ r->h = chp; bad |= 1; }
  if (r->x < 0 || r->x + r->w > cw){ r->x = 0; r->w = cw; bad |= 1; }
  if (r->y < 0 || r->y + r->h > chp){ r->y = 0; r->h = chp; bad |= 1; }
  if (r->sw <= 0) { r->sw = r->w; bad |= 4; }
  if (r->sh <= 0) { r->sh = r->h; bad |= 4; }
  if (r->sx < 0)  { r->sx = r->x; bad |= 4; }
  if (r->sy < 0)  { r->sy = r->y; bad |= 4; }
  if (r->sx < r->x) r->sx = r->x;
  if (r->sy < r->y) r->sy = r->y;
  if (r->sx + r->sw > r->x + r->w){ r->sw = r->x + r->w - r->sx; bad |= 4; }
  if (r->sy + r->sh > r->y + r->h){ r->sh = r->y + r->h - r->sy; bad |= 4; }
  if (r->sw < 8){ r->sw = 8; bad |= 4; }
  if (r->sh < 8){ r->sh = 8; bad |= 4; }
  return bad;
}
/* 1 iff the safe area really is inside the (visible) frame.  This is the
 * invariant the phone checks; the bench asserts it at EVERY scale. */
static int va_in_frame(const va_rect *r)
{
  return r && r->sw > 0 && r->sh > 0 &&
         r->sx >= r->x && r->sy >= r->y &&
         r->sx + r->sw <= r->x + r->w &&
         r->sy + r->sh <= r->y + r->h;
}

/* Class-safe boxes for the THREE cluster layouts, DERIVED from the classifier
 * rather than invented: each box sits well inside the class it must land in, with
 * margin against the mm error that moves the cliffs.  POSITION is still a
 * placeholder -- the CLASS is deliberate, the COORDINATES must be measured against
 * the car's own clear window.
 * Override any box with MIB3_SHIM_VA<i>="x,y,w,h,sx,sy,sw,sh".
 *
 * REFERENCE MODEL (omonob/MHI2-Carplay-Maps, card-core navigation_card_scene.h; a
 * DIFFERENT platform family -- MHI2/QNX, AID 10.5, 1010x376 -- so the numbers are
 * not ours, the MODEL is).  Their golden SAFE rects:
 *   idle / pure map, not navigating : (202,16,606,344)
 *   pure map, CarPlay OWNS navigation: (42,16,606,344)   <-- the map RE-CENTRES
 *   left card                       : (18,75,606,284)     when nav ownership
 *   right card                      : (386,75,606,284)    flips; the OUTER view
 *   compact centred card            : (325,75,360,284)    area stays full-panel
 * i.e. the outer view area stays the full panel (our shape, confirmed), the nested
 * safe rect differs per layout, and the phone composes the MAP at a DIFFERENT
 * offset depending on whether CarPlay owns navigation -- the leading explanation
 * for "map off-centre from the actual vehicle location", driven from our side of
 * the wire by MIB3_SHIM_URL. */
static void va_preset_rect(int i, int cw, int chp, va_rect *r)
{
  geom_t g;
  int wt;
  cluster_geom(&g);
  wt = va_wide_px(cw, g.phy_w);
  /* The visible frame stays the FULL panel: reducing it has got the 111 stream
   * rejected at activation. */
  r->x = 0; r->y = 0; r->w = cw; r->h = chp;
  if (i == 0){
    /* full-map layout: WIDE, centred, generous margins */
    int sw = cw * 5 / 8, sh = chp * 7 / 12;
    if (sw < wt + wt / 3) sw = wt + wt / 3;
    if (sh < chp * 242 / 480) sh = chp * 242 / 480;   /* ETA floor + margin, scaled */
    if (sw > cw) sw = cw;
    if (sh > chp) sh = chp;
    r->sx = (cw - sw) / 2; r->sy = (chp - sh) / 2; r->sw = sw; r->sh = sh;
  } else if (i == 1){
    /* THE TUNED CARD: locked at 439,50,397,375 on 1280x480.
     *
     * Expressed as FRACTIONS of the panel so the same card lands in the same
     * place, with the same class, at ANY pixel size; at 1280x480 they evaluate to
     * the locked values exactly (1280*397/1280 = 397, 480*375/480 = 375), so the
     * 1x advert is byte-identical to the shipped one.
     *
     * The CLASS survives because the classifier's thresholds are a fixed FRACTION
     * of the width while the advertised mm is unchanged (wideThrPx = 34.6 % at
     * 292 mm) and this card is 31 % of the width -> "centred" at every scale.
     * Measured, desk (container, unit CFLite): 1x -> 439,50,397,375;
     * 1.5x -> 658,75,595,562; 2x -> 878,100,794,750; 2x+SCALE_PHY -> same px.
     * The old branch produced 883,200,793,855 at 2x (95 px below the frame).
     *
     * The laws stay relative to the frame: bottom margin = chp-(sy+sh) =
     * 55*chp/480, the top edge follows sh, and the 300 px width class at 1x scales
     * with the panel at 2x so the type keeps its size. */
    r->sw = cw * 397 / 1280;
    r->sh = chp * 375 / 480;
    r->sx = cw * 439 / 1280;
    r->sy = chp * 50  / 480;
    /* keep the platter at/above its size class, proportionally */
    if (r->sw < cw * 165 / 1280) r->sw = cw * 165 / 1280;
    if (r->sh < chp * 253 / 480) r->sh = chp * 253 / 480;
  } else {
    /* third layout (unmeasured): WIDE with a real margin, offset left so the
     * wide-side branch is exercised. */
    int sw = cw * 47 / 100, sh = chp * 62 / 100;
    if (sw < wt + wt / 3) sw = wt + wt / 3;
    if (sh < chp * 242 / 480) sh = chp * 242 / 480;
    if (sw > cw) sw = cw;
    if (sh > chp) sh = chp;
    r->sx = cw / 20; r->sy = (chp - sh) / 2; r->sw = sw; r->sh = sh;
  }
}
/* set by va_rect_at when it had to clamp, read (and cleared) by va_build so the
 * warning can name the area index. */
static int g_va_fit_bad = 0;
/* Fill rect i (1-based env name suffix: area 0 -> MIB3_SHIM_VA0). */
static int va_rect_at(int i, int cw, int chp, va_rect *r)
{
  char nm[24]; int j = 0; const char *p = "MIB3_SHIM_VA";
  int got = 0;
  g_va_fit_bad = 0;
  while (*p && j < (int)sizeof(nm) - 3) nm[j++] = *p++;
  nm[j++] = (char)('0' + (i % 10)); nm[j] = 0;
  { const char *s = getenv(nm); if (s && s[0] && va_parse_rect(s, r)) got = 1; }
  if (!got){
    if (env_on("MIB3_SHIM_VA_PRESET",1)){
      va_preset_rect(i, cw, chp, r);
      got = 1;
    } else if (i == 0){                              /* area 0 = the full panel */
      r->x = 0; r->y = 0; r->w = cw; r->h = chp;
      r->sx = 0; r->sy = 0; r->sw = cw; r->sh = chp;
      got = 1;
    } else {
      va_rect g; va_parse_rect(VA_GAUGE_SAFE, &g);   /* provisional default */
      r->x = 0; r->y = 0; r->w = cw; r->h = chp;
      r->sx = g.sx; r->sy = g.sy; r->sw = g.sw; r->sh = g.sh;
    }
  }
  /* EVERY path ends here: a box that leaves the frame is invalid advert, so it
   * is pulled inside and the event is logged (never shipped silently). */
  g_va_fit_bad = va_fit(r, cw, chp);
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
/* one "visible=x,y,w,h safe=x,y,w,h" line, the SAFE-AREA-IN-FRAME verdict, then
 * the CLASSIFIER verdict for this box (0 = blank cluster), so the car tells us
 * what it will draw instead of us guessing from the picture.  The rect prints
 * SIGNED numbers: clamping negatives to 0 is how a safe area ending 95 px below
 * the frame still read as a plausible "safe=883,200,793,855" for two sessions. */
static void va_put_int(char *b, int *k, int x)
{
  char d[12]; int m = 0;
  if (x < 0){ b[(*k)++] = '-'; x = -x; }
  if (!x) d[m++] = '0';
  while (x){ d[m++] = (char)('0' + (x % 10)); x /= 10; }
  while (m) b[(*k)++] = d[--m];
}
static void va_log_rect(int i, const va_rect *r, int cw)
{
  char b[160]; int k = 0; int v[8]; int q, j;
  v[0]=r->x; v[1]=r->y; v[2]=r->w; v[3]=r->h;
  v[4]=r->sx; v[5]=r->sy; v[6]=r->sw; v[7]=r->sh;
  for (q = 0; q < 2; q++){
    const char *t = q ? " safe=" : "visible=";
    while (*t) b[k++] = *t++;
    for (j = 0; j < 4; j++){
      if (j) b[k++] = ',';
      va_put_int(b, &k, v[q*4 + j]);
    }
  }
  b[k] = 0;
  logkv("cluster/va","area index",(u64)(u32)i,0);
  logline("cluster/va/rect", b);
  /* THE invariant the phone checks: is the safe area inside the frame? */
  logkv("cluster/va","  safefit(1=inside)",(u64)(unsigned)va_in_frame(r),0);
  /* the phone's own verdict on this box, computed BEFORE the session so the car
   * tells us what it will draw instead of us guessing from the picture */
  { geom_t g; int cls;
    cluster_geom(&g);
    cls = va_class_of(r->sw, r->sh, cw, g.phy_w);
    logkv("cluster/va","class (0=BLANK 1=centred 2=wide)",(u64)(u32)cls,0);
    logkv("cluster/va","  safeW",(u64)(u32)r->sw,0);
    logkv("cluster/va","  safeH",(u64)(u32)r->sh,0);
    logkv("cluster/va","  wideThrPx",(u64)(u32)va_wide_px(cw, g.phy_w),0);
    logkv("cluster/va","  centreThrPx(safeH)",(u64)(u32)va_centre_px(cw, g.phy_w),0);
    logkv("cluster/va","  etaOk(safeH>202)",(u64)(unsigned)(r->sh > va_eta_px(cw, g.phy_w)),0);
    logkv("cluster/va","  nearCliff(<=10%)",(u64)(unsigned)va_class_near_cliff(r->sw, r->sh, cw, g.phy_w),0); }
}

/* The multi-area `viewAreas` array.  Returns +1 owned by the caller.  Logs one
 * `cluster/va/area` line per declared area (new lines only -- the existing
 * "viewAreas added" meaning is untouched). */
static CFMutableArrayRef va_build(int cw, int chp)
{
  fn_CFArrayCreateMutable mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
  CFMutableArrayRef va;
  int n = va_count(), i;
  int clamped = 0, classes[8], nclass = 0;
  if (!mka || !p_kCFTypeArrayCallBacks || !p_CFArrayAppendValue) return 0;
  va = mka(0, n, p_kCFTypeArrayCallBacks);
  if (!va) return 0;
  for (i = 0; i < n; i++){
    va_rect r;
    CFMutableDictionaryRef a;
    if (!va_rect_at(i, cw, chp, &r)) continue;
    /* MIB3_SHIM_VA_VISIBLE=1: use the safe rect as the visible frame too (a
     * shape the phone rejects at activation -- a deliberate trip test). */
    if (i > 0 && env_on("MIB3_SHIM_VA_VISIBLE",0)){
      r.x = r.sx; r.y = r.sy; r.w = r.sw; r.h = r.sh;
    }
    a = va_build_one(&r);
    if (!a) continue;
    p_CFArrayAppendValue(va, a);       /* array retains */
    if (p_CFRelease) p_CFRelease(a);
    va_log_rect(i, &r, cw);
    if (g_va_fit_bad){
      clamped++;
      logkv("cluster/va","  WARN clamped (1=visible 4=safe)",(u64)(u32)g_va_fit_bad,0);
    }
    if (nclass < 8){
      geom_t g; cluster_geom(&g);
      classes[nclass++] = va_class_of(r.sw, r.sh, cw, g.phy_w);
    }
  }
  logkv("cluster/va","count declared",(u64)(unsigned)p_CFArrayGetCount(va),0);
  /* ONE summary line per advert: the two numbers the desk bench asserts at every
   * scale -- boxes clamped (must be 0: an out-of-frame safe area is an invalid
   * advert) and classes (must be the SAME sequence at every scale while the
   * advertised mm is unchanged). */
  { char b[80]; int k = 0, j;
    logkv("cluster/va","out-of-frame boxes clamped",(u64)(u32)clamped,0);
    for (j = 0; j < nclass; j++){
      if (j) b[k++] = ',';
      b[k++] = (char)('0' + (classes[j] & 7));
    }
    b[k] = 0;
    logline("cluster/va/classes", b); }
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
   * MIB3_SHIM_NOURL=1 omits it (the phone accepts a dict without one). */
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
      int n = 0, iv;
      dseto(d, "viewAreas", va2);
      /* WHICH AREA A SESSION STARTS IN.  Default 1 = the centred card with the
       * ETA underneath, verified on the car.
       * Area 0 is the wide class (a 213 px column at the box edge); switch with
       * viewarea.sh 0 to get that.  The index is CLAMPED to what was actually
       * declared, so a short VA_COUNT can never declare an out-of-range start. */
      if (p_CFArrayGetCount) n = (int)p_CFArrayGetCount(va2);
      iv = (int)env_num("MIB3_SHIM_VA_INITIAL", 1);
      if (iv < 0 || (n > 0 && iv >= n)) iv = 0;
      dseti(d, "initialViewArea", (s64)iv);
      g_va_current = iv;                /* keep the switch state consistent */
      logkv("cluster/va","initialViewArea",(u64)(u32)iv,0);
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
       * shim owned them and never released them -> leak on every cluster ad. */
      if (p_CFRelease){ if (safe) p_CFRelease(safe); if (area) p_CFRelease(area); }
      if (p_CFRelease) p_CFRelease(va);          /* dict retains it */
    }
  }
  if (env_on("MIB3_SHIM_CFSHOW",0) && p_CFShow) { logline("cluster","CFShow(dict) ->"); p_CFShow(d); }
}

/* ==========================================================================
 * /info COMPLETION -- the MODERN CarKit display schema.
 *
 * THE GAP.  AirPlayCopyServerInfo() copies `displays` (and `extendedFeatures`,
 * `audioFormats`, ...) out of the platform dict into a CFMutableDictionary, and
 * the /info HTTP handler (libAirPlay 0xcc98) serialises that SAME dict with
 * CFBinaryPlistV0Create.  LG fills `displays` via AirPlayInfoArrayAddScreenDisplay
 * with the OLD 210 schema (identifier key `uuid`, no `initialVideoStreams`);
 * iOS-26-era CarKit (CRDisplayInfo / CARSessionConfiguration) requires the NEW
 * schema at the display-dict level plus a separate top-level `displayPanels`, and
 * a `displays` entry it cannot parse makes it reject the WHOLE array and
 * teardown-loop the session.
 *
 * Required fields (verified against the iOS 26.1 CarKit decompile):
 *   per display :  uid (missing -> CRDisplayInfo nil -> array rejected), uuid
 *                  (AirPlaySender picks 110 vs 111 by a uuid-vs-display-0
 *                  compare), name, type, maxFPS, width/heightPixels,
 *                  width/heightPhysical, viewAreas, initialViewArea,
 *                  adjacentViewAreas, initialVideoStreams (a present, NON-empty
 *                  array or CRDisplayInfo returns nil), cornerMasks, nightMode,
 *                  limitedUI, limitedUIElements;
 *   cluster only:  properties = ["Instrument Cluster"]  (-> supportsInstrument-
 *                  ClusterContent);
 *   top level   :  displayPanels = the same [main,cluster] array (without it the
 *                  cluster is unrecognized and iOS never proposes altScreen), and
 *                  extendedFeatures = ["altScreen","viewAreas"] -- the /info
 *                  advertisement, a SEPARATE message from SETUP's
 *                  `enabledFeatures`.
 *
 * THE WIRING IS PROVEN FROM THE BINARY (R8830L, disassembly):
 *   1. CarPlayConfiguration::setDisplays (libCarPlayAppFramework 0x236e4) calls
 *      AirPlayInfoArrayAddScreenDisplay(&this[0x88], ...) -- the array our first
 *      seam appends to.
 *   2. CarPlayServerCallbacks::copyRequiredProperty(key=="displays", fw) (0x1db88)
 *      returns [fw+0x310]; CarPlayConfiguration is the sub-object at fw+0x288
 *      (proved by the sibling "audioFormats" case), so fw+0x310 == config[0x88].
 *   3. AirPlayCopyServerInfo (libAirPlay 0xe3d8-0xe3ec) calls
 *      AirPlayReceiverServerPlatformCopyProperty(server,1,"displays",..) -> (2)
 *      -> CFDictionarySetValue(info,"displays",thatArray).
 *   4. The /info handler serialises it, so this seam sees the SAME array.
 * Also: copyExtendedFeatures (0x1dc54) BUILDS `extendedFeatures` from LG flags
 * (`enhancedRequestCarUI` @[fw+0x300], `vocoderInfo` @[fw+0x3f8]), so the key is
 * often PRESENT without altScreen -- hence MERGE, never skip-if-present or clobber.
 *
 * HONEST SCOPE: the field SET is SUPPORTED (a working reference receiver verified
 * against the iOS 26.1 decompile), not PROVEN against this unit's libraries -- it
 * has no `uid`/`displayPanels`/`initialVideoStreams` strings at all.
 *
 * Everything is gated on the CLUSTER ENTRY ACTUALLY BEING PRESENT (type==111 in
 * `displays`): otherwise NEITHER displayPanels NOR extendedFeatures is published,
 * so the 2-display/altScreen contract can never be advertised without the second
 * display -- the same both-ways discipline as g_cluster_advertised.
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
 * advertisement idempotent PER DISPLAY LIST, not per process: LG rebuilds
 * `displays` on every session, so the old once-per-process latch meant only the
 * FIRST plug after injection advertised -- every later plug still claimed
 * altScreen while showing one display, the contradiction that kills the session. */
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
      /* cornerMasks goes on the wire as a BOOLEAN (false), not an int. */
      void *bf = p_kCFLBooleanFalse ? *(void**)p_kCFLBooleanFalse : 0;
      if (bf && p_CFDictionarySetValue) p_CFDictionarySetValue(d, g_k_corner, bf);
      else p_CFDictionarySetInt64(d, g_k_corner, 0);
    }
    if (!p_CFDictionaryGetValue(d, g_k_lui))    p_CFDictionarySetInt64(d, g_k_lui, 0);
    /* NIGHT: FILL, NEVER OVERWRITE.  An existing key is left exactly as the car
     * provided it -- this used to poke the key on BOTH entries and could
     * overwrite the unit's own value on the MAIN display.  The caller picks the
     * fill value; the MIRROR block below tracks the live mode on the cluster. */
    if (!p_CFDictionaryGetValue(d, g_k_night))
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
/* displayPanels: a panel per display with uid="{uuid}-panel", the display's
 * size, properties (showsInstruments for the cluster) and
 * initialVideoStreams=[{uuid}].  The previous code set displayPanels = the same
 * `displays` array. */
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
    while (j < sizeof("-panel") - 1 && k < 79){ pb[k++] = "-panel"[j++]; }  /* uid = uuid + "-panel" */
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

/* (night_resolve was deleted 2026-10-29: pass-through/mirror left it with no
 * caller -- info_complete mirrors the main display's nightMode object directly
 * and cluster_send_nightmode consults MIB3_SHIM_NIGHT itself.  The NIGHT MODE
 * IS FOLLOW, NEVER ENFORCE contract lives in those two places.) */

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

  /* PER-DISPLAY COMPLETION.  The night-mirror rework once dropped these two
   * calls, so every /info went out without the per-display schema (uid,
   * name/properties on the cluster, cornerMasks, limitedUI, nightMode fill,
   * initialVideoStreams, adjacentViewAreas) -- the session that reported "white
   * frame on connect" and "map off-centre", while every working session ran WITH
   * them.
   * They run FIRST and only FILL missing keys; the mirror block below then
   * overwrites the cluster's nightMode with the main entry's current value object,
   * so the mirror wins on that one key.  MIB3_SHIM_NIGHT never reaches /info (it
   * gates only the debug setNightMode command). */
  { s64 night = main ? dict_i64((CFDictionaryRef)main, g_k_night) : 0;
    g_night_mode = night;   /* the value the debug-force path sends as setNightMode */
    if (main && env_on("MIB3_SHIM_MAINMOD",1)) info_complete_entry(main, 0, night);
    info_complete_entry(cluster, 1, night); }

  { /* NIGHT = MIRROR THE CAR'S MAIN DISPLAY ONTO THE CLUSTER.  Fill-only was wrong:
     * the car's /info carries nightMode on the MAIN display and it CHANGES
     * mid-drive (observed num=0 early, bool=true later), so an existing key left
     * the cluster frozen at DAY for the whole drive.  Copy the main entry's VALUE
     * OBJECT onto the cluster on every build -- preserving the car's own
     * representation (num vs bool) -- and never touch main.  Pass-through of the
     * car's state, not a mode we invent: no setNightMode is shipped from here. */
    CFTypeRef mnv = main ? p_CFDictionaryGetValue(main, g_k_night) : 0;
    g_night_mode = (s64)dict_i64(main ? (CFDictionaryRef)main : 0, g_k_night);
    if (mnv && p_CFDictionarySetValue) {
      p_CFDictionarySetValue(cluster, g_k_night, mnv);
      logkv("info", "cluster nightMode mirrored from main", 1, 0);
    } else if (p_CFDictionarySetInt64 && !p_CFDictionaryGetValue(cluster, g_k_night)) {
      p_CFDictionarySetInt64(cluster, g_k_night, 0);
      logline("info", "cluster nightMode: main had none -- defaulted 0 (day)");
    }
  }

  /* top-level keys, published only when the cluster entry exists (above).
   * displayPanels: set unconditionally -- a panel per display with
   * uid="{uuid}-panel", the display's size, properties (showsInstruments for the
   * cluster) and initialVideoStreams=[{uuid}].  Nothing in the unit provides this
   * key at all; MIB3_SHIM_PANELS=0 restores displayPanels = the same displays
   * array.
   * extendedFeatures: MERGE -- never skip-if-present (altScreen could be left out)
   * and never clobber (the unit's libAirPlay BUILDS this key from LG flags, so a
   * real feature could be dropped).  Append our two strings iff missing. */
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
      if (env_on("MIB3_SHIM_EXTFEAT",1)){    /* /info advertises the pair; the +0x3E gate is SETUP's */
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
 * THE ADVERT SELF-CHECK -- is the dict we are about to serialise internally
 * consistent?
 *
 * One malformed `displays` entry costs the WHOLE CarPlay session, so "the phone
 * refused 2x" and "our advert was broken" are indistinguishable without this (at
 * 2x a safe area ended 95 px below the frame, invisibly, for two sessions).  It
 * asserts, from that dict alone:
 *   - exactly one type-111 entry, and its pixel/physical size;
 *   - every declared area's safeArea INSIDE its visible frame, initialViewArea in
 *     range;
 *   - initialVideoStreams present and NON-empty (an empty array is a 0 s session);
 *   - displayPanels: one panel per display with the SAME pixel and physical
 *     geometry, and `showsInstruments` on the cluster panel;
 *   - extendedFeatures carries altScreen + viewAreas, and altScreen only when
 *     >= 2 displays exist (the both-ways contract).
 *
 * Runs on every /info (MIB3_SHIM_INFOCHECK=0 disables) and is exported for the
 * desk bench, so the SAME code judges the advert on the desk and on the car.
 * Log tag `infocheck`; `mismatch=0` is the only good value.
 * ========================================================================*/
static int ic_in_frame(CFTypeRef area, CFTypeRef *safeOut)
{
  CFTypeRef s;
  int vx, vy, vw, vh, sx = 0, sy = 0, sw = 0, sh = 0;
  if (!is_dict(area)) return -1;
  vx = (int)dgeti64k((CFDictionaryRef)area, "originXPixels", 0);
  vy = (int)dgeti64k((CFDictionaryRef)area, "originYPixels", 0);
  vw = (int)dgeti64k((CFDictionaryRef)area, "widthPixels", 0);
  vh = (int)dgeti64k((CFDictionaryRef)area, "heightPixels", 0);
  s  = dgetk((CFDictionaryRef)area, "safeArea");
  if (safeOut) *safeOut = s;
  if (!is_dict(s)) return -1;
  sx = (int)dgeti64k((CFDictionaryRef)s, "originXPixels", 0);
  sy = (int)dgeti64k((CFDictionaryRef)s, "originYPixels", 0);
  sw = (int)dgeti64k((CFDictionaryRef)s, "widthPixels", 0);
  sh = (int)dgeti64k((CFDictionaryRef)s, "heightPixels", 0);
  if (vw <= 0 || vh <= 0 || sw <= 0 || sh <= 0) return 0;
  return (sx >= vx && sy >= vy && sx + sw <= vx + vw && sy + sh <= vy + vh) ? 1 : 0;
}
static int ic_arr_has(CFTypeRef arr, const char *want)
{
  CFIndex i, n;
  if (!is_array(arr) || !p_CFStringGetCString) return 0;
  n = p_CFArrayGetCount((CFArrayRef)arr);
  for (i = 0; i < n; i++){
    CFTypeRef s = p_CFArrayGetValueAtIndex((CFArrayRef)arr, i);
    char tmp[64]; int k = 0;
    if (!s) continue;
    tmp[0] = 0;
    if (!p_CFStringGetCString((CFStringRef)s, tmp, sizeof(tmp), K128)) continue;
    while (want[k] && tmp[k] && want[k] == tmp[k]) k++;
    if (want[k] == 0 && tmp[k] == 0) return 1;
  }
  return 0;
}
static int info_check(CFMutableDictionaryRef info)
{
  CFTypeRef dv, cl = 0, va, pa, ef;
  CFIndex n = 0, i;
  int cli = -1, areas = 0, inside = 0, ix = -1, ivs = 0;
  int ctype = 0, cw = 0, ch = 0, cpw = 0, cph = 0;
  int panels = 0, wm = 0, hm = 0, pm = 0, mk = 0;
  int alt = 0, vaf = 0, mismatch = 0;
  info_keys_init();
  if (!g_info_keys_ready) return 0;
  if (!info){ logline("infocheck","no dict"); return 0; }
  dv = dgetk(info, "displays");
  if (!is_array(dv)){ logline("infocheck","no displays array"); return 0; }
  n = p_CFArrayGetCount((CFArrayRef)dv);
  for (i = 0; i < n; i++){
    CFTypeRef e = p_CFArrayGetValueAtIndex((CFArrayRef)dv, i);
    if (!is_dict(e)) continue;
    if ((int)dict_i64((CFDictionaryRef)e, g_k_type) == MIB3_CLUSTER_TYPE){ cl = e; cli = (int)i; break; }
  }
  if (!cl){
    logline("infocheck","no cluster entry -- nothing to check (publishes nothing)");
    return 0;
  }
  ctype = (int)dict_i64((CFDictionaryRef)cl, g_k_type);
  cw  = (int)dgeti64k((CFDictionaryRef)cl, "widthPixels", 0);
  ch  = (int)dgeti64k((CFDictionaryRef)cl, "heightPixels", 0);
  cpw = (int)dgeti64k((CFDictionaryRef)cl, "widthPhysical", 0);
  cph = (int)dgeti64k((CFDictionaryRef)cl, "heightPhysical", 0);
  /* initialVideoStreams: present and NON-empty */
  { CFTypeRef v = dgetk((CFDictionaryRef)cl, "initialVideoStreams");
    if (is_array(v)) ivs = (int)p_CFArrayGetCount((CFArrayRef)v); }
  if (ivs < 1) mismatch++;
  /* view areas: count, every safeArea inside its frame, initial index in range */
  va = dgetk((CFDictionaryRef)cl, "viewAreas");
  if (is_array(va)){
    areas = (int)p_CFArrayGetCount((CFArrayRef)va);
    for (i = 0; i < areas; i++){
      CFTypeRef a = p_CFArrayGetValueAtIndex((CFArrayRef)va, i);
      if (ic_in_frame(a, 0) == 1) inside++;
    }
  }
  ix = (int)dgeti64k((CFDictionaryRef)cl, "initialViewArea", 0);
  if (areas < 1 || inside != areas) mismatch++;
  if (ix < 0 || (areas > 0 && ix >= areas)) mismatch++;
  /* displayPanels: one per display, same geometry as the display, cluster marked */
  pa = dgetk(info, "displayPanels");
  if (is_array(pa)){
    panels = (int)p_CFArrayGetCount((CFArrayRef)pa);
    for (i = 0; i < panels && i < n; i++){
      CFTypeRef p = p_CFArrayGetValueAtIndex((CFArrayRef)pa, i);
      CFTypeRef d = p_CFArrayGetValueAtIndex((CFArrayRef)dv, i);
      CFTypeRef pr;
      if (!is_dict(p) || !is_dict(d)) continue;
      if ((int)dgeti64k((CFDictionaryRef)p, "widthPixels", -1)  == (int)dgeti64k((CFDictionaryRef)d, "widthPixels", -2))  wm++;
      if ((int)dgeti64k((CFDictionaryRef)p, "heightPixels", -1) == (int)dgeti64k((CFDictionaryRef)d, "heightPixels", -2)) hm++;
      if ((int)dgeti64k((CFDictionaryRef)p, "widthPhysical", -1) == (int)dgeti64k((CFDictionaryRef)d, "widthPhysical", -2) &&
          (int)dgeti64k((CFDictionaryRef)p, "heightPhysical", -1) == (int)dgeti64k((CFDictionaryRef)d, "heightPhysical", -2)) pm++;
      pr = dgetk((CFDictionaryRef)p, "properties");
      if (ic_arr_has(pr, "showsInstruments")) mk++;
    }
    if (panels != (int)n) mismatch++;
    if (wm != panels || hm != panels || pm != panels) mismatch++;
  } else mismatch++;
  /* features: both-ways contract with the display count */
  ef = dgetk(info, "extendedFeatures");
  alt = ic_arr_has(ef, "altScreen");
  vaf = ic_arr_has(ef, "viewAreas");
  if (!alt || !vaf) mismatch++;
  if (alt && (int)n < 2) mismatch++;
  (void)ctype;
  logkv("infocheck","displays",(u64)(unsigned)n,0);
  logkv("infocheck","cluster index",(u64)(unsigned)cli,0);
  logkv("infocheck","cluster type",(u64)(unsigned)ctype,0);
  logkv("infocheck","cluster w",(u64)(unsigned)cw,0);
  logkv("infocheck","cluster h",(u64)(unsigned)ch,0);
  logkv("infocheck","cluster wPhys",(u64)(unsigned)cpw,0);
  logkv("infocheck","cluster hPhys",(u64)(unsigned)cph,0);
  logkv("infocheck","cluster initialVideoStreams",(u64)(unsigned)ivs,0);
  logkv("infocheck","viewAreas declared",(u64)(unsigned)areas,0);
  logkv("infocheck","viewAreas inside frame",(u64)(unsigned)inside,0);
  logkv("infocheck","initialViewArea",(u64)(unsigned)ix,0);
  logkv("infocheck","panels",(u64)(unsigned)panels,0);
  logkv("infocheck","panels size match",(u64)(unsigned)wm,0);
  logkv("infocheck","panels phys match",(u64)(unsigned)pm,0);
  logkv("infocheck","panels showsInstruments",(u64)(unsigned)mk,0);
  logkv("infocheck","extFeatures altScreen",(u64)(unsigned)alt,0);
  logkv("infocheck","extFeatures viewAreas",(u64)(unsigned)vaf,0);
  logkv("infocheck","VERDICT mismatch",(u64)(unsigned)mismatch,0);
  return mismatch == 0;
}


/* ==========================================================================
 * Cluster (type 111) STREAM handling.
 *
 * The unit's SETUP dispatcher (inside AirPlayReceiverSessionSetup) handles types
 * 100/101/102/110 and DROPS everything else at its `sub w1,w0,#0x64; cmp w1,#0xa;
 * b.hi` guard (only indices 0..10 are in the jump table).  Worse, the session
 * holds exactly ONE screen: the 110 case uses session+0x1af0 (screen), +0x1ae8
 * (screenInitialized), +0x1af8 (listen sock), +0x1b00 (net sock) and a single
 * _ScreenThread -- and it asserts if a second 110 arrives -- so 111 cannot simply
 * be spoofed as 110.
 *
 * The shim therefore owns a SECOND screen object and reproduces the unit's own
 * _ScreenSetup + _ScreenThread sequence for it via the exported
 * AirPlayReceiverSessionScreen_* API.  Two helpers it needs are NOT exported
 * (chacha key derive; the two screen time-sync callbacks): those are called by RVA
 * off the libAirPlay base (train-specific, logged as such).
 * ========================================================================*/
static void sink_resolve(void);
static int  sink_start(void);                    /* deferred to cluster_thread */
static int  sink_feed(const void *data, unsigned long len, unsigned long ts);
int  mib3shim_helper_connected(void);

#define SCREEN_SOCK_QOS    0xc
#define TIMESYNC_CB1_RVA   0x13b84   /* FUN_00113b84  (unexported) */
#define TIMESYNC_CB2_RVA   0x13b48   /* FUN_00113b48  (unexported) */
#define CHACHA_DERIVE_RVA  0x163d8   /* FUN_001163d8  (unexported) */

/* ==========================================================================
 * COMMANDS TO THE PHONE (the post-connect sequence).
 *
 * Once connected the phone sends opcode-1 (avcC) + opcode-5 (telemetry) but ZERO
 * video until the CAR drives the scene: forceKeyFrame{params:{uuid}} at stream
 * start, then showUI{params:{uuid,url}} (+ setNightMode{params:{uuid,nightMode}})
 * -- then it renders the cluster scene (Apple Maps) and the video flows.  All
 * three carry exactly those keys, confirmed against two independent working
 * receivers.  Shipped via the stock AirPlayReceiverSessionSendCommand.  The uuid
 * MUST be the CLUSTER DISPLAY uuid we advertised.
 * ======================================================================== */
/* build {type:.., params:..}; params is consumed (retained by the cmd dict,
 * our +1 released here after SendCommand returns, per the stock pattern). */
/* ---- COMMAND COMPLETION ---------------------------------------------------
 * AirPlayReceiverSessionSendCommand(session, cmd, completion, ctx) takes the stock
 * handler for POST /command calls:
 *     completion(int status, CFDictionaryRef resp|NULL, void *ctx)
 * status 0 = the phone ACCEPTED/applied, non-zero = refused.  Passing (0,0) could
 * not tell "delivered" from "refused" -- the same blindness that hid the missing
 * "type" key.
 *
 * WHERE IT RUNS: libAirPlay's HTTP-client thread, AFTER SendCommand returns, and
 * it may outlive the session that issued it.  So it does NOTHING but compare the
 * generation it was issued under with g_cmd_gen and write one log line: no CF
 * object touched (resp is deliberately ignored), no lock, no display or socket
 * I/O -- it cannot deadlock the control queue or race a teardown.
 *
 * LIFETIME: `ctx` is one of a small ring of STATIC slots valid for the process
 * lifetime -- no malloc, nothing to free, no cross-thread ownership.  ship_cmd runs
 * on one thread at a time, so the round-robin is monotonic; a slot reused before
 * its reply arrives logs under the newer command's name, which is bounded.
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

/* The completion callback.  Return type is void; the stock pattern in
 * libAirPlay's ProcessFrames response path is a plain function pointer with this
 * exact 3-argument shape. */
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
  /* ---- GENERATION GUARD ---------------------------------------------------
   * AirPlayReceiverSessionSendCommand is a dispatch_sync_f onto the SESSION's own
   * control queue with NO timeout, that builds an RTSP POST /command and
   * HTTPClientSendMessage()s it (the shipped binary at VA 0x17f3c calls
   * dispatch_sync_f@plt).
   * It runs on the CLUSTER RECEIVER THREAD, so: (a) while it is in flight we are
   * not read()ing the phone's cluster socket, which then looks
   * silent-but-ESTABLISHED; and (b) `session` was captured at SETUP time, so a
   * superseded receiver thread could command an OLD session object.  Two
   * session-lifecycle defects of exactly this kind are what the guard prevents.
   * Only the CURRENT setup generation may command (g_cmd_session is
   * written by cluster_setup on every type-111 SETUP), and stock's own liveness
   * byte must be set (below).  MIB3_SHIM_CMDGEN=0 restores the old behaviour. */
  if (kn()->cmdgen){
    if (session != g_cmd_session){
      logline("cluster/cmd/skipped-superseded", type);
      return -2;
    }
    /* The 0x446 liveness byte is a REVERSE-ENGINEERED offset (session+0x446, set
     * once by AirPlayReceiverSessionStart and never cleared).  The disassembly is
     * a different BUILD from this unit, so a wrong offset here would silently
     * disable EVERY command (showUI / forceKeyFrame / nightMode /
     * changeMapZoomLevel) while everything still looks connected -- a
     * silent, trip-costing failure.  Hence its own knob:
     *   MIB3_SHIM_CMDALIVE=0  -> skip the byte check (the generation check above
     *                            still applies; that one is sound by construction).
     * Every trip should grep the log for these two skip lines. */
    if (kn()->cmdalive &&
        *(volatile signed char*)((char*)session + 0x446) == 0){
      logline("cluster/cmd/skipped-session-not-started", type);
      return -3;
    }
  }
  CFMutableDictionaryRef cmd = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
  if (!cmd) return -1;
  int rc = -1;
  {
    /* THE "type" KEY: this used to pass sstr(type) as the dictionary KEY, so
     * every command went out as {"<name>": "<name>", "params": {...}} -- with NO
     * "type" key -- while iOS dispatches on {"type": "<name>", "params": {...}}.
     * All four commands were therefore shipped in a shape the phone does not
     * recognise. */
    CFStringRef kt = sstr("type"), kp = sstr("params"), vt = sstr(type);
    if (kt && kp && vt && params){
      p_CFDictionarySetValue(cmd, kt, vt);
      p_CFDictionarySetValue(cmd, kp, params);
      /* COMPLETION: MIB3_SHIM_CMDACK=1 (default) passes a real callback so the
       * phone's verdict is visible in the log; =0 is the old call,
       * SendCommand(session, cmd, 0, 0). */
      if (kn()->cmdack){
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
  if (!kn()->cmds || !session) return;
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
  if (!kn()->cmds || !session) return;
  if (!g_cluster_uuid_str[0]){ logline("cluster/cmd","forceKeyFrame skipped (no cluster uuid)"); return; }
  CFMutableDictionaryRef p = cmd_params_uuid();
  if (!p) return;
  int rc = ship_cmd(session, "forceKeyFrame", p);
  logkv("cluster/cmd","forceKeyFrame ship rc (NOT phone action)",(u64)(u32)rc,0);
  logline("cluster/cmd/idr-why", why);
  if (p_CFRelease) p_CFRelease(p);
}
/* CLOCK, monotonic, milliseconds -- used by the recovery-point pump.
 * Prefers the libc symbol; falls back to syscall(2) so nothing here depends on a
 * libc header (this file is built -nostdlib).  If BOTH are unavailable the pump
 * cannot time anything, so say so once -- a silently disabled fix is the worst
 * outcome (that mistake has already cost a session: see MIB3_SHIM_CMDALIVE). */
typedef struct { long sec; long nsec; } mib3_ts;
static long (*p_clock_gettime)(int, void*);
static int  g_clock_warned = 0;
/* THE CLOCK MUST RESOLVE ITSELF.  mono_ms() used to depend on resolve() having run
 * to fill p_clock_gettime / p_syscall.  In carplay that is guaranteed -- SETUP
 * fires resolve() long before any AU exists -- but the AU-socket exports are also
 * driven STANDALONE (the audit's driver_helper, any direct consumer), and there
 * both pointers stayed NULL: mono_ms() returned 0 forever, send_all()'s deadline
 * (0 + 60) could never expire, and against a peer that never drains the EAGAIN
 * branch polled POLLOUT in an INFINITE 20 ms loop -- the stalled-receiver-thread
 * class this shim must make impossible.  So the clock resolves lazily here (dlsym
 * is thread-safe, this runs once) and send_all() additionally bounds its poll
 * iterations.  The NO CLOCK line stays: a silently-disabled timer is still the
 * worst outcome and must be visible. */
static u64 mono_ms(void)
{
  mib3_ts t;
  t.sec = 0; t.nsec = 0;
  if (!p_clock_gettime && !p_syscall){
    extern void *dlsym(void*, const char*);
    p_clock_gettime = (long(*)(int,void*)) dlsym(RTLD_DEFAULT, "clock_gettime");
    if (!p_clock_gettime)
      p_syscall = (long(*)(long,...)) dlsym(RTLD_DEFAULT, "syscall");
  }
  if (p_clock_gettime) p_clock_gettime(1 /*CLOCK_MONOTONIC*/, &t);
  else if (p_syscall)  p_syscall(113 /*__NR_clock_gettime*/, 1, &t);
  else if (!g_clock_warned) { g_clock_warned = 1; logline("cluster/cmd","NO CLOCK -- periodic recovery points DISABLED"); }
  return (u64)t.sec * 1000u + (u64)t.nsec / 1000000u;
}

/* ==========================================================================
 * PERIODIC RECOVERY POINTS -- the ingredient the cluster picture was missing
 * (2026-10-27, measured).
 *
 * Every H.264 stream this ECU sustains carries a recovery point at a fixed short
 * cadence, and the phone's does not:
 *
 *   stream                              IDRs                 cadence      held?
 *   the car's own nav (multi-screen-   17 of 81 AUs          every 5 AUs  yes
 *     server, captured 2026-10-26)                            (500 ms @10fps)
 *   the known-good test pattern        5 of 50 frames        every 12     yes (40 s)
 *     (pathb_base.264)                                        (1.2 s @10fps)
 *   the phone's live stream            2 of 2077 AUs         one at start NO  (0.5-1 s
 *     (w7.bin, 2026-10-27 field run)                          + our own      then the
 *                                                             start-request  placeholder)
 *
 * So the picture appeared and fell back not on framing, pacing, SPS, profile or
 * rate -- all measured and eliminated -- but because after ~0.5 s the ECU had no
 * keyframe to decode from.  The verb exists: forceKeyFrame{params:{uuid}} (rc=0
 * measured, answered in ~3 access units).  What was missing is the CADENCE -- it
 * used to fire once, at stream-start / first-IDR / helper-connect.
 *
 * MIB3_SHIM_IDR_MS sets the interval (default 500 ms == the car's own 10 fps /
 * 5-frame cadence; 0 disables).  A request goes only while video is flowing and
 * only while no IDR has arrived inside the interval.
 */
static u64  g_kf_seen_ms;      /* when the last IDR ARRIVED (0 = none yet) */
static u64  g_kf_req_ms;       /* when we last ASKED */
static u64  g_kf_reqs;         /* requests shipped */
static u64  g_au_last_ms;      /* when the last video AU ARRIVED */
static u64  g_au_fed;          /* VIDEO AUs handed to the sink (records are 2x this: cfg + frame) */
static int  g_gap_resume;      /* set when AU flow resumes after a pause (scene change) */
static u64  g_flag_changes;    /* opcode-1 flag transitions this process (diagnostic only) */

/* DELAYED RE-ACTIVATION (2026-11-02, field: "no location circle until guidance").
 * The scene activation (showUI) is sent at stream-connected AND at first-IDR --
 * about 100 ms apart -- and the phone ACKS both (rc=0).  But the cluster scene
 * then renders without the vehicle marker until something forces a redraw
 * (starting and stopping guidance does; so does quitting Maps, which redraws
 * BOTH screens).  The reference project hit the identical thing and fixed it the
 * same way: their note records that activation sent before the screen reports
 * READY is accepted but does not fully draw, and they re-post it after a delay.
 * So send one more, once, ~1.8 s later -- after the scene is really up.
 * `MIB3_SHIM_SHOWUI_MS=0` disables it; the banner prints the value. */
static u64 g_showui_retry_ms = 0;

static void cluster_showui_retry(void *session)
{
  if (!session || !g_showui_retry_ms) return;
  { u64 now = mono_ms();
    if (now < g_showui_retry_ms) return;
    g_showui_retry_ms = 0;
    cluster_send_showui(session, "delayed-activation"); }
}

static void cluster_idr_pump(void *session)
{
  if (!kn()->cmds || !session) return;
  /* before every gate below: a paused phone must not swallow the re-activation */
  cluster_showui_retry(session);
  if (!g_kf_seen_ms) return;                       /* the start sequence owns the first one */
  /* THE PUMP IS LOAD-BEARING -- DO NOT REMOVE IT AGAIN (2026-10-30, field).
   * Removing this timer produced the pre-2026-10-27 defect on the glass: the
   * cluster rendered the first frame and then NOTHING but one frame when a scene
   * edge happened to fire a keyframe (measured in the broken build: periodic=0,
   * scene-edge=2 -> first frame + one update at guidance end).  The ECU's map app
   * advances on recovery points; every stream it has ever decoded carries one
   * every ~5 access units.  Keep 500 ms.  The real cost of this pump is the
   * phone's 65 KB IDRs -- a phone-side encoder property to investigate separately,
   * not a reason to starve the receiver of the only signal it presents on. */
  int iv = kn()->idr_ms;
  if (iv <= 0) return;
  u64 now = mono_ms();
  /* the phone is quiet when the map is static: asking then is pointless (there
   * is no picture to refresh) and would only churn the command channel. */
  if (g_au_last_ms && (now - g_au_last_ms) > 2000) return;
  if ((now - g_kf_seen_ms) < (u64)iv) return;      /* a recovery point is still fresh */
  if (g_kf_req_ms && (now - g_kf_req_ms) < (u64)iv) return;  /* one request per interval */
  g_kf_req_ms = now;
  g_kf_reqs++;
  cluster_send_forcekeyframe(session, "periodic-recovery-point");
}

/* Poll the stream socket for at most `wait_ms`, then let the recovery-point
 * pump run.  Without this the receiver thread blocks in read() for as long as
 * the phone is quiet and NOTHING can be requested exactly when a fresh
 * recovery point is most needed.  Returns 1 when the socket is readable. */
static int cluster_wait_frame(int fd, void *session, int wait_ms)
{
  struct pollfd { int fd; short events; short revents; } pfd;
  extern int poll(void*, unsigned long, int);
  int pr;
  pfd.fd = fd; pfd.events = 1 /*POLLIN*/; pfd.revents = 0;
  pr = poll(&pfd, 1, wait_ms);
  helper_reattach_pump();            /* static-map reattach: see the note at the pump */
  cluster_idr_pump(session);
  return pr > 0;
}

static void cluster_send_nightmode(void *session)
{
  if (!kn()->cmds || !session) return;
  /* PASS-THROUGH NIGHT.  DEFAULT: this function does NOTHING.  Night is not
   * ours to decide and not ours to command: the phone renders the map pixels in
   * its own mode, and /info is filled-only (never overwritten).  The only way a
   * setNightMode is ever shipped is the explicit debug force
   * MIB3_SHIM_NIGHT=0|1 -- there is no in-band phone signal today (the opcode-1
   * flags byte chatters 2,484x per session and is NOT a mode signal; wiring it
   * caused 198 sync commands and the worst lag measured). */
  if (!kn()->night_forced) return;
  if (!g_cluster_uuid_str[0]) return;
  CFMutableDictionaryRef p = cmd_params_uuid();
  if (!p) return;
  dseti(p, "nightMode", (s64)(g_night_mode ? 1 : 0));
  int rc = ship_cmd(session, "setNightMode", p);
  g_night_applied = g_night_mode;
  logkv("cluster/cmd","setNightMode rc",(u64)(u32)rc,0);
  if (p_CFRelease) p_CFRelease(p);
}

/* CONFIG-FLAG OBSERVATION -- LOG ONLY, NEVER A SIGNAL.
 * The opcode-1 screen-state byte was hypothesised to carry day/night and does NOT:
 * it flips thousands of times per session (22<->86 in a measured run).  Wiring it
 * into the theme produced 198 synchronous setNightMode commands on the video thread
 * -- the worst lag this project has measured.  Diagnostic only, therefore: a
 * rate-limited delta line (at most one per 5 s, plus a per-session counter at
 * stream close).  Nothing in this shim consumes the value. */
static void night_flag_update(unsigned char flags, const char *tag)
{
  static signed char prev = -1;
  static u64 last_log = 0;
  if (prev != (signed char)flags){
    u64 now = mono_ms();
    g_flag_changes++;
    prev = (signed char)flags;
    if (last_log == 0 || now - last_log >= 5000){
      last_log = now;
      logkv(tag, "config flags (observed only, NOT a mode signal)", (u64)flags, 0);
      logkv(tag, "  flag changes so far", g_flag_changes, 0);
    }
  }
}

/* ==========================================================================
 * UPDATE VIEW AREA -- the command that re-frames the cluster scene.
 *
 * The VIEW button changes the layout the ECU paints, but nothing tells the phone,
 * so Apple Maps keeps rendering into the whole panel.  This ships:
 *
 *   {type:"updateViewArea",
 *    params:{uuid, viewAreaIndex, animationDurationMillis, adjacentViewAreas}}
 *
 * `adjacentViewAreas` MUST ALWAYS BE PRESENT (verified against the stock
 * AirPlayReceiverSessionViewAreaUpdate): [] when the FULL-screen area is current,
 * [0] when a smaller area is.  iOS keeps CARScreenInfo.adjacentViewArea SEPARATE
 * from currentViewArea, so omitting the key differs from sending an empty array.
 *
 * The phone bounds-checks viewAreaIndex against the viewAreas declared in /info
 * (carEndpoint_validateViewAreaParams: "viewAreaIndex < viewAreas.count"), so
 * va_build() must land first.
 *
 * GATES: MIB3_SHIM_VIEWAREAS=1 AND MIB3_SHIM_CMDS=1 AND MIB3_SHIM_VA_CMD=1.  A
 * refused switch is a LOGGED NO-OP: g_va_current moves only on a status-0
 * completion, so a failure cannot strand the display in a state we believe in.
 * ========================================================================*/
static int cluster_send_viewarea(void *session, int index, int dur_ms, const char *why)
{
  CFMutableDictionaryRef p;
  fn_CFArrayCreateMutable mka;
  CFMutableArrayRef adj;
  int rc;
  if (!session || !kn()->cmds) return -1;
  if (!kn()->viewareas){
    logline("cluster/va/cmd","skipped (MIB3_SHIM_VIEWAREAS=0)");
    return -1;
  }
  if (!kn()->va_cmd){
    logline("cluster/va/cmd","skipped (MIB3_SHIM_VA_CMD=0)");
    return -1;
  }
  if (index < 0 || index >= kn()->va_count){
    logkv("cluster/va/cmd","index out of range (declared count)",(u64)(u32)kn()->va_count,0);
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
 * APPEARANCE -- per-surface, finer grained than setNightMode.
 *
 *   {type:"mapAppearanceUpdate"|"uiAppearanceUpdate",
 *    params:{uuid, appearanceMode, appearanceSetting}}
 *
 * The pair goes on the wire as a [target][mode][setting] triple where
 * target 0 = UI, 1 = map.
 *
 * KNOBS: MIB3_SHIM_APPEARANCE=1 arms it; MIB3_SHIM_APPMODE / MIB3_SHIM_APPSETTING
 * give the pair; MIB3_SHIM_APPTARGET=map|ui picks the command.  Nothing is sent
 * unless APPEARANCE is on, so the shipped default is unchanged.  At runtime the
 * control-inbox line `a <map|ui> <mode> <setting>` drives it.
 *
 * NOT driven from the car's theme: the only theme-ish signal is g_night_mode, and
 * no shipped RSI/viwi service publishes the cluster's appearance.  dayNight IS
 * exposed on viwi.service.hmiintegration (port 49506) -- the honest candidate for
 * a future auto-drive, deliberately not wired here.
 * ========================================================================*/
static void cluster_send_appearance(void *session, int is_map, int mode, int setting)
{
  CFMutableDictionaryRef p;
  int rc;
  if (!session || !kn()->cmds) return;
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
  if (!kn()->appearance) return;
  cluster_send_appearance(session, kn()->app_map, kn()->app_mode, kn()->app_setting);
}

/* ==========================================================================
 * ZOOM: the STEERING-WHEEL up/down mirror.
 *
 * The wheel roller does NOT reach CarPlay.  It goes
 *   cluster (CAN MFL) -> BAP NavigationSd MapScale SetGet -> navsd_app
 *   -> POST {relativeSteps} to /maprenderer/discreteInputs/<renderer>
 *   -> the CAR'S OWN nav zooms -> navsd reports BAP MapScale Status -> the
 *   cluster paints its own scale OSD.
 * Our video replaces the nav's map, so the wheel moved a map nobody sees.
 *
 * The CarPlay cluster API for exactly this:
 *   changeMapZoomLevel {uuid: <cluster display uuid>, zoomDirection: 0|1}
 * (0 = zoom IN, non-zero = zoom OUT -- as CRSUIClusterZoomAction and the
 * DashBoard scene path both encode it).
 *
 * The steps come from the external tool `mib3wheel` (shim/tools/mib3wheel.c),
 * which watches the unit's own maprenderer RSI and writes "+1"/"-1" lines into an
 * AF_UNIX socket we own; this module is the receiving end.
 *
 * WHY A MAILBOX AND NOT A SEND FROM THE LISTENER THREAD: the listener would hold
 * the session pointer, so it could send into a torn-down session (use-after-free
 * inside carplay => reboot).  The listener only QUEUES; the CLUSTER THREAD, which
 * already owns a valid session for the length of the stream, drains and ships.
 * No new lifetime contract, no lock.
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
 * DashBoard wire form: 0 = in, anything else = out. */
static void cluster_send_zoom(void *session, int dir_in)
{
  if (!kn()->cmds || !session) return;
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
 * ONE socket carries three logical channels, so a single small tool can drive all
 * of them without a second listener/thread:
 *   +1 | 1 | in      zoom in        (queued, shipped by the cluster thread)
 *   -1 | out         zoom out
 *   v<N> | view <N>  switch to view-area index N
 *   next | prev      step the view-area index (wrapping over MIB3_SHIM_VA_COUNT)
 *   a <map|ui> <mode> <setting>   appearance update
 * An unknown line is LOGGED and ignored -- never acted on.  Lines are parsed from
 * a small accumulator so a streamed client ("+1-1" with no separators) behaves
 * exactly as it did before this change. */
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
  int fd, conn;
  logline("cluster/zoom","listener starting");
  unlink(path);
  fd = socket(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0);
  if (fd < 0){ logline("cluster/zoom","socket() failed"); return 0; }
  { unsigned char *q = (unsigned char*)&sa;
    memset(q, 0, sizeof(sa));
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
  static char path[128];
  void *th = 0;
  const char *p;
  if (g_zoom_started) return 0;                 /* idempotent: one listener per process */
  if (!env_on("MIB3_SHIM_ZOOM",1)){ logline("cluster/zoom","disabled (MIB3_SHIM_ZOOM=0)"); return 0; }
  if (!p_pthreadCreate){ logline("cluster/zoom","no pthread -- disabled"); return 0; }
  p = getenv("MIB3_SHIM_ZOOM_SOCK");
  if (!p || !p[0]) p = "/dev/shm/mib3zoom.sock";
  { unsigned long n = 0; while (p[n] && n < sizeof(path)-1){ path[n] = p[n]; n++; } path[n] = 0; }
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
    if (r == VA_REQ_NEXT)      idx = (g_va_current + 1) % kn()->va_count;
    else if (r == VA_REQ_PREV) idx = (g_va_current + kn()->va_count - 1) % kn()->va_count;
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

/* ---- AVCC <-> Annex-B ------------------------------------------------------
 * opcode-1 bodies are unencrypted avcC record configs; opcode-0 bodies are
 * AVCC access units (u32-be length-prefixed NALs at lengthSize ls).  The
 * decoder side (Cinemo/OMX) wants Annex-B start codes; this is the same
 * conversion a live-proven stock head unit performs. */
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
      memcpy(out + o + 4, a + i, size);
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
  unsigned cmdgen;                 /* g_cmd_gen when this setup became the command target */
} cl_ctx;

/* The type-111 listener we currently own, plus a GENERATION token.  A newer
 * SETUP supersedes both; see the listener-ownership note in cluster_thread.
 * The token, not the fd number, is the ownership test: own_listen closes the old
 * fd and immediately socket()s a new one, which the kernel hands back with the
 * SAME number -- so an fd comparison would let a stale receiver thread accept on
 * the NEW listener and service the new phone stream with an OLD key/session.
 * CLOSE OWNERSHIP: the party that moves g_lsock_gen from G to G+1 by CAS closes
 * generation G's fd, and nobody else does -- either the receiver thread after its
 * accept, or own_listen superseding it.  g_lsock and g_lsock_fdgen are written
 * only by the SETUP side. */
static volatile int g_lsock = -1;
static volatile int g_lsock_gen = 0;
static int g_lsock_fdgen = 0;              /* the generation g_lsock was published under */

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
  /* ---- LISTENER OWNERSHIP -------------------------------------------------
   * The old code did ONE blocking accept() with no timeout: if the phone never
   * opened the type-111 stream for this session (it aborted, or its SYN was
   * dropped) the thread sat in accept() forever and kept port 5000 bound for the
   * life of the process.  The NEXT SETUP then could not bind 5000 (SO_REUSEADDR
   * does not permit a second live listener), own_listen() fell through to 6031 --
   * which the firewall drops -- and the phone dialled a black hole WE had
   * advertised (-6722/-6753, 23 s sessions).  The same class of failure is fixed
   * the same way here.  So: accept on a short tick, and abandon the listener as
   * soon as a newer SETUP has superseded it. */
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
    { int g = c->lsockgen;
      if (__atomic_compare_exchange_n(&g_lsock_gen, &g, g + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) && p_close)
        p_close(c->lsock); }
    c->lsock = -1;
    logline("cluster/thread","stream connected");
    knobs_load();                    /* the loop below reads no environment */
    /* `c->session` is borrowed, not retained, so this thread must not outlive
     * the phone.  A vanished peer (cable pulled) never sends EOF; keepalive turns
     * it into a read error after ~8 s.  No idle timeout: a static map is
     * legitimately silent, and the phone's kernel answers the probes. */
    { int on = 1, idle = 5, intvl = 1, cnt = 3;
      if (setsockopt(connfd, SOL_SOCKET_, SO_KEEPALIVE_, &on, sizeof(on)) != 0 ||
          setsockopt(connfd, SOL_TCP_, TCP_KEEPIDLE_, &idle, sizeof(idle)) != 0 ||
          setsockopt(connfd, SOL_TCP_, TCP_KEEPINTVL_, &intvl, sizeof(intvl)) != 0 ||
          setsockopt(connfd, SOL_TCP_, TCP_KEEPCNT_, &cnt, sizeof(cnt)) != 0)
        logkv("cluster/thread","keepalive setsockopt failed errno",(u64)(unsigned)SHIM_ERRNO,0); }
    /* THE SINK STANDS UP HERE -- on this thread, NOT in the SETUP seam.  A
     * failing/blocking sink can no longer stall CarPlay session setup.  In the
     * default (off) mode this touches nothing at all; in helper mode it is one
     * UNIX-socket connect (the Path B sender on the other end). */
    sink_start();
    /* THE PHONE IS ON THE WIRE. The phone now sends opcode-1 (avcC) + opcode-5
     * (telemetry) but NO video until we drive the scene: forceKeyFrame at stream
     * start (a decoder cannot start mid-GOP, so ask for a cluster-targeted IDR),
     * and showUI so Maps renders the cluster scene.  showUI goes out both at
     * stream-connected and at first IDR -- cheap, and it covers both cipher
     * paths. */
    cluster_send_forcekeyframe(c->session, "stream-start");
    cluster_send_showui(c->session, "stream-connected");
    cluster_send_nightmode(c->session);
    /* The recovery pump's timers are PER-SESSION state.
     * They used to survive into the next session, so a new session inherited an
     * armed cadence from the old one (an early duplicate request or a wrong
     * first interval) and g_night_applied could suppress a needed first send.
     * One line, zero risk; the counters stay cumulative on purpose. */
    { u64 now0 = mono_ms(); (void)now0;
      g_kf_seen_ms = 0; g_kf_req_ms = 0; g_showui_retry_ms = 0; g_au_last_ms = 0;
      g_night_applied = -2; }
    /* Inbox requests queued while no stream existed belong to no session:
     * drop them here, on the consumer side, so they do not replay into this one. */
    g_zoom_qr = g_zoom_qw; g_va_qr = g_va_qw; g_ap_qr = g_ap_qw;
    /* ...and only THEN arm the one delayed re-activation (see cluster_showui_retry) */
    { long d = (long)env_num("MIB3_SHIM_SHOWUI_MS", 1800);
      g_showui_retry_ms = (d > 0) ? (mono_ms() + (u64)d) : 0;
      if (d > 0) logkv("cluster/cmd","showUI re-activation in ms",(u64)d,0); }
    u64 kf_base = g_kf_reqs;          /* per-session deltas for the close line */
    u64 flag_base = g_flag_changes;
    {
      unsigned char hdr[128];
      unsigned char *rbuf = 0; unsigned long rcap = 0;
      unsigned char cxbuf[512];
      long frames = 0, configs = 0, others = 0, idrs = 0;
      int stop = 0, first_idr_sent = 0;
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
        /* SCENE EDGE: the phone pauses its video when it re-composes (guidance
         * start/stop, app switch).  A working receiver re-sends showUI on every
         * guidance edge; we mirror that here, keyed on the only signal we have.
         * One keyframe + one showUI per edge,
         * rate-limited to 1/s so a flapping source cannot storm the command
         * channel. */
        if (g_gap_resume){
          static u64 last_edge = 0;
          u64 edge_now = mono_ms();
          g_gap_resume = 0;
          if (edge_now - last_edge > 1000){
            last_edge = edge_now;
            cluster_send_forcekeyframe(c->session, "scene-edge");
            cluster_send_showui(c->session, "scene-edge");
          }
        }
        /* 128-byte screen header (bodySize u32 LE @0, opcode @4) */
        while (got < 128){
          /* PERIODIC RECOVERY POINTS: wait at most 100 ms, then run the pump.
           * A blocking read() would hide the phone's silences from us -- and a
           * silence is exactly when the ECU needs a keyframe. */
          if (got == 0 && !cluster_wait_frame(connfd, c->session, 100)) continue;
          n = read(connfd, hdr + got, 128 - got); if (n <= 0){ logkv("cluster/thread","hdr read ended (0=EOF,1=err)",(u64)(n==0?0:1),0); logkv("cluster/thread","  bytes got",(u64)got,0); stop = 1; break; } got += n; }
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
              memcpy(nb2, rbuf, rcap);
              if (p_free && rbuf) p_free(rbuf);
              rbuf = nb2; rcap = newsz;
            } else { logline("cluster/thread","no malloc"); break; }
          }
          got = 0;
          while (got < (long)bs){ n = read(connfd, rbuf + got, (long)bs - got); if (n <= 0){ logkv("cluster/thread","body read ended (0=EOF,1=err)",(u64)(n==0?0:1),0); logkv("cluster/thread","  bodySize",(u64)bs,0); logkv("cluster/thread","  bytes got",(u64)got,0); stop = 1; break; } got += n; }
          if (stop) break;
          if (op == 1){
            /* codec config: UNENCRYPTED avcC (the phone also attaches
             * screen-state flags -- hdr[6] -- which we log) */
            configs++;
            logkv("cluster/thread","codec-config body",(u64)bs,0);
            logkv("cluster/thread","config flags",(u64)(unsigned)hdr[6],0);
            night_flag_update(hdr[6], "cluster/thread");   /* phone signal for night */
            { float w = 0, h = 0;   /* sender's encoded size @ hdr+16/+20 */
              unsigned char *pw = (unsigned char*)&w, *ph = (unsigned char*)&h;
              int k;
              for (k = 0; k < 4; k++){ pw[k] = hdr[16+k]; ph[k] = hdr[20+k]; }
              logkv("cluster/thread","encoded size w",(u64)(u32)(w>0?((unsigned)(w+0.5f)):0),0);
              logkv("cluster/thread","encoded size h",(u64)(u32)(h>0?((unsigned)(h+0.5f)):0),0); }
            { unsigned char cfgout[4096]; unsigned cfglen = 0;
              unsigned nls = avcc_config_to_annexb(rbuf, bs, cfgout, sizeof(cfgout), &cfglen);
              if (nls){
                ls = (unsigned char)nls; g_cl_ls = ls;
                { unsigned long k; for (k = 0; k < cfglen && k < sizeof(g_cl_cfg_annexb); k++)
                    g_cl_cfg_annexb[k] = cfgout[k]; }
                g_cl_cfg_len = cfglen < sizeof(g_cl_cfg_annexb) ? cfglen : sizeof(g_cl_cfg_annexb);
                logkv("cluster/thread","avcC -> Annex-B SPS/PPS bytes",(u64)g_cl_cfg_len,0);
                logkv("cluster/thread","NAL lengthSize",(u64)(unsigned)ls,0);
              } else logline("cluster/thread","opcode-1: not an avcC record (kept, no SPS/PPS)"); }
          }
          else if (op == 0){
            /* video AU -- decrypt per the stream's security.  THE WIRE FORMAT
             * (ProcessFrames 0x11fbf0, verified): chacha bodies are
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
                { u64 aunow = mono_ms();
                  if (g_au_last_ms && aunow - g_au_last_ms > 800) g_gap_resume = 1;
                  g_au_last_ms = aunow; }
                if (is_idr){
                  idrs++;
                  g_kf_seen_ms = g_au_last_ms;      /* the recovery point is now on the wire */
                  if (!first_idr_sent){
                    first_idr_sent = 1;
                    logkv("cluster/thread","FIRST IDR at frame",(u64)frames,0);
                    /* showUI + night + IDR refresh at the first decodable frame
                     * (the stream is now a real canvas) */
                    cluster_send_showui(c->session, "first-IDR");
                    cluster_send_nightmode(c->session);
                    cluster_send_forcekeyframe(c->session, "first-IDR");
                    cluster_send_appearance_armed(c->session);   /* no-op unless MIB3_SHIM_APPEARANCE=1 */
                  }
                }
                /* The receiver hands us SLICES ONLY -- the parameter sets arrive out
                 * of band (the opcode-1 avcC record above) and are NOT in the media
                 * AUs.  A decoder fed bare slices decodes nothing and conceals to
                 * flat black ("decodes but the surface is black"), so stamp
                 * SPS/PPS onto EVERY AU: ~32 bytes, each AU self-contained, which
                 * also makes the helper robust across reconnects. */
                if (g_cl_cfg_len > 0) sink_feed(g_cl_cfg_annexb, g_cl_cfg_len, 0);
                { int was_conn = mib3shim_helper_connected();
                  if (sink_feed(rbuf, (unsigned long)pl, 0) == 0) g_au_fed++;
                  /* A freshly (re)connected helper has no reference frames, so
                   * ask the phone for a keyframe and a real IDR will follow. */
                  if (!was_conn && mib3shim_helper_connected()){
                    /* UNCONDITIONAL -- the one request that must never be
                     * rate-limited.  It fires at most once per helper connection
                     * (guarded by !was_conn) and is what makes a freshly attached
                     * decoder start on an IDR instead of mid-GOP.  An earlier
                     * storm limiter suppressed it on every reconnect (rc=0 at
                     * stream-start, then "rate-limited (s since last)=1", decoder
                     * got 77 P-slices, qdrop=279, frozen). */
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
              /* the helper-hop totals on the same marker: the DECODER prints its
               * own counters (parse/ausz/kdrop/qdrop/awt), so diffing `sent total`
               * here against `ausz` there, per window, is the ONLY way to see AUs
               * lost between the two processes.  `dropped total` must stay 0 in a
               * healthy session.
               * TIMESTAMPED because an untimed copy of these numbers was misread
               * for a whole session: `sent total` counts RECORDS (~2 per video
               * AU: cfg + frame), which produced a bogus 2x rate error. */
              logkv("cluster/thread","hb ms",mono_ms(),0);
              logkv("cluster/thread","helper sent total (RECORDS; ~2/AU)",(u64)hlp_sent,0);
              logkv("cluster/thread","helper AUs total",g_au_fed,0);
              logkv("cluster/thread","helper dropped total",(u64)hlp_drop,0);
            }
          } else {
            others++;   /* opcodes 2/4/5: keepalive/misc.  NAMED from the SHIPPED
                         * binary (AirPlayReceiverSessionScreen_ProcessFrames
                         * 0x11fbf0):
                         *   opcode 0 -> video AU (decrypt, ScreenStreamProcessData)
                         *   opcode 1 -> avcC + SetWidthHeight + SetAVCC
                         *   opcode 2/4/5 -> goto "done" (local_dc = 0) SILENTLY
                         *   anything else -> "### Unknown screen opcode: %u" + -6727
                         * So 2/4/5 are INERT BY CONSTRUCTION: no error, no state
                         * change, not a hidden control channel.  (requestUI is not a
                         * screen opcode at all -- it is an RTSP /command type.)
                         * Logged anyway: navigation start drops the whole session
                         * and control traffic is the prime suspect. */
            { const char *H = "0123456789abcdef"; int k; char hs[97]; char bh[192];
              unsigned lim = 48, bl = (bs < 64u) ? (unsigned)bs : 64u;
              for (k = 0; k < (int)lim; k++){ unsigned char cc = hdr[k]; hs[k*2] = H[cc>>4]; hs[k*2+1] = H[cc&15]; }
              hs[lim*2] = 0;
              for (k = 0; k < (int)bl; k++){ unsigned char cc = rbuf[k]; bh[k*2] = H[cc>>4]; bh[k*2+1] = H[cc&15]; }
              bh[bl*2] = 0;
              logkv("cluster/ctl","opcode",(u64)op,0);
              logkv("cluster/ctl","bodySize",(u64)bs,0);
              if (op == 2)      logline("cluster/ctl/meaning","opcode 2 = stock-ignored keepalive/misc (no error, no state change)");
              else if (op == 4) logline("cluster/ctl/meaning","opcode 4 = stock-ignored keepalive/misc (no error, no state change)");
              else if (op == 5) { logline("cluster/ctl/meaning","opcode 5 = SENDER TELEMETRY (binary plist: encoderCurrentFPS/sinkOverflowDropFPS/rttAvg/txUsageAvg) -- inert to us, but it is the phone telling us its own rate");
                                   ctl5_dump(rbuf, (unsigned long)bs); }
              else              logline("cluster/ctl/meaning","UNKNOWN opcode (stock returns -6727 '### Unknown screen opcode')");
              logline("cluster/ctl/hdr-head", hs);
              logline("cluster/ctl/body-head", bh); } }
        }
      }
      logkv("cluster/thread","frames total",(u64)frames,0);
      logkv("cluster/thread","idrs total",(u64)idrs,0);
      logkv("cluster/thread","keyframe requests this session",g_kf_reqs - kf_base,0);
      logkv("cluster/thread","config-flag changes this session",g_flag_changes - flag_base,0);
      logkv("cluster/thread","idr refresh target ms",(u64)(int)env_num("MIB3_SHIM_IDR_MS",500),0);
      logkv("cluster/thread","configs",(u64)configs,0);
      logkv("cluster/thread","other-opcodes",(u64)others,0);
      if (p_free && rbuf) p_free(rbuf);
      logline("cluster/thread","stream closed");
      /* TELL THE DECODER THE STREAM IS OVER.  Nothing downstream sees the phone
       * hang up otherwise: the AU socket stays open, the decoder's read() never
       * returns 0, it keeps running and re-commits its LAST frame -- which is
       * exactly why the cluster shows a map of a road that no longer exists,
       * forever.  Closing the helper socket IS the EOF the decoder already
       * handles (`online: shim gone` -> re-accept), so no decoder change is
       * needed.
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
  /* Stop naming a session that may be freed once we are gone, unless a newer
   * SETUP already took the command target (generation moved). */
  { void *mine = c->session;
    if (g_cmd_gen == c->cmdgen &&
        __atomic_compare_exchange_n(&g_cmd_session, &mine, (void*)0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      logline("cluster/thread","command target cleared"); }
  logline("cluster/thread","exit");
  if (p_free) p_free(c);
  return 0;
}

/* ---- our own listener (what every working receiver does), plus the port chain
 * the unit's own FIREWALL forces.  A working receiver binds a WILDCARD TCP
 * listener and advertises whatever port it got; on THIS unit the phone-facing
 * interfaces (ncm0 wired, wlap* wireless) have an iptables/ip6tables default-DENY
 * allowlist whose only carplay TCP ports are 5000:5001 / 6030 / 7000
 * (lge/app_ro/network, section "## carplay for ncm0 (usb interface)").  The stock
 * event listener owns 5001 and the stock screen owns 6030, so 5000 is the only
 * firewall-legal free port.  6031 (the MHI3-native alt port) and any ephemeral
 * port are DROPPED -- the phone's SYN never arrives -- so they are fallbacks that
 * need a firewall rule this repo does not ship. */
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
  struct sa6 sa, got; unsigned int glen = sizeof(got);
  /* port chain: explicit knob -> 5000 (fw-allowlisted, stock-observed free) ->
   * 6031 (MHI3-native alt; fw-DROPPED, needs a firewall rule) -> ephemeral
   * (fw-DROPPED, needs a firewall rule).  Log every attempt so the log
   * shows exactly which link of the chain won. */
  unsigned long chain[4]; int nc = 0;
  { char *pe = getenv("MIB3_SHIM_PORT");
    unsigned long pv = 0;
    if (pe && pe[0] >= '0' && pe[0] <= '9'){ while (*pe >= '0' && *pe <= '9'){ pv = pv*10 + (*pe - '0'); pe++; } }
    if (pv) chain[nc++] = pv; }
  chain[nc++] = 5000;
  chain[nc++] = 6031;
  chain[nc++] = 0;   /* ephemeral */
  /* SUPERSEDE THE PREVIOUS LISTENER FIRST: a listener left over from an earlier
   * type-111 SETUP holds 5000, SO_REUSEADDR does not permit a second live
   * listener, and the bind would fall through to the firewall-dropped 6031.
   * Closing it here also unsticks the old thread's poll. */
  if (g_lsock >= 0){
    int old = g_lsock, g = g_lsock_fdgen;
    g_lsock = -1;
    if (__atomic_compare_exchange_n(&g_lsock_gen, &g, g + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)){
      close(old);
      logkv("cluster/listen","superseded old listener fd",(u64)(u32)old,0);
      logkv("cluster/listen","new listener generation",(u64)(u32)g_lsock_gen,0);
    }
  }
  fd = socket(10 /*AF_INET6*/, 1 /*SOCK_STREAM*/, 0);
  if (fd < 0) return -1;
  setsockopt(fd, 1 /*SOL_SOCKET*/, 2 /*SO_REUSEADDR*/, &one, sizeof(one));
  { int zero = 0;
    int r = setsockopt(fd, 41 /*IPPROTO_IPV6*/, 26 /*IPV6_V6ONLY*/, &zero, sizeof(zero));
    logkv("cluster/listen","IPV6_V6ONLY=0 rc",(u64)(u32)r,0); }
  memset(&sa, 0, sizeof(sa));
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
  g_lsock_fdgen = __atomic_add_fetch(&g_lsock_gen, 1, __ATOMIC_SEQ_CST);
  if (out_gen) *out_gen = g_lsock_fdgen;
  *out_port = 0;
  if (getsockname(fd, &got, &glen) == 0){
    *out_port = (int)(((got.port & 0xff) << 8) | (got.port >> 8));   /* ntohs */
    logkv("cluster/listen","bound family",(u64)(u32)got.family,0);
    logkv("cluster/listen","bound port",(u64)(u32)(*out_port),0);
    { const char *fw = (*out_port == 5000 || *out_port == 5001) ? "FW-ALLOWED (ncm0 5000:5001)"
                      : (*out_port == 6030 || *out_port == 7000) ? "FW-ALLOWED but stock-owned"
                      : "FW-DROPPED (needs a firewall rule this repo does not ship)";
      logline("cluster/listen/fw", fw); }
  }
  return fd;
}

/* Set up a second screen for a type-111 stream dict and append
 * {type:111, dataPort} to response["streams"].  Returns 0 on success. */
static int cluster_setup(void *session, void *streamDict, void *response){
  if (!session || !streamDict || !response){ logline("cluster","bad args"); return -1; }
  /* SELF-CONTAINED RECEIVER -- no stock ScreenStream.  A stock alt screen binds the
   * MAIN display and starts a second in-dio OMX decoder on this unit: black main
   * screen + decoder crash.  So we own the listener and service the stream
   * ourselves; nothing here touches the unit's screen/stream machinery. */
  long connID = (long)dgeti64k((CFDictionaryRef)streamDict, "streamConnectionID", 0);
  logkv("cluster","streamConnectionID",(u64)connID,0);

  /* Stream key: mirror stock _ScreenSetup (libAirPlay case 0x6e) --
   * AES when session+0x478==0, else chacha via the unit's FUN_001163d8.
   * AES: 16-byte key at [0..16) + 16-byte IV at [16..32). */
  unsigned char skey[32]; int keylen = 0, cipher = 0;
  memset(skey, 0, 32);
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
      /* A paired/authenticated receiver also echoes the request's
       * streamConnectionID in the response descriptor.  Our stock 110 descriptor
       * proves it is not REQUIRED on this unit (stock sends only {type,dataPort}
       * and the phone connects), so this is a belt knob: MIB3_SHIM_CONNID=1. */
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

  /* ROUTE-B sink: THE SINK IS DEFERRED TO THE RECEIVER THREAD (cluster_thread ->
   * sink_start) and is NOT created here.  Display-pipeline work inside this seam
   * -- the CarPlay SETUP path -- is exactly what blacked the centre screen and
   * reset the unit on 2026-10-16: a blocking Cinemo or EGL call here stalls
   * session setup and carplay.service is FailureAction=reboot-immediate.  So this
   * only RESOLVES the mode and logs it. */
  sink_resolve();
  { char m[96]; buf b = { m, sizeof(m)-2, 0 };
    bs(&b,"mode="); bu64(&b,(u64)(u32)g_sink,10,0);
    bs(&b," (0=off 2=helper; deferred to cluster/thread)"); m[b.n]=0;
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
  memcpy(c->key, skey, 32);
  c->keylen = keylen; c->cipher = cipher; c->counter = 0;
  c->night = g_night_mode; c->aes_inited = 0;
  g_cmd_session = session;   /* the command target once the stream connects */
  g_cmd_gen++;               /* invalidate completions from the previous SETUP */
  c->cmdgen = g_cmd_gen;
  /* the wheel->CarPlay zoom path: one listener per process, started here so it
   * outlives per-connection cluster threads.
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
  if (p_pthreadDetach) p_pthreadDetach(th);
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
   * change (the 2026-10-16 "banner still reads a stale knob" class of bug).  Reset
   * the breadcrumb fd too so the stick path is re-discovered if it just mounted. */
  g_sink = -1;
  if (bc_fd >= 0) close(bc_fd);
  bc_fd = -2;
  bs(&b,"  SINK="); sink_resolve(); bu64(&b,(u64)(u32)g_sink,10,0);
  bs(&b," (0=off 2=helper)");
  /* view-area / command-completion knobs (2026-10-19) */
  bs(&b,"  CMDACK="); bu64(&b,(u64)env_on("MIB3_SHIM_CMDACK",1),10,0);
  bs(&b,"  VIEWAREAS="); bu64(&b,(u64)env_on("MIB3_SHIM_VIEWAREAS",0),10,0);
  bs(&b,"  VA_COUNT="); bu64(&b,(u64)(unsigned)va_count(),10,0);
  bs(&b,"  VA_PRESET="); bu64(&b,(u64)env_on("MIB3_SHIM_VA_PRESET",1),10,0);
  bs(&b,"  VA_INITIAL="); bu64(&b,(u64)(unsigned)env_num("MIB3_SHIM_VA_INITIAL",1),10,0);
  bs(&b,"  VA_CMD="); bu64(&b,(u64)env_on("MIB3_SHIM_VA_CMD",1),10,0);
  bs(&b,"  VA_VISIBLE="); bu64(&b,(u64)env_on("MIB3_SHIM_VA_VISIBLE",0),10,0);
  /* The frame-rate CEILING we advertise for the cluster, and whether the phone's
   * own telemetry will be recorded.  Both are on the banner because both were
   * invisible knobs in a session that needed them: FPS decides what the phone
   * encodes at, CTL5 is the only channel reporting the rate it actually achieved.
   *
   * DEFAULT 60 (2026-10-30, re-measured).  The phone uses maxFPS as its RATE, not
   * a ceiling: at FPS=30 it delivered 14.61 fps, at FPS=60 it delivered 26.60 fps
   * with wire peak 53 dgrams per 25 ms (the car's own 61), RTP loss 0, lossAvg ~0,
   * no artifacts, rttAvg 3 ms.  MIB3_SHIM_FPS=30 restores the previous value. */
  bs(&b,"  FPS="); bu64(&b,(u64)(u32)env_num("MIB3_SHIM_FPS",60),10,0);
  ctl5_resolve();
  bs(&b,"  CTL5="); bu64(&b,(u64)(u32)(ctl5_want > 0 ? 1 : 0),10,0);
  /* geometry, on every plug: the 2x experiment was run without knowing what the
   * phone was told about the panel's PHYSICAL size, i.e. without its density. */
  { geom_t gm; cluster_geom(&gm);
    bs(&b,"  W="); bu64(&b,(u64)(u32)gm.w,10,0);
    bs(&b,"  H="); bu64(&b,(u64)(u32)gm.h,10,0);
    bs(&b,"  SCALE="); bu64(&b,(u64)env_num("MIB3_SHIM_SCALE",100),10,0);
    bs(&b,"  SCALE_PHY="); bu64(&b,(u64)env_on("MIB3_SHIM_SCALE_PHY",0),10,0);
    bs(&b,"  PHY="); bu64(&b,(u64)(u32)gm.phy_w,10,0);
    bs(&b,"x"); bu64(&b,(u64)(u32)gm.phy_h,10,0);
    bs(&b,"mm="); bu64(&b,(u64)(u32)geom_density_milli(&gm),10,0);
    bs(&b,"px/mm(milli)"); }
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
  log_write(mem, b.n);
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
  g_cluster_advertised = 0;              /* re-decided for THIS display list below */
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

  /* 2) advertise the cluster for THIS display list, after the main display,
   *    gated on the stick marker.  Per-LIST idempotent -- see array_has_cluster. */
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
    geom_t gm;
    int cw, chp, cwph, chph, cfps;
    cluster_geom(&gm);
    cw   = gm.w;
    chp  = gm.h;
    cwph = gm.phy_w;      /* real PHYSICAL size in mm, NOT the pixel size */
    chph = gm.phy_h;
    cfps = (int)env_num("MIB3_SHIM_FPS", 60);  /* DEFAULT 60 (re-measured).
                                                 *
                                                 * It was 15 at first because the phone's
                                                 * telemetry read "queue 8-13" at 15 against
                                                 * "18-22" at 60 -- a UNITS ERROR: the field is
                                                 * queuedFramesAvg, i.e. FRAMES, so 8-13 frames
                                                 * at 15 fps is 530-870 ms of delay while 17
                                                 * frames at 60 fps is 283 ms.  The higher rate
                                                 * had the SHORTER queue all along.
                                                 *
                                                 * Measured with the paced sender:
                                                 * 14.61 fps at FPS=30 vs 26.60 fps at FPS=60,
                                                 * wire peak 53 dgrams per 25 ms (car's own 61),
                                                 * RTP loss 0, no artifacts, rttAvg 3 ms.
                                                 *
                                                 * The MECHANISM was never wrong (the bandwidth
                                                 * ladder maps avgTx to 10/20/30/60, then clamps
                                                 * to maxFPS; the WIRED branch uses maxFPS
                                                 * directly): a high ceiling is harmless only
                                                 * when something re-encodes at its own pace.
                                                 * Path B hands the phone's stream to the ECU
                                                 * unchanged, so the phone's queue lands on the
                                                 * glass. */
    logline("AddScreenDisplay","ADDING cluster display");
    log_uuid("AddScreenDisplay/cluster-uuid", cu);
    logkv("AddScreenDisplay","cluster widthPixels",(u64)(u32)cw,0);
    logkv("AddScreenDisplay","cluster heightPixels",(u64)(u32)chp,0);
    logkv("AddScreenDisplay","cluster maxFPS",(u64)(u32)cfps,0);
    /* the advertised PHYSICAL size and the DENSITY it implies.  The density is
     * the number that changes when only the pixels are scaled -- it was never
     * logged before, so "the phone sees 222 dpi at 2x" was invisible. */
    logkv("AddScreenDisplay","cluster widthPhysical",(u64)(u32)cwph,0);
    logkv("AddScreenDisplay","cluster heightPhysical",(u64)(u32)chph,0);
    logkv("AddScreenDisplay","cluster px per mm (milli)",(u64)(u32)geom_density_milli(&gm),0);
    logkv("AddScreenDisplay","cluster scale pct",(u64)(u32)env_num("MIB3_SHIM_SCALE",100),0);
    logkv("AddScreenDisplay","cluster scale physical",(u64)(u32)env_on("MIB3_SHIM_SCALE_PHY",0),0);
    /* cluster features/inputDevice: one working receiver config uses 0/0;
     * another uses features=0x0A, primaryInputDevice=3. Env-tunable. */
    int cfeat  = (int)env_num("MIB3_SHIM_CFEAT", 0x0A);  /* HIGH_FIDELITY_TOUCH|KNOBS = 0x0A */
    int cindev = (int)env_num("MIB3_SHIM_CINDEV", 0);
    /* cwph/chph came from cluster_geom() above (geometry + mm + density live in
     * ONE place now -- reading MIB3_SHIM_CWPHY again here is what let the two
     * diverge during the 2x experiment). */
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

  /* Inject enabledFeatures = [...existing, viewAreas, altScreen] -- into the
   * CONTROL SETUP response ONLY.
   *
   * SCOPE: the phone proposes in the control-SETUP REQUEST under "features"
   * (media/stream SETUPs carry "streams" only -- the two are split exactly this
   * way) and reads the negotiated set from the control-SETUP RESPONSE under
   * "enabledFeatures" (iOS _apsession_ensureControlStreamSetup, verified in the
   * iOS 26.6 AirPlaySender decompile).  The old build injected it into EVERY
   * response, including the 111 stream SETUP, which is wrong.
   *
   * CONTENT: each string in that array flips a sibling endpoint flag
   * (viewAreas+0x3D, altScreen+0x3E, uiContext+0x3F, enhancedSiri+0x39), so
   * NEVER echo the phone's proposal.  Seed from the receiver's EXISTING response
   * value (usually absent on this unit) and add ONLY the two contracts we
   * implement.  iAPChannel is opt-in (MIB3_SHIM_EF_IAPCHANNEL=1): the stock MIB3
   * has never negotiated it and no stock head unit has been shown to claim it.
   *
   * COUPLING: only when the cluster was actually advertised (g_cluster_advertised),
   * i.e. altScreen flag <=> >=2 displays.  MIB3_SHIM_ENABLEDFEAT=0 ablates it. */
  if (env_on("MIB3_SHIM_ENABLEDFEAT",1) && gate_active() && g_cluster_advertised &&
      responseOut && *responseOut && p_CFDictionarySetValue &&
      p_CFStringCreateWithCString && p_kCFTypeArrayCallBacks &&
      p_CFDictionaryGetValue && session && request) {
    /* control SETUP iff the request carries the "features" proposal key */
    void *kfeat = dgetk((CFDictionaryRef)request, "features");
    int is_control_setup = (kfeat != 0);
    if (is_control_setup){
      /* log the phone's proposal: the exact set this phone would negotiate --
       * the diagnostic the old builds never had */
      if (p_CFArrayGetCount && p_CFArrayGetValueAtIndex && p_CFGetTypeID &&
          p_CFArrayGetTypeID && p_CFStringGetTypeID && p_CFStringGetCString &&
          kfeat && p_CFGetTypeID(kfeat) == p_CFArrayGetTypeID()){
        CFIndex j, m = p_CFArrayGetCount((CFArrayRef)kfeat);
        char list[320]; unsigned long lw = 0;
        list[0] = 0;
        for (j = 0; j < m && j < 24; j++){
          void *e = (void*)p_CFArrayGetValueAtIndex((CFArrayRef)kfeat, j);
          char tmp[48]; tmp[0] = 0;
          if (e && p_CFGetTypeID(e) == p_CFStringGetTypeID() &&
              p_CFStringGetCString((CFStringRef)e, tmp, sizeof(tmp), K128)){
            const char *sp = " "; const char *pp = sp;
            unsigned long tl = 0; while (tmp[tl]) tl++;
            unsigned long pl = 0; while (pp[pl]) pl++;
            unsigned long k;
            if (lw + pl + tl >= sizeof(list)) break;
            for (k = 0; k < pl; k++) list[lw+k] = pp[k];
            lw += pl;
            for (k = 0; k < tl; k++) list[lw+k] = tmp[k];
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
 * Interposes AirPlayCopyServerInfo, which BUILDS the /info dict the phone reads
 * (displays, extendedFeatures, audioFormats, ...).  The unit's own /info HTTP
 * handler (libAirPlay 0xcc98) calls it through the PLT stub at 0x9640 and
 * serialises the returned dict with CFBinaryPlistV0Create.
 *
 * Why not the display-array seam: `displayPanels` is a DIFFERENT TOP-LEVEL key of
 * the /info dictionary and AirPlayInfoArrayAddScreenDisplay only ever reaches the
 * `displays` array.  This seam hands us the whole mutable dict, so it is the only
 * place the modern schema can be completed.
 *
 * MIB3_SHIM_INFO=0 ablates it (target_env.py).  With the gate off, or the cluster
 * never advertised, the dict is byte-identical to stock -- info_complete publishes
 * nothing without a type-111 entry.
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
    /* judge the advert we are about to serialise: one malformed display entry
     * costs the whole session, and the 2x experiment proved the log alone does
     * not show it (see THE ADVERT SELF-CHECK). */
    if (done && env_on("MIB3_SHIM_INFOCHECK",1)) info_check((CFMutableDictionaryRef)info);
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
int  mib3shim_info_check(void *info){ resolve(); return info_check((CFMutableDictionaryRef)info); }
int  mib3shim_info_completed(void){ return g_info_completed; }

/* ---- cluster frame counter (harness) --------------------------------------
 * The AUs the receiver actually handed to the sink.  The test harness dlsym's
 * this export, and it is exactly the number the audit bench means by "frames"
 * (the old ScreenStreamProcessData counter is gone with the in-process path). */
unsigned long mib3shim_cluster_frames(void){ return g_au_fed; }



/* ==========================================================================
 * THE SINK DISPATCHER -- the ONE place cluster frames may leave the process, and
 * why the black-screen/reset cannot repeat.
 *
 * An in-process display sink was once enabled live: the centre screen went black
 * as CarPlay started and the unit reset.  The sink had been created INSIDE the
 * CarPlay SETUP seam (stalling session setup -- carplay.service is
 * FailureAction=reboot-immediate) and BOTH display paths ran IN-PROCESS, sharing
 * Adreno/OMX/ILM/EGL state with CarPlay's rendering.
 *
 * What is left (Path B only):
 *   * off (default)  -- nothing leaves the process.
 *   * helper         -- frames forwarded OUT OF PROCESS over the AU socket
 *                       (mib3helper.h framing); the consumer is mib3clusterstream,
 *                       the RTP/TS sender.  carplay performs NO display call, so a
 *                       sender fault cannot touch the centre screen or carplay.
 * The sink stands up on the RECEIVER THREAD (cluster_thread), never in the SETUP
 * seam.  dry/local and the machinery they drove were REMOVED with the rest of Path
 * A; a stale knob naming them is REFUSED loudly, so the accident cannot re-arm and
 * the removal is visible.
 * ========================================================================*/
static void sink_resolve(void){
  char *v;
  if (g_sink >= 0) return;
  g_sink = SINK_OFF;
  v = getenv("MIB3_SHIM_SINK");
  if (v && v[0]){
    if      (v[0]=='h') g_sink = SINK_HELPER;
    else if (v[0]=='o' || v[0]=='0') g_sink = SINK_OFF;
    else {
      logline("sink","MIB3_SHIM_SINK=<dry|local|...> REMOVED (Path A, 2026-10-29): only 'off' and 'helper' exist. Staying OFF.");
      g_sink = SINK_OFF;
    }
  } else if (getenv("MIB3_SHIM_SURFACE") || getenv("MIB3_SHIM_BRIDGE") ||
             getenv("MIB3_SHIM_LOCAL") || getenv("MIB3_SHIM_LOCAL_ACK") ||
             getenv("MIB3_SHIM_LOCAL_FALLBACK")){
    logline("sink","LEGACY SURFACE/BRIDGE/LOCAL KNOBS ARE DEAD (Path A removed 2026-10-29). Use MIB3_SHIM_SINK=helper for Path B. Staying OFF.");
  }
  { char m[64]; buf b={m,sizeof(m)-2,0};
    bs(&b,"mode="); bu64(&b,(u64)(u32)g_sink,10,0);
    bs(&b," (0=off 2=helper)"); m[b.n]=0; logline("sink/resolve", m); }
}

static int sink_start(void){
  sink_resolve();
  bc("sink:start");
  if (g_sink == SINK_OFF){ logline("sink","off: nothing leaves the process"); bc("sink:off"); return 0; }
  /* SINK_HELPER: one UNIX-socket connect, retried per frame if absent. */
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

static int sink_feed(const void *data, unsigned long len, unsigned long ts){
  /* no sink_resolve() here: cluster_setup and sink_start resolved it before the
   * loop, and the loop must not reach getenv (see KNOB SNAPSHOT) */
  if (g_sink == SINK_HELPER) return helper_feed(data, len, ts);
  return 0;   /* off: dropped, counted by the caller */
}

/* ---- harness hooks (sink dispatcher + out-of-process helper) ------------ */
int  mib3shim_sink_mode(void){ sink_resolve(); return g_sink; }
int  mib3shim_sink_start(void){ sink_resolve(); return sink_start(); }
int  mib3shim_sink_feed(const void *d,unsigned long n,unsigned long ts){ sink_resolve(); return sink_feed(d,n,ts); }
int  mib3shim_helper_connect(void){ return helper_connect(); }
int  mib3shim_helper_feed(const void *d,unsigned long n,unsigned long ts){ return helper_feed(d,n,ts); }
int  mib3shim_helper_connected(void){ return hlp_fd >= 0; }
unsigned long mib3shim_helper_sent(void){ return hlp_sent; }
unsigned long mib3shim_helper_dropped(void){ return hlp_drop; }
const char *mib3shim_bc_path(void){ if (bc_fd == -2){ bc_pick_path(); } return bc_path; }
