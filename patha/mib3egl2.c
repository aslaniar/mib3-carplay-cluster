#define NULL ((void*)0)
/* ============================================================================
 * mib3egl2.c -- the DIAGNOSTIC EGL presenter for ivi surface 3000.
 *
 *   *** LIVE-ONLY. Runs ON THE UNIT, in its own process. ***
 *
 * WHY THIS EXISTS (LANE_CLUSTER_DELIVERY_IMPLEMENT_2026-10-16.md):
 *   mib3egl.c already gets a buffer onto ivi surface 3000 and multi-screen-server
 *   (MS-S) grabs+encodes it -- but ~250x slower than the nav's surface 33, and the
 *   cluster shows an undecodable frame.  Two candidate root causes survived the desk:
 *     (A) the BUFFER differs (format/modifier/heap): the nav's is a stable, directly
 *         shareable GPU dmabuf; ours is whatever the default eglChooseConfig yields.
 *     (B) the SHARE CHURNS: Weston repeatedly re-configures our surface, and MS-S's
 *         handle_share_surface_configure DESTROYS AND REBUILDS the whole encoder
 *         (OMX + Rgba2yuv + EGLImage) on EVERY configure -> ~250x.
 *   This tool makes both testable and observable:
 *     * it selects an EXPLICIT recognised colour config and prints the chosen
 *       config's EGL_NATIVE_VISUAL_ID -- which IS the wl_drm fourcc Weston sees
 *       ('AR24'=0x34325241 -> fmt 1, 'XR24'=0x34325258 -> fmt 2, 'AB24'=0x34324241
 *       -> fmt 0, anything else -> fmt 3/unknown, which MS-S then mishandles).
 *     * --hold commits ONE frame and never redraws (isolates "one configure" from
 *       per-frame churn);  --fps N controls the commit cadence (try 1 to test (B)).
 *     * it reuses one wl_egl_window + one EGL surface for its whole life, so no
 *       buffer churn is introduced by us.
 *
 * USAGE:
 *   XDG_RUNTIME_DIR=/run/platform/weston WAYLAND_DISPLAY=wayland-0 \
 *     mib3egl2 [ivi_id] [w] [h] [seconds] [colourRRGGBB] [--fps N|--hold|--frames N]
 *   defaults: id 3000, 1280x480, 120s, 00ff00, 10 fps.
 *
 * SUCCESS TEST (no eyes needed):  MS-S's :54321 log shows
 *   "handle_share_surface_configure [1280x480] stride ... fmt N"  (N tells you the fourcc)
 *   then "pass buffer (gles): avg time = X ms"  -- X ~3ms = fixed, X ~450ms = still slow.
 *
 * SAFETY: RAM-only; a crash here cannot touch carplay or multi-display.
 * ==========================================================================*/
typedef unsigned int  u32;
typedef int           i32;
typedef unsigned long usize;

struct wl_interface;
struct wl_message { const char *name; const char *signature; const struct wl_interface **types; };
struct wl_interface { const char *name; int version; int method_count;
                      const struct wl_message *methods; int event_count;
                      const struct wl_message *events; };

