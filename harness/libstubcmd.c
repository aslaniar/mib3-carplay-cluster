/* ============================================================================
 * libstubcmd.c -- DESK STUB of AirPlayReceiverSessionSendCommand (2026-10-18).
 *
 * The shim resolves the command shipper with dlsym(RTLD_NEXT, ...).  If this
 * stub is preloaded AFTER the shim
 *      LD_PRELOAD="mib3shim.so libstubcmd.so"
 * then RTLD_NEXT from inside the shim finds THIS function instead of the real
 * libAirPlay one -- so the bench reads exactly what the shim would have put on
 * the wire, with no phone and no risk of driving a real session.
 *
 * HOW IT READS THE DICTIONARY (three dead ends led here -- worth recording):
 *   * libAirPlay does NOT contain CFLite: its CF* symbols are UNDEFINED and are
 *     satisfied by lge/app_rw/lib64/libCoreUtils.so.  So this stub links
 *     libCoreUtils, the same CFLite the shim's dlsym resolves.
 *   * A key LOOKUP (CFDictionaryGetValue with a freshly created equal CFString)
 *     returned NULL on the shim's dictionary in the desk bench, so this stub
 *     does not rely on lookups at all: it ENUMERATES
 *     (CFDictionaryGetCount + CFDictionaryGetKeysAndValues) and classifies each
 *     value by CFGetTypeID before touching it.  That is also safer than
 *     string-casting unknown values.
 *   * the log flags were O_WRONLY|O_CREAT|0x200; 0x200 is O_TRUNC on Linux, so
 *     every line but the last was erased.
 *
 * Built like the other harness stubs: aarch64, -nostdlib, against the unit's
 * glibc + libCoreUtils, loaded as LD_PRELOAD.
 * ==========================================================================*/
typedef unsigned long u64;

extern long  write(int, const void *, unsigned long);
extern int   open(const char *, int, ...);
extern int   close(int);

/* CFLite as shipped in libCoreUtils.so */
extern unsigned long CFGetTypeID(const void *cf);
extern unsigned long CFStringGetTypeID(void);
extern unsigned long CFNumberGetTypeID(void);
extern unsigned long CFDictionaryGetTypeID(void);
extern long  CFDictionaryGetCount(const void *dict);
extern void  CFDictionaryGetKeysAndValues(const void *dict, const void **keys, const void **values);
extern int   CFStringGetCString(const void *s, char *buf, long size, unsigned enc);
extern int   CFNumberGetValue(const void *num, int theType, void *valuePtr);

#define KUTF8 0x08000100u
#define O_WRONLY 1
#define O_CREAT  0100
#define O_APPEND 02000
#define TYPE_STR 0x0001u

static int  g_count;
static char g_last[256];

static unsigned long slen(const char *s){ unsigned long n = 0; while (s && s[n]) n++; return n; }
static void app(char *d, int *n, const char *s){ while (s && *s && *n < 250) d[(*n)++] = *s++; }
static void appn(char *d, int *n, long v){
  char t[16]; int i = 0; unsigned long u;
  if (v < 0){ app(d, n, "-"); u = (unsigned long)(-v); } else u = (unsigned long)v;
  if (!u){ app(d, n, "0"); return; }
  while (u){ t[i++] = (char)('0' + (u % 10)); u /= 10; }
  while (i) d[(*n)++] = t[--i];
}
static void flushlog(const char *line, unsigned long n){
  int fd = open("/tmp/zoomcmd.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0){ write(fd, line, n); close(fd); }
}
static void emit(const char *line, int n){ char b[256]; int i;
  for (i = 0; i < n && i < 250; i++) b[i] = line[i];
  b[n < 250 ? n : 250] = 0;
  flushlog(b, slen(b)); flushlog("\n", 1);
}
/* direct linkage (this stub NEEDs libCoreUtils), so no null guards needed */
static int is_string(const void *v){ return v && CFGetTypeID(v) == CFStringGetTypeID(); }
static int is_number(const void *v){ return v && CFGetTypeID(v) == CFNumberGetTypeID(); }
static int is_dict(const void *v){ return v && CFGetTypeID(v) == CFDictionaryGetTypeID(); }
/* "key=value" for a scalar; nested dicts get their own line prefixed `sub.` */
static void dump_dict(const void *d, const char *prefix, int depth)
{
  const void *keys[24]; const void *vals[24];
  long n, i;
  if (!d || depth > 2) return;
  n = CFDictionaryGetCount(d);
  if (n > 24) n = 24;
  if (n <= 0) return;
  CFDictionaryGetKeysAndValues(d, keys, vals);
  for (i = 0; i < n; i++){
    char line[256]; int k = 0;
    char kbuf[64], vbuf[96];
    kbuf[0] = 0; vbuf[0] = 0;
    if (!is_string(keys[i])) continue;
    if (!CFStringGetCString(keys[i], kbuf, sizeof(kbuf), KUTF8)){ kbuf[0] = '?'; kbuf[1] = 0; }
    app(line, &k, prefix); app(line, &k, kbuf); app(line, &k, "=");
    if (is_string(vals[i])){
      if (!CFStringGetCString((void*)vals[i], vbuf, sizeof(vbuf), KUTF8)){ vbuf[0] = '?'; vbuf[1] = 0; }
      app(line, &k, vbuf);
      emit(line, k);
    } else if (is_number(vals[i])){
      long long num = -999999;                       /* kCFNumberSInt64Type == 4 */
      int ok = CFNumberGetValue((void*)vals[i], 4, &num);
      app(line, &k, ok ? "num:" : "num(?)");
      appn(line, &k, (long)num);
      emit(line, k);
    } else if (is_dict(vals[i])){
      char subp[64]; int j = 0, t = 0;
      while (prefix[t] && j < 40) subp[j++] = prefix[t++];
      app(subp, &j, kbuf); app(subp, &j, ".");
      subp[j] = 0;
      emit(line, k);                                  /* "params.<key> =" marker */
      dump_dict(vals[i], subp, depth + 1);
    } else {
      app(line, &k, "other");
      emit(line, k);
    }
  }
}

int stubcmd_count(void){ return g_count; }
const char *stubcmd_last(void){ return g_last; }

int AirPlayReceiverSessionSendCommand(void *session, void *cmd, void *completion, void *ctx)
{
  char line[256]; int n = 0;
  (void)session; (void)completion; (void)ctx;
  g_count++;
  n = 0;
  app(line, &n, "SEND #"); appn(line, &n, g_count);
  app(line, &n, " shape=");
  if (is_dict(cmd)) app(line, &n, "dict"); else app(line, &n, "NOT-A-DICT");
  emit(line, n);
  dump_dict(cmd, "", 0);
  { int i; for (i = 0; i < n && i < 255; i++) g_last[i] = line[i]; g_last[n] = 0; }
  return 0;
}
