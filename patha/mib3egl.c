#define NULL ((void*)0)
/* ============================================================================
 * mib3egl.c -- Wayland **EGL** presenter that owns an ivi surface (GPU/dmabuf buffer).
 *
 *   *** LIVE-ONLY. Runs ON THE UNIT, in its own process. ***
 *
 * WHY (2026-10-16): the SHM variant (mib3surface.c) commits a CPU buffer; the share
 * machinery hands out a **GPU dmabuf**, so this version renders through EGL and the
 * compositor gets a dmabuf-backed surface -- the same mechanism the vendor uses.
 *
 * 2026-10-16 (LANE_CLUSTER_DELIVERY_E2E) -- PERSISTENCE + REAL PIXELS.
 *   The delivery defect is LIFECYCLE: the old presenter was a *timed* process and the
 *   helper exited on client disconnect, so ivi surface 3000 was repeatedly created and
 *   destroyed under multi-screen-server -> its ivi_share churned -> MS-S's
 *   handle_share_surface_configure (which DESTROYS+REBUILDS the encoder) fired over and
 *   over -> the ECU got an undecodable frame.  So now:
 *     * DEFAULT IS PERSISTENT -- this process owns ivi 3000 and never self-terminates
 *       while the share is live.  A positive `secs`, or `--frames N`, bounds it (tests).
 *     * ONE wl_egl_window + ONE EGL surface + ONE GL texture, reused for the whole life
 *       of the process: no per-commit buffer churn of our own.
 *     * It presents REAL PIXELS from a texture instead of a flat colour:
 *         --rgbafile PATH   load w*h*4 raw RGBA once and present it (a decoded frame)
 *         --rgba-sock PATH   SERVE an AF_UNIX socket and present each w*h*4 RGBA frame
 *                            received (the decode-to-texture DELIVERY half: any decoder
 *                            -- the unit's vendor decoder, or a desk decoder -- writes
 *                            frames here; see shim/tools/h264ipcm.py)
 *
 * USAGE:
 *   XDG_RUNTIME_DIR=/run/platform/weston WAYLAND_DISPLAY=wayland-0 \
 *     mib3egl [ivi_id] [w] [h] [secs] [colourRRGGBB] \
 *             [--persist] [--fps N] [--hold] [--frames N] \
 *             [--rgbafile F | --rgba-sock S]
 *   defaults: id 3000, 1280x480, PERSISTENT, 00ff00, 10 fps.
 *
 * Success test (no eyes needed): the ILM scene shows surface 3000 with
 * `original size 1280x480` / `source region 1280x480`, MS-S logs ONE
 * `handle_share_surface_configure` (not a storm) and `pass buffer (gles)` ~3 ms.
 * ==========================================================================*/
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
extern int    usleep(unsigned int);
extern void  *malloc(unsigned long);
extern void   free(void *);
extern int    socket(int, int, int);
extern int    bind(int, const void *, unsigned int);
extern int    listen(int, int);
extern int    accept(int, void *, unsigned int *);
extern int    unlink(const char *);
extern int    chmod(const char *, unsigned);
extern int    fcntl(int, int, ...);
static int streq(const char *a, const char *b){ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void put(const char *s){ usize n=0; while (s[n]) n++; write(2, s, n); }
static void putnum(long v){ char b[24]; int i=0; if(v<0){write(2,"-",1);v=-v;} if(!v){write(2,"0",1);return;} while(v){b[i++]=(char)('0'+v%10);v/=10;} while(i) write(2,&b[--i],1); }
static void puthex(long v){ char b[16]; int i=0; if(!v){write(2,"0",1);return;} while(v){int d=(int)(v&0xf); b[i++]=(char)(d<10?'0'+d:'a'+d-10); v>>=4;} while(i) write(2,&b[--i],1); }
static void ok(const char*m){ put("[egl] "); put(m); put("\n"); }
static void num(const char*m,long v){ put("[egl] "); put(m); put("="); putnum(v); put("\n"); }
static void numx(const char*m,long v){ put("[egl] "); put(m); put("=0x"); puthex(v); put("\n"); }
static int my_atoi(const char*s){ long v=0; int neg=0; if(*s=='-'){neg=1;s++;} while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return (int)(neg?-v:v); }
static long my_strtol16(const char*s){ long v=0; for(;*s;s++){ char c=*s; if(c>='0'&&c<='9')v=v*16+(c-'0'); else if(c>='a'&&c<='f')v=v*16+(c-'a'+10); else if(c>='A'&&c<='F')v=v*16+(c-'A'+10); else break; } return v; }
static int read_full(int fd, void *buf, unsigned long n){
  unsigned long got=0; while (got<n){ long r=read(fd,(char*)buf+got,n-got); if(r<=0) return 0; got+=(unsigned long)r; } return 1;
}

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
extern int    eglGetConfigAttrib(void *dpy, void *cfg, i32 attr, i32 *val);
extern void  *eglCreateWindowSurface(void *dpy, void *cfg, void *win, const i32 *attribs);
extern void  *eglCreateContext(void *dpy, void *cfg, void *share, const i32 *attribs);
extern int    eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx);
extern int    eglSwapBuffers(void *dpy, void *surf);
extern int    eglSwapInterval(void *dpy, int interval);
extern const char *eglQueryString(void *dpy, int name);
extern void   glClearColor(float, float, float, float);
extern void   glClear(u32 mask);
extern void   glViewport(int, int, int, int);
extern unsigned glCreateShader(u32 type);
extern void   glShaderSource(unsigned sh, int count, const char *const *str, const int *len);
extern void   glCompileShader(unsigned sh);
extern void   glGetShaderiv(unsigned sh, u32 pname, int *out);
extern unsigned glCreateProgram(void);
extern void   glAttachShader(unsigned prog, unsigned sh);
extern void   glLinkProgram(unsigned prog);
extern void   glGetProgramiv(unsigned prog, u32 pname, int *out);
extern void   glUseProgram(unsigned prog);
extern int    glGetAttribLocation(unsigned prog, const char *name);
extern void   glEnableVertexAttribArray(unsigned idx);
extern void   glVertexAttribPointer(unsigned idx, int size, u32 type, unsigned char norm, int stride, const void *ptr);
extern int    glGetUniformLocation(unsigned prog, const char *name);
extern void   glUniform1i(int loc, int v);
extern void   glGenTextures(int n, unsigned *out);
extern void   glBindTexture(u32 target, unsigned tex);
extern void   glTexParameteri(u32 target, u32 pname, int param);
extern void   glTexImage2D(u32 target, int level, int ifmt, int w, int h, int border, u32 fmt, u32 type, const void *px);
extern void   glTexSubImage2D(u32 target, int level, int x, int y, int w, int h, u32 fmt, u32 type, const void *px);
extern void   glDrawArrays(u32 mode, int first, int count);
extern unsigned glGetError(void);

