/* ============================================================================
 * driver_viewarea.c -- prove the SHIM's view-area path on the desk (2026-10-19).
 *
 * With libstubcmd.so standing in for the stock AirPlayReceiverSessionSendCommand
 * (preloaded AFTER the shim, so the shim's dlsym(RTLD_NEXT) lands on the stub),
 * the assertions are on the REAL dictionary the shim would put on the wire --
 * the same instrument that caught the missing `type` key.
 *
 * Modes (VA_TEST_MODE):
 *   set0        VIEWAREAS=1, VA_COUNT=2 -> ship updateViewArea index 0
 *   set1        ...                     -> ship index 1
 *   bounds      ...                     -> ship index 9 (must be refused locally)
 *   off         VIEWAREAS=0             -> must ship NOTHING
 *   nopcmd      VIEWAREAS=1, VA_CMD=0   -> must ship NOTHING
 *   index2      VA_COUNT=3              -> ship index 2 (a third area)
 *   ctl         ctl_line("v2") + pump   -> the inbox grammar ships index 2
 *   next        ctl_line("next")        -> current 0 -> ships index 1
 *   appear      APPEARANCE path         -> ship mapAppearanceUpdate{...}
 *   advert_off  drive the /info seam with VIEWAREAS unset (the shipped default)
 *   advert_on   drive the /info seam with VIEWAREAS=1, VA_COUNT=3
 *
 * Usage (the bench does this):  LD_PRELOAD="mib3shim.so libstubcmd.so" \
 *     ld-2.23.so --library-path ... driver_viewarea
 * ==========================================================================*/
typedef unsigned long u64;

extern void  *dlsym(void *, const char *);
extern void  *calloc(unsigned long, unsigned long);
extern int    write(int, const void *, unsigned long);
extern void   _exit(int);
extern int    setenv(const char *, const char *, int);
extern char  *getenv(const char *);
#define RTLD_DEFAULT ((void *)0)

/* the seam, as an ordinary extern -> driver PLT -> preloaded shim */
extern int AirPlayInfoArrayAddScreenDisplay(void *inArray, void *uuid,
                                            int a, int b, int c,
                                            int w, int h, int pw, int ph);

typedef unsigned long      CFIndex;
typedef void              *CFMutableArrayRef;
typedef const void        *CFStringRef;
typedef int  (*fn_listen)(void *, const char *, const char *);
typedef int  (*fn_send)(void *, int);
typedef int  (*fn_pump)(void *, int);
typedef int  (*fn_ctl)(const char *);
typedef int  (*fn_count)(void);
typedef const char *(*fn_lasttype)(void);
typedef int  (*fn_ackcalls)(void);
typedef int  (*fn_ackstatus)(void);
typedef CFStringRef (*fn_cfstr)(void *, const char *, unsigned);
typedef CFMutableArrayRef (*fn_cfarr)(void *, CFIndex, const void *);
typedef CFIndex (*fn_cfcount)(const void *);
typedef const void *(*fn_at)(const void *, CFIndex);
typedef const void *(*fn_dget)(const void *, const void *);

#define KUTF8 0x08000100u

static u64 slen_(const char *s){ u64 n = 0; while (s && s[n]) n++; return n; }
static void P(const char *s){ write(1, s, slen_(s)); }
static void Pn(const char *tag, long v){
  char b[64]; int k = 0; char t[16]; int j = 0; unsigned long u;
  b[k++] = '['; while (*tag) b[k++] = *tag++; b[k++] = '=';
  u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
  if (v < 0) b[k++] = '-';
  if (!u) t[j++] = '0'; while (u){ t[j++] = (char)('0' + (u % 10)); u /= 10; }
  while (j) b[k++] = t[--j];
  b[k++] = ']'; b[k++] = '\n';
  write(1, b, (unsigned long)k);
}
static const char *envs(const char *n){ return getenv(n); }
static int streq(const char *a, const char *b){ while (*a && *a == *b){ a++; b++; } return *a == *b; }

static void *g_session;
static fn_send   g_send;
static fn_pump   g_pump;
static fn_ctl    g_ctl;
static fn_count  g_count;
static fn_lasttype g_lasttype;
static fn_ackcalls g_ackcalls;
static fn_ackstatus g_ackstatus;

/* the /info advert seam (modes advert_off / advert_on) */
static void advert(void)
{
  fn_cfstr   cfstr  = (fn_cfstr)  dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
  fn_at      at     = (fn_at)     dlsym(RTLD_DEFAULT, "CFArrayGetValueAtIndex");
  fn_cfcount cfcount= (fn_cfcount)dlsym(RTLD_DEFAULT, "CFArrayGetCount");
  fn_dget    dget   = (fn_dget)   dlsym(RTLD_DEFAULT, "CFDictionaryGetValue");
  CFMutableArrayRef arr = 0;
  CFStringRef uuid;
  int rc;
  if (!cfstr){ P("[drvva] no CFStringCreateWithCString\n"); return; }
  uuid = cfstr(0, "C1057E12-A0D1-4E20-B0C0-1057E12A0D10", KUTF8);
  rc = AirPlayInfoArrayAddScreenDisplay(&arr, (void *)uuid, 2, 0, 60, 1280, 480, 292, 110);
  Pn("seam rc", rc);
  Pn("displays array", (long)arr);
  if (cfcount) Pn("displays count", (long)cfcount(arr));
  if (at && dget && cfcount && arr){
    long n = (long)cfcount(arr);
    const void *d = at(arr, n - 1);
    const void *va = dget(d, cfstr(0, "viewAreas", KUTF8));
    Pn("cluster viewAreas present", va ? 1 : 0);
    if (va && cfcount) Pn("viewAreas count", (long)cfcount(va));
  }
}

