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
 *                                        session SETUP response.
 *
 * Logs to /tmp/mib3shim.log (world-writable tmpfs; SMACK `_`).
 * Env knobs:
 *   MIB3_SHIM_ADD=0/1     advertise the cluster display (default 1)
 *   MIB3_SHIM_W/H/FPS     cluster geometry (default 1280x720@60)
 *   MIB3_SHIM_DBG=0/1     verbose (default 1)
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

/* ---- CF/libAirPlay symbols, lazily resolved ------------------------------ */
typedef CFStringRef (*fn_CFStringCreateWithCString)(CFAllocatorRef,const char*,CFStringEncoding);
typedef Boolean     (*fn_CFStringGetCString)(CFStringRef,char*,CFIndex,CFStringEncoding);
typedef CFIndex     (*fn_CFArrayGetCount)(CFArrayRef);
typedef CFTypeRef   (*fn_CFArrayGetValueAtIndex)(CFArrayRef,CFIndex);
typedef CFMutableArrayRef (*fn_CFArrayCreateMutable)(CFAllocatorRef,CFIndex,const void*);
typedef void        (*fn_CFArrayAppendValue)(CFMutableArrayRef,const void*);
typedef void        (*fn_CFDictionarySetValue)(CFMutableDictionaryRef,const void*,const void*);
typedef CFTypeRef   (*fn_CFDictionaryGetValue)(CFDictionaryRef,const void*);
typedef void        (*fn_CFShow)(CFTypeRef);
typedef void        (*fn_CFRelease)(CFTypeRef);
typedef void        (*fn_CFDictionarySetInt64)(CFMutableDictionaryRef,const void*,s64);
typedef CFMutableDictionaryRef (*fn_CFDictionaryCreateMutable)(CFAllocatorRef,CFIndex,const void*,const void*);
typedef s64         (*fn_CFDictionaryGetInt64)(CFDictionaryRef,const void*,s64);
typedef unsigned long (*fn_CFGetTypeID)(CFTypeRef);
typedef unsigned long (*fn_CFTypeGetTypeID)(void);

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
typedef void (*fn_SessionSetup)(void*, u64, CFMutableDictionaryRef*);

static fn_CFStringCreateWithCString p_CFStringCreateWithCString;
static fn_CFStringGetCString        p_CFStringGetCString;
static fn_CFArrayGetCount           p_CFArrayGetCount;
static fn_CFArrayGetValueAtIndex    p_CFArrayGetValueAtIndex;
static fn_CFArrayAppendValue        p_CFArrayAppendValue;
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
static void *p_kCFTypeArrayCallBacks;
static void *p_kCFLDictKeyCB;
static void *p_kCFLDictValCB;
static void *p_kCFLBooleanTrue;

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
static void               *p_opt_video_device_id;   /* &CINEMO_OPTION_VIDEO_DEVICE_ID */
static void               *p_iid_cinemo_option;     /* &ICinemoOption::iid */

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

#define K128 (0x08000100)   /* kCFStringEncodingUTF8 */

