#define NULL ((void*)0)
/* ============================================================================
 * mib3surface.c -- minimal Wayland **SHM** presenter that owns an ivi surface.
 *
 *   *** LIVE-ONLY. Runs ON THE UNIT. Own process: a crash here cannot touch
 *       carplay or multi-display. ***
 *
 * WHY (2026-10-16, LANE_PATH_A_SURFACES_MAP_2026-10-16.md S7j): the delivery wall is
 * that LG's Cinemo decoder accepts our H.264 but never attaches a buffer to the ivi
 * surface it created, so ivi_share has nothing to hand multi-screen-server.  A surface
 * only needs a buffer attached by *any* Wayland client.  This commits **SHM** buffers
 * (plain shared memory, CPU memcpy -- no EGL, no GL), so nothing can fail on EGL config
 * selection.  Step 1 paints a moving test pattern: it proves the delivery path with zero
 * dependency on the decoder; feeding real pixels afterwards is then a decode+memcpy job.
 *
 * USAGE:  XDG_RUNTIME_DIR=/run/platform/weston WAYLAND_DISPLAY=wayland-0 \
 *           mib3surface <ivi_id> [width] [height] [seconds]
 *
 * The ivi protocol structs are defined HERE: libwayland-client on this image does NOT
 * export ivi_application_interface (which is why the shim's own surface path silently
 * skipped the bind and never applied its id).
 * ==========================================================================*/
/* no target headers on the desk: declare exactly what we use */
extern int    open(const char *, int, ...);
extern long   read(int, void *, unsigned long);
extern int    close(int);
extern void   _exit(int);
extern int    write(int, const void *, unsigned long);
extern int    ftruncate(int, long);
extern void  *mmap(void *, unsigned long, int, int, int, long);
extern unsigned int sleep(unsigned int);
extern int    usleep(unsigned int);
extern int    fcntl(int, int, ...);
extern int    dup(int);
#define O_RDWR_   02
#define O_CREAT_  0100
#define O_TRUNC_  01000
#define PROT_RW_  3
#define MAP_SHARED_ 1
#define MAP_FAILED_ ((void*)-1)

static int streq(const char *a, const char *b){ while (*a && *a == *b) { a++; b++; } return *a == *b; }

typedef unsigned int  u32;
typedef int           i32;
typedef unsigned long usize;

struct wl_interface;
struct wl_message { const char *name; const char *signature; const struct wl_interface **types; };
struct wl_interface { const char *name; int version; int method_count;
                      const struct wl_message *methods; int event_count;
                      const struct wl_message *events; };

extern void  *wl_display_connect(const char *name);
extern int    wl_display_roundtrip(void *display);
extern int    wl_display_flush(void *display);
extern int    wl_proxy_add_listener(void *proxy, void **impl, void *data);
extern void  *wl_proxy_marshal_constructor(void *proxy, u32 opcode, const struct wl_interface *iface, ...);
extern void  *wl_proxy_marshal_constructor_versioned(void *proxy, u32 opcode, const struct wl_interface *iface, u32 version, ...);
extern const struct wl_interface wl_compositor_interface;
extern const struct wl_interface wl_shm_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface wl_registry_interface;
extern const struct wl_interface wl_shm_pool_interface;
extern const struct wl_interface wl_buffer_interface;
extern void *wl_proxy_marshal(void *proxy, u32 opcode, ...);
#define OP_ATTACH 1
#define OP_DAMAGE 2
#define OP_COMMIT 6

/* ---- output helpers (no libc printf dependency) ---- */
static void put(const char *s){ usize n=0; while (s[n]) n++; write(2, s, n); }
static void putnum(long v){
  char b[24]; int i=0; if (v<0){ write(2,"-",1); v=-v; }
  if (!v) { write(2,"0",1); return; }
  while (v) { b[i++]=(char)('0'+v%10); v/=10; }
  while (i) write(2, &b[--i], 1);
}
static int my_atoi(const char *s){ long v=0; int neg=0; if (*s=='-'){neg=1;s++;} while (*s>='0'&&*s<='9'){ v=v*10+(*s-'0'); s++; } return (int)(neg?-v:v); }
static void ok(const char *m){ put("[surface] "); put(m); put("\n"); }
static void num(const char *m, long v){ put("[surface] "); put(m); put("="); putnum(v); put("\n"); }

