/* ============================================================================
 * wl_trace.c -- a tiny interposer that logs the DRAWING calls a process makes.
 *
 * Loaded into OUR OWN helper process ONLY (first entry in MIB3_HELPER_LIBS, so it
 * enters the global scope with RTLD_GLOBAL before the graphics libs load) -- NEVER
 * into carplay.  Its job: answer the one question we could not answer from outside
 * -- when the vendor's video device is handed our frames, does it ever create a
 * drawing target, and does it ever draw on it?
 *
 * SYMBOL RESOLUTION (important, cost a debugging cycle): dlsym(RTLD_NEXT) does NOT
 * work from a dlopen'd library, so every forward returned NULL and broke the client
 * (CinemoCreateWindow failed).  Instead each real function is resolved from an
 * explicit handle to its own library (dlopen by soname, already loaded -> cheap).
 * That is deterministic and cannot recurse into our own interposer.
 *
 * LOGS to $MIB3_TRACE_LOG (default /dev/shm/wltrace.log), one line per call.
 * READING IT -- the tell is eglSwapBuffers / wl_surface "commit":
 *   no eglCreateWindowSurface at all -> the device never made a drawable;
 *   a drawable but ZERO swaps        -> it decodes into a chain it never presents
 *                                       (NmeSwapChain "Dummy"/"Painter");
 *   swaps happening                  -> it IS drawing, to somewhere else.
 *
 * Built -nostdlib against the unit's own glibc (a library: no crt, no _start).
 * This is a diagnostic: if a real symbol is missing it still forwards nothing and
 * returns 0 rather than crashing.
 * ==========================================================================*/
typedef unsigned int  u32;
typedef unsigned long u64;

extern void  *dlopen(const char *path, int mode);
extern void  *dlsym(void *handle, const char *name);
extern char  *getenv(const char *name);
extern int    open(const char *path, int flags, ...);
extern long   write(int fd, const void *buf, unsigned long n);

#define RTLD_LAZY 1
#define O_WRONLY  1
#define O_CREAT   0100
#define O_APPEND  02000

static void *h_wl  = 0;   /* libwayland-client */
static void *h_egl = 0;   /* libEGL            */

static int  g_fd = -1;
static int  g_tried = 0;
static int  g_in = 0;     /* re-entrancy guard */

static unsigned slen_(const char *s){ unsigned n = 0; while (s && s[n]) n++; return n; }

static void *real_of(void **h, const char *lib, const char *sym){
  if (!*h) *h = dlopen(lib, RTLD_LAZY);
  return *h ? dlsym(*h, sym) : 0;
}
static const char *CLS(void *proxy){          /* wl_proxy_get_class, from libwayland */
  static const char *(*rf)(void*);
  if (!rf) rf = (const char *(*)(void*)) real_of(&h_wl, "libwayland-client.so.0", "wl_proxy_get_class");
  return rf ? rf(proxy) : "?";
}

static void log_open(void){
  char *p;
  if (g_fd >= 0 || g_tried) return;
  g_tried = 1;
  p = getenv("MIB3_TRACE_LOG");
  if (!p || !p[0]) p = "/dev/shm/wltrace.log";
  g_fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
}
static void P(const char *s){ if (g_fd >= 0) write(g_fd, s, slen_(s)); }
static void Pv(unsigned long v){
  char b[24]; int i = 0;
  if (g_fd < 0) return;
  if (!v){ write(g_fd, "0", 1); return; }
  while (v){ b[i++] = (char)('0' + (v % 10)); v /= 10; }
  while (i) write(g_fd, &b[--i], 1);
}
static void ev(const char *tag, unsigned long a, unsigned long b){
  if (g_in) return;
  g_in = 1;
  log_open();
  if (g_fd >= 0){ P(tag); P(" a="); Pv(a); P(" b="); Pv(b); P("\n"); }
  g_in = 0;
}