void _start(void)
{
  const char *mode = envs("VA_TEST_MODE");
  fn_listen listen;
  if (!mode || !mode[0]) mode = "set0";
  P("[drvva] mode="); P(mode); P("\n");

  listen      = (fn_listen)     dlsym(RTLD_DEFAULT, "mib3shim_zoom_listen");
  g_send      = (fn_send)       dlsym(RTLD_DEFAULT, "mib3shim_viewarea_send");
  g_pump      = (fn_pump)       dlsym(RTLD_DEFAULT, "mib3shim_viewarea_pump");
  g_ctl       = (fn_ctl)        dlsym(RTLD_DEFAULT, "mib3shim_ctl_line");
  g_count     = (fn_count)      dlsym(RTLD_DEFAULT, "stubcmd_count");
  g_lasttype  = (fn_lasttype)   dlsym(RTLD_DEFAULT, "stubcmd_last_type");
  g_ackcalls  = (fn_ackcalls)   dlsym(RTLD_DEFAULT, "stubcmd_ack_calls");
  g_ackstatus = (fn_ackstatus)  dlsym(RTLD_DEFAULT, "stubcmd_last_ack_status");
  Pn("shim _viewarea_send", (long)g_send);
  Pn("shim _viewarea_pump", (long)g_pump);
  Pn("stubcmd_count", (long)g_count);

  setenv("MIB3_SHIM_CMDS", "1", 1);
  /* The generation guard's own liveness byte is a REVERSE-ENGINEERED offset; this
   * driver sets it so the guard PASSES (the wheel bench deliberately does the
   * opposite to prove the guard refuses). */
  setenv("MIB3_SHIM_CMDALIVE", "1", 1);
  setenv("MIB3_SHIM_ZOOM", "0", 1);     /* no listener thread needed here */

  g_session = calloc(1, 0x2000);
  if (!g_session){ P("[drvva] calloc failed\n"); _exit(4); }
  *(volatile signed char *)((char *)g_session + 0x446) = 1;   /* "session started" */
  if (listen) listen(g_session, "C1057E12-A0D1-4E20-B0C0-1057E12A0D10", "");
  else { P("[drvva] no mib3shim_zoom_listen\n"); _exit(3); }

  if (streq(mode, "advert_off") || streq(mode, "advert_on")){
    if (streq(mode, "advert_on")){
      setenv("MIB3_SHIM_VIEWAREAS", "1", 1);
      setenv("MIB3_SHIM_VA_COUNT", "3", 1);
    }
    advert();
    P("[drvva] done\n");
    _exit(0);
  }

  if (streq(mode, "set0")){ setenv("MIB3_SHIM_VIEWAREAS", "1", 1); setenv("MIB3_SHIM_VA_COUNT", "2", 1); g_send(g_session, 0); }
  else if (streq(mode, "set1")){ setenv("MIB3_SHIM_VIEWAREAS", "1", 1); setenv("MIB3_SHIM_VA_COUNT", "2", 1); g_send(g_session, 1); }
  else if (streq(mode, "bounds")){ setenv("MIB3_SHIM_VIEWAREAS", "1", 1); setenv("MIB3_SHIM_VA_COUNT", "2", 1); g_send(g_session, 9); }
  else if (streq(mode, "off")){ setenv("MIB3_SHIM_VIEWAREAS", "0", 1); g_send(g_session, 1); }
  else if (streq(mode, "nopcmd")){ setenv("MIB3_SHIM_VIEWAREAS", "1", 1); setenv("MIB3_SHIM_VA_CMD", "0", 1); g_send(g_session, 1); }
  else if (streq(mode, "noack")){ setenv("MIB3_SHIM_VIEWAREAS", "1", 1); setenv("MIB3_SHIM_VA_COUNT", "2", 1); setenv("MIB3_SHIM_CMDACK", "0", 1); g_send(g_session, 1); }
  else if (streq(mode, "index2")){ setenv("MIB3_SHIM_VIEWAREAS", "1", 1); setenv("MIB3_SHIM_VA_COUNT", "3", 1); g_send(g_session, 2); }
  else if (streq(mode, "ctl")){ setenv("MIB3_SHIM_VIEWAREAS", "1", 1); setenv("MIB3_SHIM_VA_COUNT", "3", 1);
                                g_ctl("v2"); Pn("pump shipped", g_pump(g_session, 1)); }
  else if (streq(mode, "next")){ setenv("MIB3_SHIM_VIEWAREAS", "1", 1); setenv("MIB3_SHIM_VA_COUNT", "2", 1);
                                 g_ctl("next"); Pn("pump shipped", g_pump(g_session, 1)); }
  else if (streq(mode, "appear")){ setenv("MIB3_SHIM_CMDS", "1", 1);
                                   g_ctl("a map 1 2"); Pn("pump shipped", 0);
                                   { fn_pump ap = (fn_pump) dlsym(RTLD_DEFAULT, "mib3shim_appear_pump");
                                     if (ap) Pn("appear pump shipped", ap(g_session, 1)); } }
  else P("[drvva] unknown mode\n");

  if (g_count) Pn("stubcmd calls", g_count());
  if (g_lasttype){ P("[drvva] last type="); P(g_lasttype()); P("\n"); }
  if (g_ackcalls) Pn("ack calls", g_ackcalls());
  if (g_ackstatus) Pn("last ack status", g_ackstatus());
  P("[drvva] done\n");
  _exit(0);
}