#define EGL_OPENGL_ES_API   0x30A0
#define EGL_SURFACE_TYPE    0x3033
#define EGL_WINDOW_BIT      0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT  0x0004
#define EGL_NONE            0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_RED_SIZE        0x3024
#define EGL_GREEN_SIZE      0x3023
#define EGL_BLUE_SIZE       0x3022
#define EGL_ALPHA_SIZE      0x3021
#define EGL_CONFIG_CAVEAT   0x3027
#define EGL_NATIVE_VISUAL_ID 0x302E
#define GL_COLOR_BUFFER_BIT 0x4000
/* GLES2 */
#define GL_VERTEX_SHADER    0x8B31
#define GL_FRAGMENT_SHADER  0x8B30
#define GL_COMPILE_STATUS   0x8B81
#define GL_LINK_STATUS      0x8B82
#define GL_TEXTURE_2D       0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_LINEAR           0x2601
#define GL_RGBA             0x1908
#define GL_UNSIGNED_BYTE    0x1401
#define GL_FLOAT            0x1406
#define GL_TRIANGLE_STRIP   0x0005

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
  static char cbuf[1024]; static char *av[48], *nav[48];
  int n = 0, argc = 0, i = 0, fd = open("/proc/self/cmdline", 0);
  int skip = 1, k;
  if (fd >= 0){ long r = read(fd, cbuf, (unsigned long)sizeof(cbuf)-1); if (r > 0) n = (int)r; close(fd); }
  cbuf[n] = 0;
  while (i < n && argc < 47){ av[argc++] = &cbuf[i]; while (i < n && cbuf[i]) i++; i++; }
  av[argc] = 0;
  /* When launched as `ld-2.23.so [--opts] prog args...` (the DESK bench does this),
   * /proc/self/cmdline contains the LOADER's argv too.  Normalise so main() sees
   * argv = {prog, args...} exactly as it does when run directly on the unit. */
  { const char *s = av[0]; for (const char *p = av[0]; *p; p++) if (*p == '/') s = p + 1;
    if (s[0] == 'l' && s[1] == 'd' && (s[2] == '-' || s[2] == '.')){
      int j = 1;
      while (j < argc){
        const char *t = av[j];
        if (streq(t, "--library-path") || streq(t, "--preload") || streq(t, "--audit") ||
            streq(t, "--argv0")        || streq(t, "--inhibit-cache")){ j += 2; continue; }
        if (t[0] == '-'){ j += 1; continue; }
        break;
      }
      skip = j;                       /* av[skip] = our program path */
    } }
  for (k = 0; skip + k < argc && k < 47; k++) nav[k] = av[skip + k];
  nav[k] = 0;
  _exit(main(k, nav));
}