extern int    open(const char *, int, ...);
extern long   read(int, void *, unsigned long);
extern int    close(int);
extern void   _exit(int);
extern int    write(int, const void *, unsigned long);
extern unsigned int sleep(unsigned int);
extern int    usleep(unsigned int);
static int streq(const char *a, const char *b){ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void put(const char *s){ usize n=0; while (s[n]) n++; write(2, s, n); }
static void putnum(long v){ char b[24]; int i=0; if(v<0){write(2,"-",1);v=-v;} if(!v){write(2,"0",1);return;} while(v){b[i++]=(char)('0'+v%10);v/=10;} while(i) write(2,&b[--i],1); }
static void puthex(long v){ char b[16]; int i=0; if(!v){write(2,"0",1);return;} while(v){int d=(int)(v&0xf); b[i++]=(char)(d<10?'0'+d:'a'+d-10); v>>=4;} while(i) write(2,&b[--i],1); }
static void ok(const char*m){ put("[egl2] "); put(m); put("\n"); }
static void kv(const char*m,long v){ put("[egl2] "); put(m); put("="); putnum(v); put("\n"); }
static void kvx(const char*m,long v){ put("[egl2] "); put(m); put("=0x"); puthex(v); put("\n"); }
static int my_atoi(const char*s){ long v=0; int neg=0; if(*s=='-'){neg=1;s++;} while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return (int)(neg?-v:v); }
static long my_strtol16(const char*s){ long v=0; for(;*s;s++){ char c=*s; if(c>='0'&&c<='9')v=v*16+(c-'0'); else if(c>='a'&&c<='f')v=v*16+(c-'a'+10); else if(c>='A'&&c<='F')v=v*16+(c-'A'+10); else break; } return v; }

extern void  *wl_display_connect(const char *);
extern int    wl_display_roundtrip(void *);
extern int    wl_display_flush(void *);
extern int    wl_proxy_add_listener(void *, void **, void *);
extern void  *wl_proxy_marshal_constructor(void *, u32, const struct wl_interface *, ...);
extern void  *wl_proxy_marshal_constructor_versioned(void *, u32, const struct wl_interface *, u32, ...);
extern const struct wl_interface wl_compositor_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface wl_registry_interface;
extern void  *wl_egl_window_create(void *surface, int w, int h);
extern void  *eglGetDisplay(void *native_display);
extern int    eglInitialize(void *dpy, void *maj, void *min);
extern int    eglBindAPI(u32 api);
extern int    eglChooseConfig(void *dpy, const i32 *attribs, void **cfgs, i32 n, i32 *got);
extern int    eglGetConfigAttrib(void *dpy, void *cfg, i32 attr, i32 *val);
extern void  *eglCreateWindowSurface(void *dpy, void *cfg, void *win, const i32 *attribs);
extern void  *eglCreateContext(void *dpy, void *cfg, void *share, const i32 *attribs);
extern int    eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx);
extern int    eglSwapBuffers(void *dpy, void *surf);
extern int    eglSwapInterval(void *dpy, int interval);
extern const char *eglQueryString(void *dpy, int name);
extern void   glClearColor(float, float, float, float);
extern void   glClear(u32 mask);

#define EGL_OPENGL_ES_API   0x30A0
#define EGL_SURFACE_TYPE    0x3033
#define EGL_WINDOW_BIT      0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT  0x0004
#define EGL_NONE            0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_BUFFER_SIZE     0x3020
#define EGL_ALPHA_SIZE      0x3021
#define EGL_BLUE_SIZE       0x3022
#define EGL_GREEN_SIZE      0x3023
#define EGL_RED_SIZE        0x3024
#define EGL_CONFIG_ID       0x3028
#define EGL_CONFIG_CAVEAT   0x3027
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_VENDOR          0x3053
#define EGL_VERSION         0x3054
#define GL_COLOR_BUFFER_BIT 0x4000

/* --- ivi protocol (NOT exported by this image's libwayland -- declare locally) --- */
static const struct wl_interface *ivi_surface_ev_types[] = { NULL };
static const struct wl_message    ivi_surface_evs[]    = { { "configure", "uu", ivi_surface_ev_types } };
static const struct wl_interface  ivi_surface_iface     = { "ivi_surface", 1, 0, NULL, 1, ivi_surface_evs };
static const struct wl_interface *ivi_app_req_types[]  = { NULL, &wl_surface_interface, &ivi_surface_iface };
static const struct wl_message    ivi_app_reqs[]       = { { "surface_create", "uon", ivi_app_req_types } };
static const struct wl_interface  ivi_application_iface = { "ivi_application", 1, 1, ivi_app_reqs, 0, NULL };

static void *g_display, *g_registry, *g_compositor, *g_surface, *g_ivi;
static void *g_reg_listener[2];