static void resolve(void){
  if (resolved) return;
  resolved = 1;
  p_CFStringCreateWithCString = (fn_CFStringCreateWithCString)R("CFStringCreateWithCString");
  p_CFStringGetCString        = (fn_CFStringGetCString)       R("CFStringGetCString");
  p_CFArrayGetCount           = (fn_CFArrayGetCount)          R("CFArrayGetCount");
  p_CFArrayGetValueAtIndex    = (fn_CFArrayGetValueAtIndex)   R("CFArrayGetValueAtIndex");
  p_CFArrayAppendValue        = (fn_CFArrayAppendValue)       R("CFArrayAppendValue");
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
  real_AddScreenDisplay       = (fn_AddScreenDisplay)          R("AirPlayInfoArrayAddScreenDisplay");
  real_SessionSetup           = (fn_SessionSetup)              R("AirPlayReceiverSessionSetup");

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
  logkv("init","p_realSSPD",(u64)p_realSSPD,1);
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

static void add_cluster_fields(CFMutableArrayRef *inArray, int cw, int chp){
  CFMutableDictionaryRef d = last_dict(inArray);
  if (!d){ logline("cluster","no dict to annotate"); return; }

  /* type = 111 (alt / instrument-cluster stream) */
  if (p_CFDictionarySetInt64){
    int dtype = (int)env_num("MIB3_SHIM_TYPE", 111);
    p_CFDictionarySetInt64(d, sstr("type"), (s64)dtype);
    logkv("cluster","type",(u64)(u32)dtype,0);
  }
  /* initialURL -- the cluster scene Apple Maps should render */
  if (p_CFDictionarySetValue){
    char *url = getenv("MIB3_SHIM_URL");
    if (!url || !url[0]) url = "maps:/car/instrumentcluster/map";
    p_CFDictionarySetValue(d, sstr("initialURL"), sstr(url));
    logline("cluster/initialURL", url);
  }
  /* viewAreas = [ { view rect + safeArea (full panel, drawUIOutsideSafeArea) } ] */
  if (p_CFDictionaryCreateMutable && p_CFDictionarySetInt64 && p_CFArrayAppendValue &&
      p_CFDictionarySetValue && p_kCFTypeArrayCallBacks && p_kCFLDictKeyCB && p_kCFLDictValCB){
    fn_CFArrayCreateMutable mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
    CFMutableArrayRef va = mka ? mka(0,0,p_kCFTypeArrayCallBacks) : 0;
    if (va){
      CFMutableDictionaryRef area = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
      CFMutableDictionaryRef safe = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
      if (area && safe){
        p_CFDictionarySetInt64(area, sstr("widthPixels"),  (s64)cw);
        p_CFDictionarySetInt64(area, sstr("heightPixels"), (s64)chp);
        p_CFDictionarySetInt64(area, sstr("originXPixels"),(s64)0);
        p_CFDictionarySetInt64(area, sstr("originYPixels"),(s64)0);
        p_CFDictionarySetInt64(safe, sstr("widthPixels"),  (s64)cw);
        p_CFDictionarySetInt64(safe, sstr("heightPixels"), (s64)chp);
        p_CFDictionarySetInt64(safe, sstr("originXPixels"),(s64)0);
        p_CFDictionarySetInt64(safe, sstr("originYPixels"),(s64)0);
        if (p_kCFLBooleanTrue && p_CFDictionarySetValue){
          /* kCFLBooleanTrue is a CFBooleanRef *variable* (8-byte object): deref once. */
          void *btrue = *(void **)p_kCFLBooleanTrue;
          if (btrue) p_CFDictionarySetValue(safe, sstr("drawUIOutsideSafeArea"), btrue);
        }
        p_CFDictionarySetValue(area, sstr("safeArea"), safe);
        p_CFArrayAppendValue(va, area);
        p_CFDictionarySetValue(d, sstr("viewAreas"), va);
        p_CFDictionarySetInt64(d, sstr("initialViewArea"), (s64)0);
        logline("cluster","viewAreas added");
      }
      /* release OUR +1 on area/safe (va retains area, area retains safe); the
       * shim owned them and never released them -> leak on every cluster ad (F7). */
      if (p_CFRelease){ if (safe) p_CFRelease(safe); if (area) p_CFRelease(area); }
      if (p_CFRelease) p_CFRelease(va);          /* dict retains it */
    }
  }
  if (p_CFShow) { logline("cluster","CFShow(dict) ->"); p_CFShow(d); }
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
/* FRAME INTERCEPTION state (route B).  The cluster stream's ScreenStreamRef
 * (per-stream identity) is held in a small REGISTRY (not a single slot): the
 * interposer diverts any stream present in it, and every cluster thread removes
 * its own entry when it exits.  A single global would be overwritten on a
 * reconnect / a second type-111 stream, which would (a) forward the old stream's
 * frames to the singleton (main-screen corruption) and (b) let one thread mutate
 * another thread's state. */
static int cluster_on_frame(const void *data, unsigned long len, unsigned long ts);
static unsigned long g_cluster_frames;

#define MIB3_CLUSTER_TYPE  111
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

/* per-setup context: the servicing thread reads ONLY this, never a shared
 * global that a later setup could overwrite (that was the UAF/corruption path). */
typedef struct {
  void *screen; int lsock; void *session; int flags; void *stream;
} cl_ctx;

static void *cluster_thread(void *arg){
  cl_ctx *c = (cl_ctx*)arg;
  int connfd = -1;
  logline("cluster/thread","started");
  if (!c) return 0;
  if (p_SocketAccept && c->lsock >= 0 && p_SocketAccept(c->lsock, 10, &connfd, 0) == 0 && connfd >= 0){
    if (p_close){ p_close(c->lsock); }
    c->lsock = -1;
    void *ns = 0;
    if (p_NetSocketCreate && p_NetSocketCreate(&ns, connfd) == 0){
      connfd = -1;
      if (p_NetSocketNative && p_SocketSetQoS) p_SocketSetQoS(p_NetSocketNative(ns), SCREEN_SOCK_QOS);
      if (p_ScreenSetTS && g_libap_base){
        void *ctx[3];
        ctx[0] = c->session;
        ctx[1] = (void*)(g_libap_base + TIMESYNC_CB1_RVA);
        ctx[2] = (void*)(g_libap_base + TIMESYNC_CB2_RVA);
        p_ScreenSetTS(c->screen, ctx);
      }
      if (p_ScreenStart && c->session &&
          p_ScreenStart(c->screen, *(void**)((char*)c->session + 0x28)) == 0){
        /* per-stream identity: StartSession created the ScreenStream at
         * AirPlayReceiverSessionScreen+0x328 (corpus: StopSession does
         * ScreenStreamStop(*(this+0x328)); ProcessFrames calls
         * ScreenStreamProcessData(param_1[0x65]) -- 0x65*8 = 0x328). */
        void *st = *(void**)((char*)c->screen + 0x328);
        c->stream = st;
        if (st && cl_stream_add(st) != 0)
          logline("cluster/stream","WARN registry full -> this stream will NOT be diverted");
        logkv("cluster/stream","ScreenStream",(u64)st,1);
        if (p_ScreenProcFrames) p_ScreenProcFrames(c->screen, ns, c->flags);
        if (st) cl_stream_remove(st);       /* stream ended: stop diverting it */
      }
    }
    if (connfd >= 0 && p_close) p_close(connfd);
  }
  if (p_ScreenStop) p_ScreenStop(c->screen);
  if (p_ScreenDelete){ p_ScreenDelete(c->screen); logline("cluster/thread","screen deleted"); } /* F7: Stop does not free */
  logline("cluster/thread","exit");
  if (p_free) p_free(c);
  return 0;
}

/* Set up a second screen for a type-111 stream dict and append
 * {type:111, dataPort} to response["streams"].  Returns 0 on success. */
static int cluster_setup(void *session, void *streamDict, void *response){
  if (!session || !streamDict || !response){ logline("cluster","bad args"); return -1; }
  if (!p_ScreenCreate || !p_ScreenSetup){ logline("cluster","screen API missing"); return -1; }

  void *screen = 0;
  if (p_ScreenCreate(&screen) != 0 || !screen){ logline("cluster","Create failed"); return -1; }
  p_ScreenSetup(screen, streamDict);

  long connID = p_CFDictionaryGetInt64 ? (long)p_CFDictionaryGetInt64(streamDict, sstr("streamConnectionID"), 0) : 0;
  if (!connID){ logline("cluster","no streamConnectionID"); goto fail_screen; }

  void *hk = *(void**)((char*)session + 0x478);   /* pairVerifySession (HomeKit) */
  if (!hk){
    unsigned char key[16], iv[16];
    if (!p_DeriveAES){ logline("cluster","no AES derive"); goto fail_screen; }
    p_DeriveAES((char*)session + 0x268, 0x10, connID, key, iv);
    /* the unit checks SetSecurityInfo's return (corpus _ScreenSetup, AES branch)
     * and aborts on failure -> mirror it (fail closed; DeriveAES's return the
     * unit ignores, so we do too). */
    if (p_ScreenSetSec && p_ScreenSetSec(screen, key, iv) != 0){
      logline("cluster","SetSecurityInfo failed"); goto fail_screen;
    }
    logline("cluster","security: AES");
  } else {
    unsigned char key[32];
    fn_chachaDerive ch;
    if (!g_libap_base){ logline("cluster","no lib base for chacha"); goto fail_screen; }
    ch = (fn_chachaDerive)(g_libap_base + CHACHA_DERIVE_RVA);
    /* The unit calls FUN_001163d8((long)session + 0x478, connID, 0,0, &key):
     * the ADDRESS of the pairVerifySession field, which FUN dereferences
     * (*param_1 == 0 / PairingSessionDeriveKey(*param_1,...)).  Passing the
     * field VALUE (hk) would make FUN deref the session object itself.
     * FUN returns an int STATUS (0 = success), not void: disasm at 0x163d8 ends
     * `mov w0,w19; ret`, and the unit's own callers test it (libAirPlay 0x1ab60:
     * `bl 0x163d8; str w0; cbz w0`; corpus _ScreenSetup case 0x6e).  Mirror the
     * unit and fail CLOSED -- an uninitialised key buffer must not be installed. */
    if (ch((char*)session + 0x478, connID, 0, 0, key) != 0){
      logline("cluster","chacha derive failed"); goto fail_screen;
    }
    if (p_ScreenSetChaCha) p_ScreenSetChaCha(screen, key, 0x20);
    logline("cluster","security: chacha (RVA)");
  }

  int port = 0, lsock = -1;
  unsigned family = *(unsigned short*)((char*)session + 0xec);
  if (!p_ServerSocketOpen ||
      p_ServerSocketOpen(family, 1 /*SOCK_STREAM*/, 6 /*TCP*/, 0xffffe872, &port, 0xfffd4000, &lsock) != 0){
    logline("cluster","ServerSocketOpen failed"); goto fail_screen;
  }
  if (p_SocketSetQoS) p_SocketSetQoS(lsock, SCREEN_SOCK_QOS);
  logkv("cluster","dataPort",(u64)(u32)port,0);

  /* append {type:111, dataPort:port} to response["streams"].  Mirrors the unit's
   * _AddResponseStream (which CFReleases the array after inserting it). */
  if (p_CFDictionaryCreateMutable && p_CFDictionarySetInt64 && p_CFArrayAppendValue &&
      p_CFDictionaryGetValue && p_kCFLDictKeyCB && p_kCFLDictValCB && sstr("type")){
    CFMutableDictionaryRef desc = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
    if (desc){
      p_CFDictionarySetInt64(desc, sstr("type"), MIB3_CLUSTER_TYPE);
      p_CFDictionarySetInt64(desc, sstr("dataPort"), (s64)port);
      CFMutableArrayRef streams = (CFMutableArrayRef)p_CFDictionaryGetValue((CFDictionaryRef)response, sstr("streams"));
      int created = 0;
      if (!streams){
        fn_CFArrayCreateMutable mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
        if (mka && p_kCFTypeArrayCallBacks){
          streams = mka(0,0,p_kCFTypeArrayCallBacks);
          if (streams && p_CFDictionarySetValue){ p_CFDictionarySetValue((CFMutableDictionaryRef)response, sstr("streams"), streams); created = 1; }
        }
      }
      if (streams) p_CFArrayAppendValue(streams, desc);
      if (p_CFRelease) p_CFRelease(desc);         /* array retains it */
      if (created && p_CFRelease) p_CFRelease(streams); /* dict retains it */
      logline("cluster","response entry {type:111,dataPort} appended");
    }
  }

  /* ROUTE-B: stand up the cluster's frame sink (env-gated).  Additive: the stock
   * stream handling above is unchanged; if neither sink is available we log and
   * continue (the stream is still serviced).  Preference order:
   *   1. the shim's own Cinemo pipeline (decode bridge) -- Cinemo owns the ivi
   *      surface and decodes+renders the cluster frames into it;
   *   2. the shim's own Wayland/EGL surface (fallback, no decode). */
  if (env_on("MIB3_SHIM_SURFACE",1)){
    int cw  = (int)env_num("MIB3_SHIM_W",1280);
    int chp = (int)env_num("MIB3_SHIM_H",480);
    unsigned iviid = (unsigned)env_num("MIB3_SHIM_IVIID",0x4d4233); /* 'MB3' */
    if (env_on("MIB3_SHIM_BRIDGE",1) && bridge_create(cw, chp, iviid) == 0){
      logline("cluster","route-B Cinemo bridge ready (Cinemo decodes+renders)");
      logline("cluster/surfname", surf_name());
    } else if (surface_create(cw, chp, iviid) == 0){
      logline("cluster","route-B surface ready (fallback, no decode)");
      logline("cluster/surfname", surf_name());
    } else {
      logline("cluster","route-B sink unavailable (continuing, stream still serviced)");
    }
  }

  /* spawn the servicing thread with its OWN heap context (no shared mutable
   * state a later setup could clobber -> no cross-thread UAF/corruption). */
  if (!p_malloc || !p_pthreadCreate){
    logline("cluster","no malloc/pthread -> cannot service stream");
    goto fail_sock;
  }
  cl_ctx *c = (cl_ctx*)p_malloc(sizeof(cl_ctx));
  if (!c){ logline("cluster","ctx alloc failed"); goto fail_sock; }
  c->screen = screen; c->lsock = lsock; c->session = session; c->stream = 0;
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
  if (p_close && lsock >= 0) p_close(lsock);
fail_screen:
  if (p_ScreenStop) p_ScreenStop(screen);
  if (p_ScreenDelete){ p_ScreenDelete(screen); logline("cluster","screen deleted (fail path)"); } /* F7 */
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

/* ==========================================================================
 * The display seam.
 * ========================================================================*/
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

  /* 2) advertise the cluster once, after the main display.
   *    RUNTIME GATE: only when the stick marker is present (gate_active()).
   *    If AddScreenDisplay fires before the stick is mounted the marker is
   *    absent and nothing is advertised -- fail-safe to stock (see the lane doc
   *    for the live-confirmable "is AddScreenDisplay boot-time or session-time"
   *    question). */
  if (rc == 0 && !saw_main_display && env_on("MIB3_SHIM_ADD",1) && gate_active()) {
    saw_main_display = 1;
    CFStringRef cu = p_CFStringCreateWithCString ? p_CFStringCreateWithCString(0,"MIB3-KOMBI-0001",K128) : 0;
    int cw   = (int)env_num("MIB3_SHIM_W", 1280);
    int chp  = (int)env_num("MIB3_SHIM_H", 480);   /* grounded cluster default: 1280x480 (LANE_SURFACE_BINDING_DESK §3a) */
    int cfps = (int)env_num("MIB3_SHIM_FPS", 60);
    logline("AddScreenDisplay","ADDING cluster display");
    log_uuid("AddScreenDisplay/cluster-uuid", cu);
    logkv("AddScreenDisplay","cluster widthPixels",(u64)(u32)cw,0);
    logkv("AddScreenDisplay","cluster heightPixels",(u64)(u32)chp,0);
    logkv("AddScreenDisplay","cluster maxFPS",(u64)(u32)cfps,0);
    /* cluster features/inputDevice: PlayPort's cluster config uses 0/0;
     * LIVI's working stack uses features=0x0A, primaryInputDevice=3. Env-tunable. */
    int cfeat  = (int)env_num("MIB3_SHIM_CFEAT", 0);
    int cindev = (int)env_num("MIB3_SHIM_CINDEV", 0);
    int rc2 = real_AddScreenDisplay(inArray, cu, cfeat, cindev, cfps, cw, chp, cw, chp);
    logkv("AddScreenDisplay","cluster rc",(u64)(u32)rc2,0);
    if (rc2 == 0) add_cluster_fields(inArray, cw, chp);
    if (p_CFShow && inArray && *inArray) { logline("AddScreenDisplay","CFShow(displays) ->"); p_CFShow(*inArray); }
  }
  return rc;
}

/* ==========================================================================
 * The session-setup seam.
 * ========================================================================*/
void AirPlayReceiverSessionSetup(void *session, u64 request, CFMutableDictionaryRef *responseOut)
{
  hit("SETUP\n", 6);                      /* prove entry, independent of everything */
  logline("SessionSetup","ENTERED");       /* before resolve(): resolve() may fail */
  resolve();
  logline("SessionSetup","CALL");
  logkv("SessionSetup","session",(u64)session,1);
  logkv("SessionSetup","request",(u64)request,1);
  logkv("SessionSetup","responseOut",(u64)responseOut,1);

  if (real_SessionSetup && !env_on("MIB3_SHIM_SKIP_REAL",0))
    real_SessionSetup(session, request, responseOut);

  if (responseOut && *responseOut){
    logline("SessionSetup","response dict:");
    if (p_CFShow) p_CFShow(*responseOut);
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
    CFTypeRef rv = p_CFDictionaryGetValue((CFDictionaryRef)request, sstr("streams"));
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
        s64 t = p_CFDictionaryGetInt64 ? p_CFDictionaryGetInt64((CFDictionaryRef)sd, sstr("type"), 0) : 0;
        if ((int)t == MIB3_CLUSTER_TYPE){
          logline("SessionSetup","type-111 stream -> cluster_setup");
          cluster_setup(session, sd, *responseOut);
        }
      }
    }
  }

  /* Inject enabledFeatures = [altScreen, viewAreas, uiContext] if missing.
   * Gated: inert (no marker) => don't touch the response at all. */
  if (gate_active() && responseOut && *responseOut && p_CFDictionarySetValue &&
      p_CFStringCreateWithCString && p_kCFTypeArrayCallBacks) {
    CFStringRef k = p_CFStringCreateWithCString(0,"enabledFeatures",K128);
    if (k && (!p_CFDictionaryGetValue || !p_CFDictionaryGetValue(*responseOut,k))) {
      /* build a mutable array of the 3 feature strings */
      fn_CFArrayCreateMutable mka = (fn_CFArrayCreateMutable)R("CFArrayCreateMutable");
      CFMutableArrayRef arr = mka ? mka(0,0,p_kCFTypeArrayCallBacks) : 0;
      const char *feats[3] = {"altScreen","viewAreas","uiContext"};
      int i;
      if (arr){
        for (i=0;i<3;i++){
          CFStringRef f = p_CFStringCreateWithCString(0,feats[i],K128);
          if (f) p_CFArrayAppendValue(arr, f);
        }
        p_CFDictionarySetValue(*responseOut, k, arr);
        logline("SessionSetup","injected enabledFeatures");
        if (p_CFShow) p_CFShow(*responseOut);
      } else {
        logline("SessionSetup","enabledFeatures array alloc failed");
      }
    } else {
      logline("SessionSetup","enabledFeatures already present");
    }
  }
}

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
  /* PRIMARY: the shim's own Cinemo projection -> Cinemo decodes + renders the
   * compressed access unit into the shim's surface (the decode bridge). */
  if (bridge_active()) return bridge_feed(data, len, ts);
  /* FALLBACK: the shim's own RGBA surface path (no decode).  The payload here is
   * COMPRESSED (H.264), so an RGBA upload is only meaningful when the caller
   * supplies at least w*h*4 bytes -- otherwise glTexSubImage2D would read past
   * the (much smaller) compressed buffer.  Guard it. */
  if (env_on("MIB3_SHIM_SURFACE",1) && g_gfx.ready && g_gfx.w > 0 && g_gfx.h > 0){
    unsigned long need = (unsigned long)g_gfx.w * (unsigned long)g_gfx.h * 4UL;
    if (len >= need) return surface_write_frame(data, g_gfx.w, g_gfx.h);
    if (n == 1) logline("frame/cluster","compressed payload < w*h*4: RGBA fallback skipped");
  }
  return 0;
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
} bridge_t;
static bridge_t g_br;

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
  int n=0; fmt_u(out,(unsigned)a); while(out[n]) n++; out[n++]=','; fmt_u(out+n,(unsigned)b);
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
  logline("bridge","resolved");
  logkv("bridge","CinemoCreateConfig",(u64)p_CinemoCreateConfig,1);
  logkv("bridge","CinemoCreatePlaylist",(u64)p_CinemoCreatePlaylist,1);
  logkv("bridge","CinemoCreateWindow",(u64)p_CinemoCreateWindow,1);
  logkv("bridge","CinemoCreateProjection",(u64)p_CinemoCreateProjection,1);
  logkv("bridge","CINEMO_OPTION_VIDEO_DEVICE_ID",(u64)p_opt_video_device_id,1);
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
  if (p_CinemoCreatePlaylist(&g_br.playlist) != 0 || !g_br.playlist){ logline("bridge","FAIL: CinemoCreatePlaylist"); goto fail; }
  { fn_vc1 f = (fn_vc1)vslot(g_br.playlist, 0x128);
    if (!f || f(g_br.playlist, &g_br.playback) != 0 || !g_br.playback){ logline("bridge","FAIL: playlist->CreatePlayback"); goto fail; } }
  { fn_vc2 f = (fn_vc2)vslot(g_br.playback, 0x28);
    if (f) f(g_br.playback, 0, 0); }                 /* set event handler (none needed) */
  { fn_vc2 f = (fn_vc2)vslot(g_br.cfg, 0x30);
    if (!f || f(g_br.cfg, p_opt_video_device_id, &g_br.dev) != 0 || !g_br.dev){ logline("bridge","FAIL: config->GetVideoDevice"); goto fail; } }
  { fn_vc2 f = (fn_vc2)vslot(g_br.dev, 0xa0);
    char pos[24], size[24], idbuf[16];
    fmt_2int(pos,0,0); fmt_2int(size,w,h); fmt_u(idbuf,ivi_id);
    if (!f){ logline("bridge","FAIL: dev->SetProperty missing"); goto fail; }
    if (f(g_br.dev,"windowpos",pos)  != 0){ logline("bridge","FAIL: windowpos");  goto fail; }
    if (f(g_br.dev,"windowsize",size)!= 0){ logline("bridge","FAIL: windowsize"); goto fail; }
    if (f(g_br.dev,"ivi-app-id",idbuf)!=0){ logline("bridge","FAIL: ivi-app-id"); goto fail; }
    logkv("bridge","ivi_id",(u64)ivi_id,0); }
  { fn_vc0 f = (fn_vc0)vslot(g_br.dev, 0x58);
    const char *dn = f ? (const char*)f(g_br.dev) : 0;
    if (!dn){ logline("bridge","FAIL: dev->GetName"); goto fail; }
    logline("bridge/devname", dn);
    if (p_CinemoCreateWindow(dn, &g_br.window) != 0 || !g_br.window){ logline("bridge","FAIL: CinemoCreateWindow"); goto fail; } }
  if (p_CinemoCreateProjection("?disable_scaling=1", &g_br.projection) != 0 || !g_br.projection){
    logline("bridge","FAIL: CinemoCreateProjection"); goto fail; }
  { fn_vc2 f = (fn_vc2)vslot(g_br.playlist, 0xd8);
    if (!f || f(g_br.playlist, g_br.projection, 0) != 0){ logline("bridge","FAIL: playlist->SetProjection"); goto fail; } }

  g_br.active = 1;
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
      g_br.fail_count++; return -1;
    }
    g_br.feed_count++;
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
  return R( mangled);
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