/* ---- the ivi protocol, defined locally ---- */
static const struct wl_interface *ivi_surface_ev_types[] = { NULL };
static const struct wl_message    ivi_surface_evs[]    = { { "configure", "uu", ivi_surface_ev_types } };
static const struct wl_interface  ivi_surface_iface     = { "ivi_surface", 1, 0, NULL, 1, ivi_surface_evs };
static const struct wl_interface *ivi_app_req_types[]  = { NULL, &wl_surface_interface, &ivi_surface_iface };
static const struct wl_message    ivi_app_reqs[]       = { { "surface_create", "uon", ivi_app_req_types } };
static const struct wl_interface  ivi_application_iface = { "ivi_application", 1, 1, ivi_app_reqs, 0, NULL };

static void *g_display, *g_registry, *g_compositor, *g_shm, *g_surface, *g_ivi;
static u32 *g_pix; static void *g_buffer; static int g_w = 1280, g_h = 480, g_stride;
static void *g_reg_listener[2];

static void reg_global(void *data, void *reg, u32 name, const char *iface, u32 version){
  (void)data;
  u32 v = version;
  put("[surface] global: "); put(iface); put(" v"); putnum((long)version); put("\n");
  if      (streq(iface, "wl_compositor"))  g_compositor = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_compositor_interface, v, name, "wl_compositor", v, NULL);
  else if (streq(iface, "wl_shm"))         g_shm        = wl_proxy_marshal_constructor_versioned(reg, 0, &wl_shm_interface, v, name, "wl_shm", v, NULL);
  else if (streq(iface, "ivi_application"))g_ivi        = wl_proxy_marshal_constructor_versioned(reg, 0, &ivi_application_iface, v, name, "ivi_application", v, NULL);
}
static void reg_global_remove(void *d, void *r, u32 n){ (void)d;(void)r;(void)n; }

static int g_black = 0;
static void paint(int f){
  int x, y;
  if (g_black) { int n = g_w * g_h; while (n--) g_pix[n] = 0; return; }
  for (y = 0; y < g_h; y++)
    for (x = 0; x < g_w; x++) {
      u32 r = (u32)((x + f * 12) & 0xff);
      u32 g = (u32)((y * 255) / (g_h ? g_h : 1));
      u32 b = (u32)((f * 6) & 0xff);
      g_pix[y * (g_stride / 4) + x] = (r << 16) | (g << 8) | b;
    }
}

int main(int argc, char **argv);

/* no crt on this image: read our argv from /proc/self/cmdline (the helper's _start takes none) */
extern int   open(const char *, int, ...);
extern long  read(int, void *, unsigned long);
extern int   close(int);
extern void  _exit(int);
void _start(void){
  static char cbuf[1024]; static char *av[32];
  int n = 0, argc = 0, i = 0, fd = open("/proc/self/cmdline", 0);
  if (fd >= 0) { long r = read(fd, cbuf, (unsigned long)sizeof(cbuf) - 1); if (r > 0) n = (int)r; close(fd); }
  cbuf[n] = 0;
  while (i < n && argc < 31) { av[argc++] = &cbuf[i]; while (i < n && cbuf[i]) i++; i++; }
  av[argc] = 0;
  /* busybox `nohup` drops argv[0] (execvp(argv[1], argv+1)); a numeric first token
   * means the program name is missing -> shift so positionals stay aligned. */
  { const char *t = av[0];
    if (t && t[0] >= '0' && t[0] <= '9'){
      const char *p; int all = 1;
      for (p = t; *p; p++) if (*p < '0' || *p > '9'){ all = 0; break; }
      if (all && argc < 31){ int _j; for (_j = argc; _j > 0; _j--) av[_j] = av[_j - 1];
                             av[0] = "x"; argc++; av[argc] = 0; } } }
  _exit(main(argc, av));
}