/* ------------------------------- EGL -------------------------------------- */
void *eglGetDisplay(void *native){
  static void *(*rf)(void*);
  if (!rf) rf = (void *(*)(void*)) real_of(&h_egl, "libEGL.so", "eglGetDisplay");
  ev("eglGetDisplay", (u64)native, 0);
  return rf ? rf(native) : 0;
}
int eglInitialize(void *dpy, void *maj, void *min){
  static int (*rf)(void*,void*,void*);
  if (!rf) rf = (int (*)(void*,void*,void*)) real_of(&h_egl, "libEGL.so", "eglInitialize");
  ev("eglInitialize", (u64)dpy, 0);
  return rf ? rf(dpy, maj, min) : 0;
}
int eglChooseConfig(void *dpy, const int *attrs, void **cfgs, int n, int *got){
  static int (*rf)(void*,const int*,void**,int,int*);
  if (!rf) rf = (int (*)(void*,const int*,void**,int,int*)) real_of(&h_egl, "libEGL.so", "eglChooseConfig");
  ev("eglChooseConfig", (u64)dpy, (u64)n);
  return rf ? rf(dpy, attrs, cfgs, n, got) : 0;
}
void *eglCreateWindowSurface(void *dpy, void *cfg, void *win, const int *attrs){
  static void *(*rf)(void*,void*,void*,const int*);
  if (!rf) rf = (void *(*)(void*,void*,void*,const int*)) real_of(&h_egl, "libEGL.so", "eglCreateWindowSurface");
  ev("eglCreateWindowSurface", (u64)win, 0);
  return rf ? rf(dpy, cfg, win, attrs) : 0;
}
void *eglCreateContext(void *dpy, void *cfg, void *share, const int *attrs){
  static void *(*rf)(void*,void*,void*,const int*);
  if (!rf) rf = (void *(*)(void*,void*,void*,const int*)) real_of(&h_egl, "libEGL.so", "eglCreateContext");
  ev("eglCreateContext", (u64)dpy, 0);
  return rf ? rf(dpy, cfg, share, attrs) : 0;
}
int eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx){
  static int (*rf)(void*,void*,void*,void*);
  if (!rf) rf = (int (*)(void*,void*,void*,void*)) real_of(&h_egl, "libEGL.so", "eglMakeCurrent");
  ev("eglMakeCurrent", (u64)draw, (u64)ctx);
  return rf ? rf(dpy, draw, read, ctx) : 0;
}
int eglSwapBuffers(void *dpy, void *surf){
  static int (*rf)(void*,void*);
  if (!rf) rf = (int (*)(void*,void*)) real_of(&h_egl, "libEGL.so", "eglSwapBuffers");
  ev("eglSwapBuffers", (u64)surf, 0);
  return rf ? rf(dpy, surf) : 0;
}
int eglSwapInterval(void *dpy, int interval){
  static int (*rf)(void*,int);
  if (!rf) rf = (int (*)(void*,int)) real_of(&h_egl, "libEGL.so", "eglSwapInterval");
  ev("eglSwapInterval", (u64)interval, 0);
  return rf ? rf(dpy, interval) : 0;
}
int eglTerminate(void *dpy){
  static int (*rf)(void*);
  if (!rf) rf = (int (*)(void*)) real_of(&h_egl, "libEGL.so", "eglTerminate");
  ev("eglTerminate", (u64)dpy, 0);
  return rf ? rf(dpy) : 0;
}

/* ------------------------------- GLES ------------------------------------- */
/* Does the device ever PAINT?  glTexSubImage2D/glDrawArrays would show it rendering
 * into a texture even if it never swaps -- that is the difference between "its
 * renderer never gets a frame" and "it renders but never presents". */
