/* ============================================================================
 * libstubcmd.c -- DESK STUB of AirPlayReceiverSessionSendCommand (2026-10-18).
 *
 * The shim resolves the command shipper with dlsym(RTLD_NEXT, ...).  If this
 * stub is preloaded AFTER the shim
 *      LD_PRELOAD="mib3shim.so libstubcmd.so"
 * then RTLD_NEXT from inside the shim finds THIS function instead of the real
 * libAirPlay one -- so the bench can read exactly what the shim would have put
 * on the wire, without a phone, and with no risk of driving a real session.
 *
 * It records, per command:  type, params.uuid, params.zoomDirection (and, for
 * completeness, nightMode / url when present) to /tmp/zoomcmd.log, and counts
 * calls for the driver to assert on.
 *
 * Built like the other harness stubs: aarch64, -nostdlib, against the unit's
 * glibc, loaded as LD_PRELOAD.
 * ==========================================================================*/
typedef unsigned long u64;
typedef unsigned char u8;

extern long  write(int, const void *, unsigned long);
extern int   open(const char *, int, ...);
extern int   close(int);
extern void *dlsym(void *, const char *);
#define RTLD_DEFAULT ((void *)0)

typedef void *(*fn_create)(void *, const char *, unsigned);
typedef void *(*fn_get)(const void *, const void *);
typedef long  (*fn_getint)(const void *, const void *, long);

static int  g_count;
static char g_last[256];

static unsigned long slen(const char *s){ unsigned long n = 0; while (s && s[n]) n++; return n; }
static void app(char *d, int *n, const char *s){ while (s && *s && *n < 250) d[(*n)++] = *s++; }
static void appn(char *d, int *n, long v){
  char t[16]; int i = 0; if (v < 0){ app(d, n, "-"); v = -v; }
  if (!v){ app(d, n, "0"); return; }
  while (v){ t[i++] = (char)('0' + (v % 10)); v /= 10; }
  while (i) d[(*n)++] = t[--i];
}
static void flushlog(const char *line, unsigned long n){
  int fd = open("/tmp/zoomcmd.log", 0x1 /*O_WRONLY*/ | 0x40 /*O_CREAT*/ | 0x200 /*O_APPEND*/, 0644);
  if (fd >= 0){ write(fd, line, n); close(fd); }
}

int stubcmd_count(void){ return g_count; }
const char *stubcmd_last(void){ return g_last; }

/* {type, params} -> one log line.  Returns 0 always (the shim only checks rc
 * for logging; 0 is what a successful stock send returns). */
int AirPlayReceiverSessionSendCommand(void *session, void *cmd, void *completion, void *ctx)
{
  fn_create cfstr = (fn_create)dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
  fn_get    dget  = (fn_get)   dlsym(RTLD_DEFAULT, "CFDictionaryGetValue");
  fn_getint dgeti = (fn_getint)dlsym(RTLD_DEFAULT, "CFDictionaryGetInt64");
  char line[256]; int n = 0; char type[48]; char uuid[64];
  (void)session; (void)completion; (void)ctx;
  type[0] = 0; uuid[0] = 0;
  g_count++;
  if (cfstr && dget){
    void *kt = cfstr(0, "type", 0x08000100);
    void *vt = kt ? dget(cmd, kt) : 0;
    if (vt){
      char tmp[64]; int i = 0;
      /* CFStringGetCString if present, else the description is unavailable */
      { typedef int (*fn_toc)(const void *, char *, long, unsigned);
        fn_toc toc = (fn_toc)dlsym(RTLD_DEFAULT, "CFStringGetCString");
        tmp[0] = 0;
        if (toc) toc(vt, tmp, sizeof(tmp), 0x08000100);
        while (tmp[i] && i < 40){ type[i] = tmp[i]; i++; }
        type[i] = 0; }
    }
    { void *kp = cfstr(0, "params", 0x08000100);
      void *pd = kp ? dget(cmd, kp) : 0;
      if (pd){
        void *ku = cfstr(0, "uuid", 0x08000100);
        void *vu = ku ? dget(pd, ku) : 0;
        if (vu){
          typedef int (*fn_toc)(const void *, char *, long, unsigned);
          fn_toc toc = (fn_toc)dlsym(RTLD_DEFAULT, "CFStringGetCString");
          char tmp[80]; int i = 0; tmp[0] = 0;
          if (toc) toc(vu, tmp, sizeof(tmp), 0x08000100);
          while (tmp[i] && i < 60){ uuid[i] = tmp[i]; i++; }
          uuid[i] = 0;
        }
        { void *kz = cfstr(0, "zoomDirection", 0x08000100);
          if (kz && dgeti && dget(pd, kz)){
            long v = dgeti(pd, kz, -999);
            app(line, &n, "CMD type=");         app(line, &n, type);
            app(line, &n, " uuid=");            app(line, &n, uuid);
            app(line, &n, " zoomDirection=");   appn(line, &n, v);
            app(line, &n, "\n");
            line[n] = 0; flushlog(line, (unsigned long)n);
            { int i; for (i = 0; i <= n && i < 255; i++) g_last[i] = line[i]; }
            return 0;
          } }
        { void *kn = cfstr(0, "nightMode", 0x08000100);
          if (kn && dgeti && dget(pd, kn)){
            long v = dgeti(pd, kn, -999);
            app(line, &n, "CMD type=");       app(line, &n, type);
            app(line, &n, " uuid=");          app(line, &n, uuid);
            app(line, &n, " nightMode=");     appn(line, &n, v);
            app(line, &n, "\n");
            line[n] = 0; flushlog(line, (unsigned long)n);
            { int i; for (i = 0; i <= n && i < 255; i++) g_last[i] = line[i]; }
            return 0;
          } }
      } }
  }
  app(line, &n, "CMD type="); app(line, &n, type);
  app(line, &n, " uuid=");    app(line, &n, uuid);
  app(line, &n, "\n");
  line[n] = 0; flushlog(line, (unsigned long)n);
  { int i; for (i = 0; i <= n && i < 255; i++) g_last[i] = line[i]; }
  return 0;
}