int main(int argc, char **argv){
  u32 ivi_id = (argc > 1) ? (u32)my_atoi(argv[1]) : 3000u;
  int secs = (argc > 4) ? my_atoi(argv[4]) : 120;
  int i, fd, size;
  if (argc > 2) g_w = my_atoi(argv[2]);
  if (argc > 3) g_h = my_atoi(argv[3]);
  if (argc > 5 && streq(argv[5], "black")) g_black = 1;
  if (argc > 6) g_black = 1;
  g_stride = g_w * 4;

  g_display = wl_display_connect(NULL);
  if (!g_display){ ok("FAIL wl_display_connect"); return 2; }
  g_registry = wl_proxy_marshal_constructor(g_display, 1, &wl_registry_interface, NULL);
  g_reg_listener[0] = (void*)reg_global; g_reg_listener[1] = (void*)reg_global_remove;
  wl_proxy_add_listener(g_registry, g_reg_listener, NULL);
  wl_display_roundtrip(g_display);
  if (!g_compositor){ ok("FAIL no wl_compositor"); return 3; }
  if (!g_shm)       { ok("FAIL no wl_shm");        return 3; }
  if (!g_ivi)       { ok("FAIL no ivi_application");return 3; }
  ok("registry: compositor + shm + ivi_application bound");

  g_surface = wl_proxy_marshal_constructor(g_compositor, 0, &wl_surface_interface, NULL);
  if (!g_surface){ ok("FAIL create_surface"); return 4; }
  wl_proxy_marshal_constructor(g_ivi, 0, &ivi_surface_iface, ivi_id, g_surface, NULL);
  num("ivi surface id", (long)ivi_id);

  size = g_stride * g_h;
  fd = open("/dev/shm/mib3surface.buf", O_RDWR_ | O_CREAT_ | O_TRUNC_, 0600);
  if (fd < 0){ ok("FAIL open shm file"); return 5; }
  num("shm fd", (long)fd);
  { int f1 = fcntl(fd, 1 /*F_GETFD*/, 0); int d1 = dup(fd);
    num("fcntl(F_GETFD)", (long)f1); num("dup(fd) ->", (long)d1); if (d1 >= 0) close(d1); }
  if (ftruncate(fd, size) != 0){ ok("FAIL ftruncate"); return 5; }
  g_pix = (u32*)mmap(NULL, size, PROT_RW_, MAP_SHARED_, fd, 0);
  if (g_pix == MAP_FAILED_){ ok("FAIL mmap"); return 5; }
  { void *pool = wl_proxy_marshal_constructor(g_shm, 0, &wl_shm_pool_interface, NULL /*new_id*/, fd, size, NULL);
    if (!pool){ ok("FAIL shm_create_pool"); return 6; }
    g_buffer = wl_proxy_marshal_constructor(pool, 0, &wl_buffer_interface, NULL /*new_id*/, 0, g_w, g_h, g_stride, 1 /*XRGB8888*/, NULL);
    if (!g_buffer){ ok("FAIL shm_pool_create_buffer"); return 6; } }
  { int k; put("[surface] wl_shm iface: "); put(wl_shm_interface.name); put(" v"); putnum(wl_shm_interface.version);
    put(" methods="); putnum(wl_shm_interface.method_count); put("\n");
    for (k = 0; k < wl_shm_interface.method_count; k++) {
      put("[surface]   method "); putnum(k); put(": "); put(wl_shm_interface.methods[k].name);
      put(" sig="); put(wl_shm_interface.methods[k].signature); put("\n");
    } }
  ok("shm buffer ready");

  sleep(1);
  for (i = 0; i < secs * 10; i++) {
    paint(i);
    wl_proxy_marshal(g_surface, OP_ATTACH, g_buffer, 0, 0);
    wl_proxy_marshal(g_surface, OP_DAMAGE, 0, 0, g_w, g_h);
    wl_proxy_marshal(g_surface, OP_COMMIT);
    wl_display_flush(g_display);
    usleep(100000);
    if ((i % 50) == 0) num("frame", i);
  }
  ok("done");
  return 0;
}