/* ---- tiny ES2 textured-quad renderer ------------------------------------- */
static const char *VS_SRC =
  "attribute vec2 aPos;\n"
  "attribute vec2 aTex;\n"
  "varying vec2 vTex;\n"
  "void main(){ vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *FS_SRC =
  "precision mediump float;\n"
  "varying vec2 vTex;\n"
  "uniform sampler2D uTex;\n"
  "void main(){ gl_FragColor = texture2D(uTex, vTex); }\n";
/* full-screen quad: x,y,u,v (NDC) */
static const float QUAD[16] = { -1.f,-1.f, 0.f,0.f,   1.f,-1.f, 1.f,0.f,
                                -1.f, 1.f, 0.f,1.f,   1.f, 1.f, 1.f,1.f };

static unsigned g_tex, g_prog;
static int g_lpos, g_ltex, g_lsamp;

static unsigned mk_shader(u32 type, const char *src){
  unsigned sh = glCreateShader(type); int st = 0; const char *s = src;
  glShaderSource(sh, 1, &s, NULL); glCompileShader(sh);
  glGetShaderiv(sh, GL_COMPILE_STATUS, &st);
  if (!st){ num("shader compile FAILED type", (long)type); return 0; }
  return sh;
}
static int tex_init(int w, int h){
  unsigned vs = mk_shader(GL_VERTEX_SHADER, VS_SRC);
  unsigned fs = mk_shader(GL_FRAGMENT_SHADER, FS_SRC);
  int st = 0;
  if (!vs || !fs) return -1;
  g_prog = glCreateProgram(); glAttachShader(g_prog, vs); glAttachShader(g_prog, fs);
  glLinkProgram(g_prog); glGetProgramiv(g_prog, GL_LINK_STATUS, &st);
  if (!st){ ok("program link FAILED"); return -1; }
  g_lpos = glGetAttribLocation(g_prog, "aPos");
  g_ltex = glGetAttribLocation(g_prog, "aTex");
  g_lsamp = glGetUniformLocation(g_prog, "uTex");
  glGenTextures(1, &g_tex); glBindTexture(GL_TEXTURE_2D, g_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  num("texture ready, prog", (long)g_prog);
  return 0;
}
static void tex_update(int w, int h, const void *px){
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
}
static void tex_draw(int w, int h){
  glUseProgram(g_prog);
  glUniform1i(g_lsamp, 0);
  glEnableVertexAttribArray((unsigned)g_lpos);
  glEnableVertexAttribArray((unsigned)g_ltex);
  glVertexAttribPointer((unsigned)g_lpos, 2, GL_FLOAT, 0, 16, &QUAD[0]);
  glVertexAttribPointer((unsigned)g_ltex, 2, GL_FLOAT, 0, 16, &QUAD[2]);
  glViewport(0, 0, w, h);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* ---- pixel source: 0 flat colour | 1 rgbafile (static) | 2 rgba-sock (stream) --- */
static int   g_src = 0;
static void *g_rgba = 0;

static int load_rgbafile(const char *path, int w, int h){
  unsigned long need = (unsigned long)w * (unsigned long)h * 4UL;
  int fd = open(path, 0);
  if (fd < 0){ ok("FAIL open --rgbafile"); return -1; }
  g_rgba = malloc(need);
  if (!g_rgba || !read_full(fd, g_rgba, need)){ close(fd); ok("FAIL read --rgbafile (need w*h*4 bytes)"); return -1; }
  close(fd); g_src = 1; num("rgba file loaded bytes", (long)need); return 0;
}

/* AF_UNIX server.  A decoder (the unit's vendor decoder, or the desk decoder
 * h264ipcm.py) connects and writes w*h*4 RGBA frames back to back.  The socket is
 * owned here so this process is the durable end; a decoder restart just reconnects.
 * The client fd is NON-BLOCKING: a slow/stalled decoder must never stop the
 * presenter from committing its last frame (that would re-create the very stall
 * this lane removes). */
#define F_SETFL_ 4
#define O_NONBLOCK_ 04000
static int  g_lfd = -1;
static int  g_cfd = -1;
static unsigned long g_acc = 0;      /* bytes accumulated toward the next frame */
static int  rgba_sock_open(const char *path, int w, int h){
  struct { unsigned short fam; char path[108]; } sa; int i;
  for (i = 0; i < 108; i++) sa.path[i] = 0;
  for (i = 0; i < 107 && path[i]; i++) sa.path[i] = path[i];
  sa.fam = 1 /*AF_UNIX*/;
  unlink(path);
  g_lfd = socket(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0);
  if (g_lfd < 0 || bind(g_lfd, &sa, sizeof(sa)) != 0 || listen(g_lfd, 4) != 0){ ok("FAIL rgba-sock bind"); return -1; }
  fcntl(g_lfd, F_SETFL_, O_NONBLOCK_);     /* never block on accept: keep committing frames */
  chmod(path, 0777);
  g_rgba = malloc((unsigned long)w * (unsigned long)h * 4UL);
  g_src = 2;
  ok("rgba-sock listening -> (a decoder writes w*h*4 RGBA per frame)");
  put("[egl] rgba-sock path="); put(path); put("\n");
  return 0;
}
/* returns 1 when a complete new frame has landed in g_rgba */
static int rgba_sock_pump(int w, int h){
  unsigned long need = (unsigned long)w * (unsigned long)h * 4UL;
  if (g_cfd < 0){
    int c = accept(g_lfd, 0, 0);
    if (c < 0) return 0;
    fcntl(c, F_SETFL_, O_NONBLOCK_);
    g_cfd = c; g_acc = 0;
    num("rgba-sock client fd", c);
    return 0;
  }
  for (;;){
    long r = read(g_cfd, (char*)g_rgba + g_acc, need - g_acc);
    if (r > 0){ g_acc += (unsigned long)r; if (g_acc >= need){ g_acc = 0; return 1; } continue; }
    if (r == 0){ close(g_cfd); g_cfd = -1; g_acc = 0; ok("rgba-sock client gone -> re-accept"); return 0; }
    return 0;   /* r < 0: no data right now (non-blocking) */
  }
}

int main(int argc, char **argv){
  u32 ivi_id = 3000u;
  int w = 1280, h = 480, secs = 0;                 /* secs 0 = PERSISTENT (default) */
  long col = 0x00ff00L;
  int fps = 10, hold = 0, maxframes = 0, persistent = 1, i, nframes, have_frame = 0;
  int npos = 0;
  const char *rgbafile = NULL, *rgbasock = NULL;
  float cr, cg, cb;
  void *dpy, *cfgs[16], *cfg = 0, *win, *esurf, *ctx; i32 ncfg = 0, v;
  i32 cfg_attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                      EGL_CONFIG_CAVEAT, EGL_NONE, EGL_NONE };
  i32 ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

  /* order-independent: positional id,w,h,secs,colour (in that order) + any flags */
  for (i = 1; i < argc; i++){
    const char *a = argv[i];
    if (streq(a, "--persist")) persistent = 1;
    else if (streq(a, "--hold")) hold = 1;
    else if (streq(a, "--fps") && i + 1 < argc) fps = my_atoi(argv[++i]);
    else if (streq(a, "--frames") && i + 1 < argc) maxframes = my_atoi(argv[++i]);
    else if (streq(a, "--rgbafile") && i + 1 < argc) rgbafile = argv[++i];
    else if (streq(a, "--rgba-sock") && i + 1 < argc) rgbasock = argv[++i];
    else if (a[0] == '-' ) { /* unknown flag: ignore */ }
    else {
      switch (npos++){
        case 0: ivi_id = (u32)my_atoi(a); break;
        case 1: w = my_atoi(a); break;
        case 2: h = my_atoi(a); break;
        case 3: secs = my_atoi(a); break;
        case 4: col = my_strtol16(a); break;
        default: break;
      }
    }
  }
  cr = (float)((col >> 16) & 0xff) / 255.0f; cg = (float)((col >> 8) & 0xff) / 255.0f; cb = (float)(col & 0xff) / 255.0f;
  if (fps < 1) fps = 1;
  if (secs > 0) persistent = 0;          /* a positive secs bounds the run (tests) */
  if (maxframes > 0) persistent = 0;

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
  if (eglChooseConfig(dpy, cfg_attrs, cfgs, 16, &ncfg) == 0 || ncfg < 1){ ok("FAIL eglChooseConfig"); return 6; }
  cfg = cfgs[0];
  eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &v); numx("visual (wl_drm fourcc)", v);
  win = wl_egl_window_create(g_surface, w, h);
  if (!win){ ok("FAIL wl_egl_window_create"); return 6; }
  esurf = eglCreateWindowSurface(dpy, cfg, win, NULL);
  if (!esurf){ ok("FAIL eglCreateWindowSurface"); return 6; }
  ctx = eglCreateContext(dpy, cfg, NULL, ctx_attrs);
  if (!ctx){ ok("FAIL eglCreateContext"); return 6; }
  if (eglMakeCurrent(dpy, esurf, esurf, ctx) == 0){ ok("FAIL eglMakeCurrent"); return 6; }
  eglSwapInterval(dpy, 0);
  ok("EGL surface + context current (dmabuf-backed); ONE window reused for the whole run");

  if (rgbasock){
    if (rgba_sock_open(rgbasock, w, h) == 0) tex_init(w, h);
  } else if (rgbafile){
    if (load_rgbafile(rgbafile, w, h) == 0){ tex_init(w, h); have_frame = 1; }
  }

  nframes = hold ? 1 : (maxframes > 0 ? maxframes : (persistent ? 0 : secs * fps));
  num("frames to commit", nframes ? nframes : -1);   /* -1 = unbounded/persistent */
  num("cadence (fps)", fps);
  for (i = 0; nframes == 0 || i < nframes; i++){
    int got = 0;
    if (g_src == 2) got = rgba_sock_pump(w, h);
    if (g_src == 1) got = 1;
    if (got && g_rgba){ tex_update(w, h, g_rgba); have_frame = 1; }
    if (g_src && have_frame) tex_draw(w, h);
    else { glClearColor(cr, cg, cb, 1.0f); glClear(GL_COLOR_BUFFER_BIT); }
    eglSwapBuffers(dpy, esurf);
    wl_display_flush(g_display);
    if ((i % 50) == 0) num("frame", i);
    if (nframes == 0 || i + 1 < nframes) usleep((unsigned int)(1000000 / fps));
  }
  ok(persistent ? "done" : "done (bounded run)");
  return 0;
}
