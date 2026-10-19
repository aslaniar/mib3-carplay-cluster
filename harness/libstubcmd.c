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
extern unsigned long CFArrayGetTypeID(void);
extern long  CFDictionaryGetCount(const void *dict);
extern void  CFDictionaryGetKeysAndValues(const void *dict, const void **keys, const void **values);
extern int   CFStringGetCString(const void *s, char *buf, long size, unsigned enc);
extern int   CFNumberGetValue(const void *num, int theType, void *valuePtr);
extern long  CFArrayGetCount(const void *arr);
extern const void *CFArrayGetValueAtIndex(const void *arr, long idx);
extern char *getenv(const char *);

#define KUTF8 0x08000100u
#define O_WRONLY 1
#define O_CREAT  0100
#define O_APPEND 02000
#define TYPE_STR 0x0001u

static int  g_count;
static char g_last[256];
static char g_lasttype[32];
static int  g_ack_calls;
static int  g_last_ack_status = -12345;

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
static int is_array(const void *v){ return v && CFGetTypeID(v) == CFArrayGetTypeID(); }
/* one scalar rendered into `line` (which the caller has already prefixed with
 * "<path>=" ) */
static int scalar_to(char *line, int k, const void *v)
{
  char vbuf[96];
  vbuf[0] = 0;
  if (is_string(v)){
    if (!CFStringGetCString(v, vbuf, sizeof(vbuf), KUTF8)){ vbuf[0] = '?'; vbuf[1] = 0; }
    app(line, &k, vbuf);
  } else if (is_number(v)){
    long long num = -999999;                       /* kCFNumberSInt64Type == 4 */
    int ok = CFNumberGetValue(v, 4, &num);
    app(line, &k, ok ? "num:" : "num(?)");
    appn(line, &k, (long)num);
  } else if (v == 0) {
    app(line, &k, "null");
  } else if (is_dict(v)) {
    app(line, &k, "dict");
  } else if (is_array(v)) {
    app(line, &k, "array");
  } else {
    app(line, &k, "other");
  }
  return k;
}
/* "key=value" for a scalar; nested dicts/arrays get their own lines prefixed
 * with the parent path.  ARRAYS ARE DUMPED (added 2026-10-19 for the view-area
 * lane): `<path>=array[N]` then one `<path>[i]=<value>` line per element, which
 * is what makes adjacentViewAreas == [] vs [0] assertable. */
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
    char kbuf[64];
    kbuf[0] = 0;
    if (!is_string(keys[i])) continue;
    if (!CFStringGetCString(keys[i], kbuf, sizeof(kbuf), KUTF8)){ kbuf[0] = '?'; kbuf[1] = 0; }
    app(line, &k, prefix); app(line, &k, kbuf); app(line, &k, "=");
    if (is_dict(vals[i])){                          /* keep the existing shape */
      char subp[64]; int j = 0, t = 0;
      while (prefix[t] && j < 40) subp[j++] = prefix[t++];
      app(subp, &j, kbuf); app(subp, &j, ".");
      subp[j] = 0;
      emit(line, k);                                /* "<path>key=" marker */
      dump_dict(vals[i], subp, depth + 1);
    } else if (is_array(vals[i])){
      long m = CFArrayGetCount(vals[i]), q;
      char apx[64]; int j = 0, t = 0;
      while (prefix[t] && j < 40) apx[j++] = prefix[t++];
      app(apx, &j, kbuf);
      apx[j] = 0;                                   /* MUST terminate: apx is
                                                     * reused as a string below */
      { int kk = k; app(line, &kk, "array["); appn(line, &kk, m); app(line, &kk, "]"); emit(line, kk); }
      if (m > 16) m = 16;
      for (q = 0; q < m; q++){
        char el[256]; int kk = 0;
        app(el, &kk, apx); el[kk++] = '['; appn(el, &kk, q); el[kk++] = ']'; el[kk++] = '=';
        kk = scalar_to(el, kk, CFArrayGetValueAtIndex(vals[i], q));
        emit(el, kk);
        /* nested dict inside an array: dump its scalars one level down */
        if (is_dict(CFArrayGetValueAtIndex(vals[i], q))){
          char subp[72]; int j2 = 0, t2 = 0;
          while (apx[t2] && j2 < 40) subp[j2++] = apx[t2++];
          subp[j2++] = '['; 
          { char d2[12]; int m2 = 0, x = (int)q; if (!x) d2[m2++]='0'; while(x){ d2[m2++]=(char)('0'+x%10); x/=10; }
            while (m2) subp[j2++] = d2[--m2]; }
          subp[j2++] = ']'; subp[j2++] = '.'; subp[j2] = 0;
          dump_dict(CFArrayGetValueAtIndex(vals[i], q), subp, depth + 1);
        }
      }
    } else {
      k = scalar_to(line, k, vals[i]);
      emit(line, k);
    }
  }
}
/* the "type" value of the LAST captured command ("" if none) */
const char *stubcmd_last_type(void){ return g_lasttype; }
int stubcmd_ack_calls(void){ return g_ack_calls; }
int stubcmd_last_ack_status(void){ return g_last_ack_status; }

