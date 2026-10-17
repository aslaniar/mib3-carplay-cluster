/* ============================================================================
 * mib3egl.c -- Wayland **EGL** presenter that owns an ivi surface (GPU/dmabuf buffer).
 *
 * WHY (2026-10-16): the SHM variant (mib3surface.c) DID get a buffer onto ivi surface
 * 3000 (ILM: 1280x480) -- but multi-screen-server still never received ivi_share's
 * `configure`, and a pure-black surface left the wire byte-identical (1113 B/dgram),
 * proving MS-S was encoding something else.  The share machinery hands out a **GPU
 * dmabuf**: a CPU/SHM buffer cannot be shared, so no configure ever arrives.  This
 * version renders through EGL, so the compositor gets a dmabuf-backed surface -- the
 * same mechanism the vendor uses -- and it is done in a clean process where a crash
 * cannot touch carplay or multi-display.
 *
 * USAGE: XDG_RUNTIME_DIR=/run/platform/weston WAYLAND_DISPLAY=wayland-0 \
 *          mib3egl <ivi_id> [width] [height] [seconds] [colourR,colourG,colourB hex]
 *
 * Success test (no eyes needed): the wire drops well below the car map's ~1110 B/dgram
 * when we show a flat colour, AND multi-screen-server logs handle_share_surface_configure.
 * ==========================================================================*/
#define NULL ((void*)0)
typedef unsigned int  u32;
typedef int           i32;
typedef unsigned long usize;

struct wl_interface;
struct wl_message { const char *name; const char *signature; const struct wl_interface **types; };
struct wl_interface { const char *name; int version; int method_count;
                      const struct wl_message *methods; int event_count;
                      const struct wl_message *events; };

/* --- libc bits we need (no headers on the desk) --- */
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
static void ok(const char*m){ put("[egl] "); put(m); put("\n"); }
static void num(const char*m,long v){ put("[egl] "); put(m); put("="); putnum(v); put("\n"); }
static int my_atoi(const char*s){ long v=0; int neg=0; if(*s=='-'){neg=1;s++;} while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return (int)(neg?-v:v); }
static long my_strtol16(const char*s){ long v=0; for(;*s;s++){ char c=*s; if(c>='0'&&c<='9')v=v*16+(c-'0'); else if(c>='a'&&c<='f')v=v*16+(c-'a'+10); else if(c>='A'&&c<='F')v=v*16+(c-'A'+10); else break; } return v; }

/* --- libwayland-client --- */
extern void  *wl_display_connect(const char *);
extern int    wl_display_roundtrip(void *);
extern int    wl_display_flush(void *);
extern int    wl_proxy_add_listener(void *, void **, void *);
extern void  *wl_proxy_marshal_constructor(void *, u32, const struct wl_interface *, ...);
extern void  *wl_proxy_marshal_constructor_versioned(void *, u32, const struct wl_interface *, u32, ...);
extern const struct wl_interface wl_compositor_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface wl_registry_interface;
/* --- libwayland-egl + EGL/GLES --- */
extern void  *wl_egl_window_create(void *surface, int w, int h);
extern void  *eglGetDisplay(void *native_display);
extern int    eglInitialize(void *dpy, void *maj, void *min);
extern int    eglBindAPI(u32 api);
extern int    eglChooseConfig(void *dpy, const i32 *attribs, void **cfgs, i32 n, i32 *got);
extern void  *eglCreateWindowSurface(void *dpy, void *cfg, void *win, const i32 *attribs);
extern void  *eglCreateContext(void *dpy, void *cfg, void *share, const i32 *attribs);
extern int    eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx);
extern int    eglSwapBuffers(void *dpy, void *surf);
extern void   glClearColor(float, float, float, float);
extern void   glClear(u32 mask);
#define EGL_OPENGL_ES_API   0x30A0
#define EGL_SURFACE_TYPE    0x3033
#define EGL_WINDOW_BIT      0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT  0x0004
#define EGL_NONE            0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define GL_COLOR_BUFFER_BIT 0x4000

/* --- ivi protocol (not exported on this image) --- */
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
  long col = (argc > 5) ? my_strtol16(argv[5]) : 0x00ff00L;
  int steady = (argc > 6);   /* steady = do not pulse */
  float cr = (float)((col >> 16) & 0xff) / 255.0f, cg = (float)((col >> 8) & 0xff) / 255.0f, cb = (float)(col & 0xff) / 255.0f;
  int i;
  void *dpy, *cfg = 0, *win, *esurf, *ctx; i32 ncfg = 0;
  i32 cfg_attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                      EGL_NONE };
  i32 ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

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
  num("ivi surface id", (long)ivi_id);

  dpy = eglGetDisplay(g_display);
  if (!dpy){ ok("FAIL eglGetDisplay"); return 5; }
  { i32 maj = 0, min = 0; if (eglInitialize(dpy, &maj, &min) == 0){ ok("FAIL eglInitialize"); return 5; } }
  eglBindAPI(EGL_OPENGL_ES_API);
  if (eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) == 0 || !cfg){ ok("FAIL eglChooseConfig"); return 6; }
  win = wl_egl_window_create(g_surface, w, h);
  if (!win){ ok("FAIL wl_egl_window_create"); return 6; }
  esurf = eglCreateWindowSurface(dpy, cfg, win, NULL);
  if (!esurf){ ok("FAIL eglCreateWindowSurface"); return 6; }
  ctx = eglCreateContext(dpy, cfg, NULL, ctx_attrs);
  if (!ctx){ ok("FAIL eglCreateContext"); return 6; }
  if (eglMakeCurrent(dpy, esurf, esurf, ctx) == 0){ ok("FAIL eglMakeCurrent"); return 6; }
  ok("EGL surface + context current (dmabuf-backed)");

  for (i = 0; i < secs * 10; i++){
    /* pulse the colour so the encode is obviously OURS and always has damage */
    float k = steady ? 1.0f : ((i % 20 < 10) ? 1.0f : 0.25f);
    glClearColor(cr * k, cg * k, cb * k, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(dpy, esurf);
    wl_display_flush(g_display);
    usleep(100000);
    if ((i % 50) == 0) num("frame", i);
  }
  ok("done");
  return 0;
}
