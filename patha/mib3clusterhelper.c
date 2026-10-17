/* ============================================================================
 * mib3clusterhelper.c -- the OUT-OF-PROCESS cluster delivery helper (2026-10-16).
 *
 * WHY.  On 2026-10-16 the delivery sink was enabled INSIDE carplay and the
 * centre screen went black and the unit reset -- carplay.service is
 * FailureAction=reboot-immediate, and the sink shared the process's
 * Adreno/OMX/ILM/EGL state with CarPlay's own rendering (see
 * ../LANE_CLUSTER_LIVE_2026-10-16.md S5b and the sink-dispatcher comment in
 * mib3shim.c).  This helper is the SAFE architecture: it runs as a SEPARATE
 * PROCESS, so the surface + decode happen in their own address space.  A fault
 * here cannot take down carplay or black the centre screen.
 *
 * WHAT IT DOES.
 *   1. dlopen()s the shim .so and takes its OWN sink primitives
 *      (mib3shim_bridge_create / mib3shim_bridge_feed -- the exact same code the
 *      in-carplay path would run, so there is ONE implementation);
 *   2. stands up the cluster display sink (a second Cinemo pipeline by default;
 *      `surface` / `dry` via MIB3_HELPER_SINK);
 *   3. listens on an AF_UNIX socket and feeds every frame the shim forwards
 *      (mib3helper.h wire format: 16-byte header + Annex-B payload).
 *
 * The shim inside carplay only connect()s + write()s -- it makes NO display
 * call, so the in-carplay blast radius is zero.  If this helper is absent the
 * shim drops frames and does nothing.
 *
 * CONFIG (env):
 *   MIB3_HELPER_SHIM   shim .so path            (default /dev/shm/mib3shim.so)
 *   MIB3_HELPER_SOCK   AF_UNIX path to listen on (default /dev/shm/mib3cluster.sock)
 *   MIB3_HELPER_SINK   bridge (default) | surface | dry
 *   MIB3_HELPER_LIBS   ':'-list of extra .so to dlopen (bring the real Cinemo /
 *                      Wayland / EGL into the global scope so the shim's lazy
 *                      dlsym finds them).  Default: libNmeSDK.so +
 *                      libwayland-client/egl + libEGL + libGLESv2.
 *   MIB3_SHIM_W/H/IVIID  geometry + ivi surface id (shared with the shim)
 *
 * Built -nostdlib against the unit's own glibc (same recipe as the bench
 * drivers): aarch64, entry = _start, no libc start files needed.
 *
 * *** Desk caveat: on the desk the graphics/Cinemo stack is the libstubgfx.so
 * *** stub (real Adreno/OMX is hardware).  This proves the helper's wiring and
 * *** isolation, not real pixels -- see the lane doc. ***
 * ==========================================================================*/
typedef unsigned long u64;
extern void  *dlopen(const char *, int);
extern void  *dlsym(void *, const char *);
extern char  *getenv(const char *);
extern void  *malloc(unsigned long);
extern int    write(int, const void *, unsigned long);
extern long   read(int, void *, unsigned long);
extern int    open(const char *, int, ...);
extern int    fsync(int);
extern int    close(int);
extern int    unlink(const char *);
extern int    chmod(const char *, unsigned);
extern int    socket(int, int, int);
extern int    bind(int, const void *, unsigned int);
extern int    listen(int, int);
extern int    accept(int, void *, unsigned int *);
extern void   _exit(int);

#define RTLD_NOW     2
#define RTLD_GLOBAL  0x100
#define RTLD_DEFAULT ((void *)0)

/* keep in lock-step with mib3helper.h (kept standalone: this file is linked
 * without the shim's headers on purpose, so the protocol is duplicated here and
 * a mismatch is caught by the bench assertion, not the compiler) */
#define MIB3_HELP_MAGIC        0x4d334331u
#define MIB3_HELP_HDRLEN       16u
#define MIB3_HELP_SOCK_DEFAULT "/dev/shm/mib3cluster.sock"
#define MIB3_HELP_MAXLEN       (8u << 20)

typedef int  (*fn_create)(int, int, unsigned);
typedef int  (*fn_feed)(const void *, unsigned long, unsigned long);
typedef int  (*fn_i0)(void);
typedef int  (*fn_wf)(const void *, int, int);