int stubcmd_count(void){ return g_count; }
const char *stubcmd_last(void){ return g_last; }

int AirPlayReceiverSessionSendCommand(void *session, void *cmd, void *completion, void *ctx)
{
  char line[256]; int n = 0;
  int status = 0;
  (void)session;
  g_count++;
  g_lasttype[0] = 0;
  n = 0;
  app(line, &n, "SEND #"); appn(line, &n, g_count);
  app(line, &n, " shape=");
  if (is_dict(cmd)) app(line, &n, "dict"); else app(line, &n, "NOT-A-DICT");
  app(line, &n, " ack=");
  app(line, &n, completion ? "yes" : "no");
  emit(line, n);
  if (is_dict(cmd)){
    const void *tv = 0;
    /* read the "type" string so the driver can assert on it directly */
    { const void *keys[24]; const void *vals[24]; long m = CFDictionaryGetCount(cmd), i;
      if (m > 24) m = 24;
      if (m > 0){
        CFDictionaryGetKeysAndValues(cmd, keys, vals);
        for (i = 0; i < m; i++){
          char kb[32]; kb[0] = 0;
          if (!is_string(keys[i])) continue;
          if (!CFStringGetCString(keys[i], kb, sizeof(kb), KUTF8)) continue;
          if (kb[0]=='t' && kb[1]=='y' && kb[2]=='p' && kb[3]=='e' && kb[4]==0){ tv = vals[i]; break; }
        }
      } }
    if (is_string(tv)){
      int i2 = 0;
      if (!CFStringGetCString(tv, g_lasttype, sizeof(g_lasttype), KUTF8)) g_lasttype[0] = 0;
      for (i2 = 0; g_lasttype[i2] && i2 < 31; i2++) ;
      g_lasttype[i2] = 0;
    }
  }
  dump_dict(cmd, "", 0);
  { int i; for (i = 0; i < n && i < 255; i++) g_last[i] = line[i]; g_last[n < 256 ? n : 255] = 0; }
  /* ---- THE COMPLETION CALLBACK -------------------------------------------
   * The real libAirplay calls completion(int status, CFDictionaryRef resp|NULL,
   * void *ctx) from its HTTP-client thread once the phone answers POST
   * /command.  Here we call it synchronously with a status the bench chooses,
   * which is how "the callback fires and a refusal is logged" is asserted.
   *   STUBCMD_ACK_STATUS=<n>   default 0 (= the phone accepted)
   *   STUBCMD_ACK_NONE=1       never call it (the old, blind behaviour)
   * ---------------------------------------------------------------------*/
  if (completion && !getenv("STUBCMD_ACK_NONE")){
    char *s = getenv("STUBCMD_ACK_STATUS");
    if (s && s[0] >= '0' && s[0] <= '9'){
      int v = 0; while (*s >= '0' && *s <= '9'){ v = v * 10 + (*s - '0'); s++; }
      status = v;
    }
    g_last_ack_status = status;
    g_ack_calls++;
    { void (*cb)(int, void *, void *) = (void (*)(int, void *, void *))completion;
      cb(status, 0, ctx); }
  }
  return 0;
}