static void *glh(void){
  static void *h; 
  if (!h) h = dlopen("libGLESv2.so", RTLD_LAZY);
  if (!h) h = dlopen("libGLESv2.so.2", RTLD_LAZY);
  return h;
}
#define GLFN(fn, ret, args) \
  static ret (*fn) args; \
  if (!fn) fn = (ret (*) args) dlsym(glh(), #fn);
void glClear(unsigned mask){
  GLFN(glClear, void, (unsigned))
  ev("glClear", (u64)mask, 0); if (glClear) glClear(mask);
}
void glClearColor(float r, float g, float b, float a){
  GLFN(glClearColor, void, (float,float,float,float))
  ev("glClearColor", 0, 0); if (glClearColor) glClearColor(r,g,b,a);
}
void glDrawArrays(unsigned mode, int first, int count){
  GLFN(glDrawArrays, void, (unsigned,int,int))
  ev("glDrawArrays", (u64)mode, (u64)count); if (glDrawArrays) glDrawArrays(mode, first, count);
}
void glBindTexture(unsigned tgt, unsigned tex){
  GLFN(glBindTexture, void, (unsigned,unsigned))
  ev("glBindTexture", (u64)tgt, (u64)tex); if (glBindTexture) glBindTexture(tgt, tex);
}
void glViewport(int x, int y, int w, int h){
  GLFN(glViewport, void, (int,int,int,int))
  ev("glViewport", (u64)w, (u64)h); if (glViewport) glViewport(x, y, w, h);
}
void glTexSubImage2D(unsigned tgt, int lvl, int xo, int yo, int w, int h,
                     unsigned fmt, unsigned ty, const void *px){
  GLFN(glTexSubImage2D, void, (unsigned,int,int,int,int,int,unsigned,unsigned,const void*))
  ev("glTexSubImage2D", (u64)w, (u64)h);
  if (glTexSubImage2D) glTexSubImage2D(tgt,lvl,xo,yo,w,h,fmt,ty,px);
}
void glFinish(void){
  GLFN(glFinish, void, (void))
  ev("glFinish", 0, 0); if (glFinish) glFinish();
}

/* ----------------------------- Wayland ------------------------------------ */
void *wl_display_connect(const char *name){
  static void *(*rf)(const char*);
  if (!rf) rf = (void *(*)(const char*)) real_of(&h_wl, "libwayland-client.so.0", "wl_display_connect");
  ev("wl_display_connect", (u64)name, 0);
  return rf ? rf(name) : 0;
}
int wl_display_flush(void *d){
  static int (*rf)(void*);
  if (!rf) rf = (int (*)(void*)) real_of(&h_wl, "libwayland-client.so.0", "wl_display_flush");
  ev("wl_display_flush", (u64)d, 0);
  return rf ? rf(d) : -1;
}
int wl_display_roundtrip(void *d){
  static int (*rf)(void*);
  if (!rf) rf = (int (*)(void*)) real_of(&h_wl, "libwayland-client.so.0", "wl_display_roundtrip");
  ev("wl_display_roundtrip", (u64)d, 0);
  return rf ? rf(d) : -1;
}
int wl_display_dispatch(void *d){
  static int (*rf)(void*);
  if (!rf) rf = (int (*)(void*)) real_of(&h_wl, "libwayland-client.so.0", "wl_display_dispatch");
  ev("wl_display_dispatch", (u64)d, 0);
  return rf ? rf(d) : -1;
}
void wl_display_disconnect(void *d){
  static void (*rf)(void*);
  if (!rf) rf = (void (*)(void*)) real_of(&h_wl, "libwayland-client.so.0", "wl_display_disconnect");
  ev("wl_display_disconnect", (u64)d, 0);
  if (rf) rf(d);
}

/* the non-variadic marshalling core (every wl_surface attach/damage/commit) */
void *wl_proxy_marshal_array_flags(void *proxy, u32 opcode, const void *iface,
                                   u32 version, u32 flags, void *args){
  static void *(*rf)(void*,u32,const void*,u32,u32,void*);
  if (!rf) rf = (void *(*)(void*,u32,const void*,u32,u32,void*))
                 real_of(&h_wl, "libwayland-client.so.0", "wl_proxy_marshal_array_flags");
  { const char *c = CLS(proxy);
    if (g_in == 0){ g_in = 1; log_open();
      if (g_fd >= 0){ P("wl_marshal class="); P(c ? c : "?"); P(" op="); Pv(opcode); P("\n"); }
      g_in = 0; } }
  return rf ? rf(proxy, opcode, iface, version, flags, args) : 0;
}