static u64 slen_(const char *s){ u64 n=0; while (s && s[n]) n++; return n; }
static void P(const char *s){ write(1, s, slen_(s)); }
static void Pkv(const char *tag, u64 v){
  char b[96]; int k=0; const char *h="0123456789abcdef";
  b[k++]='['; while(*tag) b[k++]=*tag++; b[k++]='='; b[k++]='0'; b[k++]='x';
  char t[16]; int j=0; if(!v) t[j++]='0'; while(v){ t[j++]=h[v&0xf]; v>>=4; }
  while(j--) b[k++]=t[j]; b[k++]=']'; b[k++]='\n'; write(1,b,k);
}
static u64 env_num(const char *name, u64 dflt){
  char *v = getenv(name); if(!v) return dflt;
  u64 r=0; int any=0; while(*v>='0'&&*v<='9'){ r=r*10+(*v-'0'); v++; any=1; }
  return any?r:dflt;
}
static void helper_bc(const char *step){
  const char *p = getenv("MIB3_HELPER_BC");
  int fd;
  if (!p || !p[0]) p = "/dev/shm/mib3helper.bc";
  fd = open(p, 1 /*O_WRONLY*/|0100 /*O_CREAT*/|02000 /*O_APPEND*/, 0644);
  if (fd >= 0){ write(fd, step, slen_(step)); write(fd, "\n", 1); fsync(fd); close(fd); }
}

/* read exactly n bytes; 1 on success, 0 on EOF/error */
static int read_full(int fd, void *buf, unsigned long n){
  unsigned long got = 0;
  while (got < n){ long r = read(fd, (char*)buf + got, n - got); if (r <= 0) return 0; got += (unsigned long)r; }
  return 1;
}