static void reg_global(void *d, void *reg, u32 name, const char *iface, u32 version){
  u32 v = version; (void)d;
  if      (streq(iface, "wl_compositor"))   g_compositor = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_compositor_interface, v, name, "wl_compositor", v, NULL);
  else if (streq(iface, "ivi_application")) g_ivi        = wl_proxy_marshal_constructor_versioned(reg, 0, &ivi_application_iface, 1, name, "ivi_application", 1, NULL);
}
static void reg_global_remove(void *d, void *r, u32 n){ (void)d;(void)r;(void)n; }

int main(int argc, char **argv);
void _start(void){
  static char cbuf[1024]; static char *av[32];
  int n = 0, argc = 0, i = 0, fd = open("/proc/self/cmdline", 0);
  if (fd >= 0){ long r = read(fd, cbuf, (unsigned long)sizeof(cbuf)-1); if (r > 0) n = (int)r; close(fd); }
  cbuf[n] = 0;
  while (i < n && argc < 31){ av[argc++] = &cbuf[i]; while (i < n && cbuf[i]) i++; i++; }
  av[argc] = 0;
  _exit(main(argc, av));
}

int main(int argc, char **argv){
  u32 ivi_id = (argc > 1) ? (u32)my_atoi(argv[1]) : 3000u;
  int w = (argc > 2) ? my_atoi(argv[2]) : 1280;
  int h = (argc > 3) ? my_atoi(argv[3]) : 480;
  int secs = (argc > 4) ? my_atoi(argv[4]) : 120;
  long col = (argc > 5 && argv[5][0] != '-') ? my_strtol16(argv[5]) : 0x00ff00L;
  int fps = 10, hold = 0, maxframes = 0, i, nframes;
  float cr = (float)((col >> 16) & 0xff) / 255.0f, cg = (float)((col >> 8) & 0xff) / 255.0f, cb = (float)(col & 0xff) / 255.0f;
  void *dpy, *cfgs[16], *cfg = 0, *win, *esurf, *ctx; i32 ncfg = 0, v;
  /* EXPLICIT, RECOGNISED colour config: 8/8/8/8 RGBA, no caveat, ES2 window. */
  i32 cfg_attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                      EGL_CONFIG_CAVEAT, EGL_NONE, EGL_NONE };
  i32 ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

  for (i = 6; i < argc; i++){
    if (streq(argv[i], "--hold")) hold = 1;
    else if (streq(argv[i], "--fps") && i + 1 < argc) fps = my_atoi(argv[++i]);
    else if (streq(argv[i], "--frames") && i + 1 < argc) maxframes = my_atoi(argv[++i]);
  }
  if (fps < 1) fps = 1;

  g_display = wl_display_connect(NULL);
  if (!g_display){ ok("FAIL wl_display_connect"); return 2; }
  g_registry = wl_proxy_marshal_constructor(g_display, 1, &wl_registry_interface, NULL);
  g_reg_listener[0] = (void*)reg_global; g_reg_listener[1] = (void*)reg_global_remove;
  wl_proxy_add_listener(g_registry, g_reg_listener, NULL);
  wl_display_roundtrip(g_display);
  if (!g_compositor || !g_ivi){ ok("FAIL registry (need wl_compositor + ivi_application)"); return 3; }
  ok("registry bound");

  g_surface = wl_proxy_marshal_constructor(g_compositor, 0, &wl_surface_interface, NULL);
  if (!g_surface){ ok("FAIL create_surface"); return 4; }
  wl_proxy_marshal_constructor(g_ivi, 0, &ivi_surface_iface, ivi_id, g_surface, NULL);
  kv("ivi surface id", (long)ivi_id);

  dpy = eglGetDisplay(g_display);
  if (!dpy){ ok("FAIL eglGetDisplay"); return 5; }
  { i32 maj = 0, min = 0; if (eglInitialize(dpy, &maj, &min) == 0){ ok("FAIL eglInitialize"); return 5; } }
  put("[egl2] EGL vendor="); put(eglQueryString(dpy, EGL_VENDOR)); put(" version="); put(eglQueryString(dpy, EGL_VERSION)); put("\n");
  eglBindAPI(EGL_OPENGL_ES_API);
  if (eglChooseConfig(dpy, cfg_attrs, cfgs, 16, &ncfg) == 0 || ncfg < 1){ ok("FAIL eglChooseConfig (no 8888 ES2 window config?)"); return 6; }
  kv("configs offered", ncfg);
  /* report the first few configs -- EGL_NATIVE_VISUAL_ID is the wl_drm fourcc Weston sees */
  for (i = 0; i < ncfg && i < 4; i++){
    put("[egl2] cfg["); putnum(i); put("] ");
    eglGetConfigAttrib(dpy, cfgs[i], EGL_CONFIG_ID, &v);           put("id=0x"); puthex(v);
    eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &v);    put(" visual=0x"); puthex(v);
    eglGetConfigAttrib(dpy, cfgs[i], EGL_BUFFER_SIZE, &v);         put(" bufsz="); putnum(v);
    eglGetConfigAttrib(dpy, cfgs[i], EGL_ALPHA_SIZE, &v);          put(" a="); putnum(v);
    eglGetConfigAttrib(dpy, cfgs[i], EGL_CONFIG_CAVEAT, &v);       put(" caveat=0x"); puthex(v);
    put("\n");
    /* decode the visual as ASCII fourcc when it looks like one */
    eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &v);
    if ((v >> 24) >= 0x20 && ((v >> 16) & 0xff) >= 0x20 && ((v >> 8) & 0xff) >= 0x20 && (v & 0xff) >= 0x20){
      char f[5]; f[0]=(char)(v&0xff); f[1]=(char)((v>>8)&0xff); f[2]=(char)((v>>16)&0xff); f[3]=(char)((v>>24)&0xff); f[4]=0;
      put("[egl2]   fourcc='"); put(f); put("'  (AR24 -> fmt1, XR24 -> fmt2, AB24 -> fmt0, other -> fmt3/unknown)\n");
    }
  }
  cfg = cfgs[0];
  eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &v); kvx("chosen visual (wl_drm fourcc)", v);
  eglGetConfigAttrib(dpy, cfg, EGL_CONFIG_ID, &v);        kvx("chosen config id", v);
  eglGetConfigAttrib(dpy, cfg, EGL_ALPHA_SIZE, &v);       kv("chosen alpha size", v);

  win = wl_egl_window_create(g_surface, w, h);
  if (!win){ ok("FAIL wl_egl_window_create"); return 6; }
  esurf = eglCreateWindowSurface(dpy, cfg, win, NULL);
  if (!esurf){ ok("FAIL eglCreateWindowSurface"); return 6; }
  ctx = eglCreateContext(dpy, cfg, NULL, ctx_attrs);
  if (!ctx){ ok("FAIL eglCreateContext"); return 6; }
  if (eglMakeCurrent(dpy, esurf, esurf, ctx) == 0){ ok("FAIL eglMakeCurrent"); return 6; }
  eglSwapInterval(dpy, 0);         /* never block on the compositor's frame callback */
  ok("EGL surface + context current (dmabuf-backed); ONE window reused for the whole run");

  nframes = hold ? 1 : (maxframes > 0 ? maxframes : secs * fps);
  kv("frames to commit", nframes);
  kv("cadence (fps)", fps);
  for (i = 0; i < nframes; i++){
    glClearColor(cr, cg, cb, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(dpy, esurf);
    wl_display_flush(g_display);
    if ((i % 50) == 0) kv("frame", i);
    if (i + 1 < nframes) usleep((unsigned int)(1000000 / fps));
  }
  ok(hold ? "done (single commit -- one configure expected)" : "done");
  return 0;
}