void _start(void){
  const char *shim, *sock, *mode, *libs;
  void *h; int use_bridge, dry = 0, oneshot = 0;
  int w, hh; unsigned ivi;
  fn_create bcreate, screate;
  fn_feed   bfeed; fn_wf swf; fn_i0 sready;
  int lfd;

  P("[helper] start (out-of-process cluster sink)\n");

  shim = getenv("MIB3_HELPER_SHIM"); if (!shim || !shim[0]) shim = "/dev/shm/mib3shim.so";
  sock = getenv("MIB3_HELPER_SOCK"); if (!sock || !sock[0]) sock = MIB3_HELP_SOCK_DEFAULT;
  mode = getenv("MIB3_HELPER_SINK"); if (!mode || !mode[0]) mode = "bridge";
  use_bridge = (mode[0] == 'b');
  dry = (mode[0] == 'd');
  /* ONESHOT=1 restores the pre-2026-10-16 single-client-exit behaviour.  It exists for
   * the DESK BENCH (which waits for the helper to exit) and for one-shot debugging; the
   * UNIT default is durable (re-accept, keep ivi 3000 alive across shim reconnects). */
  oneshot = env_num("MIB3_HELPER_ONESHOT", 0) ? 1 : 0;

  /* bring the real media/graphics libs into the GLOBAL scope so the shim's lazy
   * dlsym(RTLD_DEFAULT) finds them (on the desk the stub is already preloaded). */
  libs = getenv("MIB3_HELPER_LIBS");
  if (libs == 0) libs = "libNmeSDK.so:libwayland-client.so.0:libwaylandegl.so.1:libEGL.so:libGLESv2.so";
  { const char *p = libs; char name[64];
    while (*p){
      int k = 0;
      while (*p && *p != ':' && k < 63) name[k++] = *p++;
      name[k] = 0;
      if (*p == ':') p++;
      if (k) { void *d = dlopen(name, RTLD_NOW | RTLD_GLOBAL); P("[helper] dlopen "); P(name);
               Pkv(" ->", (u64)d); }
    } }

  h = dlopen(shim, RTLD_NOW | RTLD_GLOBAL);
  Pkv("dlopen(shim)", (u64)h);
  if (!h){ P("[helper] cannot load the shim .so\n"); _exit(2); }

  bcreate = (fn_create) dlsym(RTLD_DEFAULT, "mib3shim_bridge_create");
  bfeed   = (fn_feed)   dlsym(RTLD_DEFAULT, "mib3shim_bridge_feed");
  screate = (fn_create) dlsym(RTLD_DEFAULT, "mib3shim_surface_create");
  swf     = (fn_wf)     dlsym(RTLD_DEFAULT, "mib3shim_surface_write_frame");
  sready  = (fn_i0)     dlsym(RTLD_DEFAULT, "mib3shim_surface_ready");
  Pkv("bridge_create", (u64)bcreate); Pkv("bridge_feed", (u64)bfeed);
  Pkv("surface_create", (u64)screate);
  if (!dry && (!bcreate || !bfeed) && (!screate || !swf)){
    P("[helper] shim sink exports missing\n"); _exit(3);
  }

  w  = (int) env_num("MIB3_SHIM_W", 1280);
  hh = (int) env_num("MIB3_SHIM_H", 480);
  ivi = (unsigned) env_num("MIB3_SHIM_IVIID", 0x4d4233);
  helper_bc("sink:create");
  if (dry){ P("[helper] DRY: creating nothing\n"); }
  else if (use_bridge){
    int rc = bcreate ? bcreate(w, hh, ivi) : -1;
    Pkv("bridge_create rc", (u64)(unsigned)rc);
    helper_bc(rc == 0 ? "sink:bridge:ok" : "sink:bridge:fail");
  } else {
    int rc = screate ? screate(w, hh, ivi) : -1;
    Pkv("surface_create rc", (u64)(unsigned)rc);
    helper_bc(rc == 0 ? "sink:surface:ok" : "sink:surface:fail");
  }

  /* AF_UNIX server */
  { struct { unsigned short fam; char path[108]; } sa; int i;
    for (i = 0; i < 108; i++) sa.path[i] = 0;
    for (i = 0; i < 107 && sock[i]; i++) sa.path[i] = sock[i];
    sa.fam = 1; /* AF_UNIX */
    unlink(sock);
    lfd = socket(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0);
    if (lfd < 0 || bind(lfd, &sa, sizeof(sa)) != 0 || listen(lfd, 4) != 0){
      P("[helper] socket setup FAILED\n"); _exit(4);
    }
    /* The shim runs INSIDE carplay, which is NOT root (uid 986): connecting to a
     * unix socket needs WRITE on the inode, and bind() creates it 0755 under the
     * default umask -> the shim's connect() gets EACCES and logs "helper absent".
     * Open it up here (a socket access check is mode, not owner). */
    if (chmod(sock, 0777) != 0) P("[helper] warn: chmod 0777 on socket failed\n");
    helper_bc("listen");
    P("[helper] listening (frames from the shim)\n");
  }

  /* Serve frames.  *** RE-ACCEPT on every client disconnect (2026-10-16 fix) ***
   * The old loop `break`ed out of accept when a client left, so the FIRST
   * shim disconnect (carplay re-inject / reconnect) exited the helper and
   * destroyed ivi surface 3000 -- a prime cause of the field's
   * "Specified surface (ID:3000) does not exist" / restartShare churn, because
   * whoever owned 3000 vanished under multi-screen-server.  The helper is the
   * DURABLE owner of the surface: the surface is created ONCE above and is kept
   * across any number of client lifetimes; only a real listener failure exits. */
  {
    unsigned long total = 0, bad = 0;
    unsigned char hdr[MIB3_HELP_HDRLEN];
    unsigned char *buf = 0; unsigned long cap = 0;
    int accept_fails = 0, clients = 0;
    for (;;){
      int c = accept(lfd, 0, 0);
      if (c < 0){
        /* a blocking accept() only fails on a transient signal or a broken
         * listener.  Tolerate a few transients, then give up (never busy-spin). */
        if (++accept_fails >= 8){ P("[helper] accept failed repeatedly -- exiting\n"); break; }
        P("[helper] accept retry\n");
        continue;
      }
      accept_fails = 0;
      clients++;
      Pkv("client #", (u64)(unsigned)clients);
      Pkv("client fd", (u64)(unsigned)c);
      P("[helper] client connected\n");
      for (;;){
        unsigned long len = 0; unsigned magic; int i; long r;
        /* read the 16-byte header (r==0 -> closed) */
        { unsigned long got = 0; int ok = 1;
          while (got < MIB3_HELP_HDRLEN){ r = read(c, hdr + got, MIB3_HELP_HDRLEN - got);
            if (r <= 0){ ok = 0; break; } got += (unsigned long)r; }
          if (!ok) break; }
        magic = 0; for (i = 0; i < 4; i++) magic |= (unsigned)hdr[i] << (i*8);
        for (i = 0; i < 4; i++) len |= (unsigned long)hdr[4+i] << (i*8);
        if (magic != MIB3_HELP_MAGIC || len == 0 || len > MIB3_HELP_MAXLEN){ bad++; break; }
        if (len > cap){ unsigned long ns = cap ? cap : 65536; while (ns < len) ns <<= 1;
          unsigned char *nb = (unsigned char*)malloc(ns); if (!nb){ bad++; break; }
          buf = nb; cap = ns; }
        if (!read_full(c, buf, len)){ bad++; break; }
        /* NB (2026-10-16): a monotonic timestamp was passed here ((total+1)*100 ms) to give
         * WriteVideo a non-zero pts/flags.  It went live in the batched build that REBOOTED the unit
         * on the next phone plug, so it is REVERTED to 0 until it can be verified alone. */
        if (use_bridge && bfeed) bfeed(buf, len, 0);
        else if (swf && sready && sready() && (long)len >= (long)w*(long)hh*4) swf(buf, w, hh);
        total++;
        if (total == 1 || (total % 100) == 0){ Pkv("frames", total); Pkv("len", len); }
      }
      close(c);
      Pkv("frames total", total); Pkv("bad", bad);
      if (oneshot){ P("[helper] client gone (ONESHOT -- exiting)\n"); helper_bc("client-gone:oneshot-exit"); break; }
      P("[helper] client gone (surface retained -- re-accepting)\n");
      helper_bc("client-gone:reaccept");
      continue;   /* keep the surface + listener alive; wait for the next client */
    }
    helper_bc("exit");
    Pkv("final frames", total);
  }
  _exit(0);
}
