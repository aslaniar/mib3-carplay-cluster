/* ============================================================================
 * mib3night.c -- the DAY/NIGHT watcher: car theme -> CarPlay cluster map
 *                appearance (2026-10-26, LANE_GEN_FIX_2026-10-26.md).
 *
 *   *** runs ON THE UNIT (aarch64); the same source is benchable on the desk ***
 *
 * THE DEFECT IT FIXES (B3 of the 2026-10-25 session)
 * ---------------------------------------------------
 * The cluster rendered LIGHT while the centre screen was DARK.  Two causes,
 * both in mib3shim.c's appearance path:
 *   (a) the per-surface appearance defaults targeted the UI layer
 *       (MIB3_SHIM_APPTARGET=ui), never the map;
 *   (b) the value was circular (g_night_mode came from the nightMode WE
 *       advertise, not from the car).
 * The dynamic source exists and is AUTHORITATIVE: the unit's own hmiintegration
 * service publishes the day/night state, and the STOCK carplay binary itself
 * consumes exactly this subscription (corpus carplay/decompiled.c,
 * CHMIIntegrationClient: WS subscribe "/hmiintegration/dayNight/" on 127.0.0.1;
 * on every message it walks msg["data"][] and calls
 * CFrameworkService::SetNightMode(1) when an element's "name" == "nightmode",
 * else SetNightMode(0)).  That is what keeps the CENTRE screen dark at night.
 *
 * THE CONTRACT THIS TOOL IMPLEMENTS (read from the shipped binary, not guessed):
 *   night  <=>  the dayNight collection contains an element named "nightmode"
 *   day    <=>  it does not (the other element is named something else)
 * No state enum is needed; presence of the name IS the boolean.
 *
 * WHAT IT DOES WITH THE EDGE
 * --------------------------
 * On every CHANGE it writes ONE line to the shim's control inbox
 * (/dev/shm/mib3zoom.sock):
 *     a <map|ui> <mode> <setting>
 * which the shim's cluster thread turns into mapAppearanceUpdate /
 * uiAppearanceUpdate {uuid, appearanceMode, appearanceSetting} -- exactly the
 * rgi reference's [target][mode][setting] triple (altscreen_hook.c:2672-2683).
 * No shim rebuild, no re-injection: the inbox verb and the appearance pump are
 * already live in the deployed shim (mib3shim.c ctl_handle_line 'a' branch ->
 * ap_push -> mib3shim_appear_pump on the cluster thread).
 *
 * WHY POLL AND NOT SUBSCRIBE: the same measured reason as mib3wheel -- the
 * unit's RSI does not reliably PUSH element state on every service (the
 * renderers tap had to be polled, 2026-10-19).  dayNight DOES push to the stock
 * WS client, but a GET poll every second costs one 127.0.0.1 round trip and
 * cannot wedge, whereas a subscription client is a second lifetime contract.
 * Day/night changes are HUMAN-slow (lights on once per drive, typically), so a
 * 1 s poll adds at most 1 s of lag to an edge nobody races.
 *
 * USAGE
 *   mib3night [--probe] [--host H] [--port N] [--path /hmiintegration/dayNight]
 *             [--socket PATH] [--poll-ms N] [--target map|ui|both]
 *             [--night-mode N] [--day-mode N] [--setting N] [--invert]
 *             [--quiet] [--selftest]
 *   --probe     one GET, print status + the raw body + the parsed verdict.
 *               RUN THIS FIRST ON THE CAR: it is the evidence that the service
 *               is on 49570 and that "nightmode" is in the collection.
 *   defaults: host 127.0.0.1, port 49570 (the HMI-state service; 49506 is a
 *             DIFFERENT service -- phonemecontentnetwork -- do not use it),
 *             path /hmiintegration/dayNight, socket /dev/shm/mib3zoom.sock,
 *             poll 1000 ms, target map (the map layer is the one that renders
 *             wrong), night->mode 1, day->mode 0.
 *
 * SAFETY: read-only HTTP GET on localhost + one short line to an AF_UNIX socket
 * in /dev/shm.  No RSI mutation, no display call, no service.  A reboot is the
 * full undo; killing the tool stops the mirroring instantly.
 * ==========================================================================*/
#define NULL ((void*)0)
typedef unsigned int  u32;
typedef unsigned long usize;
typedef long          s64;

extern long   read(int, void *, unsigned long);
extern long   write(int, const void *, unsigned long);
extern int    close(int);
extern int    open(const char *, int, ...);
extern void   _exit(int);
extern int    socket(int, int, int);
extern int    connect(int, const void *, unsigned int);
extern int    setsockopt(int, int, int, const void *, unsigned int);
extern int    usleep(unsigned int);
extern long   time(long *);
extern int    clock_gettime(int, void *);

static usize slen(const char *s){ usize n = 0; while (s && s[n]) n++; return n; }
static void P(int fd, const char *s){ write(fd, s, slen(s)); }
static void PN(int fd, s64 v){
  char b[24]; int i = 0; unsigned long long u;
  if (v < 0){ write(fd, "-", 1); u = (unsigned long long)(-v); } else u = (unsigned long long)v;
  if (!u){ write(fd, "0", 1); return; }
  while (u){ b[i++] = (char)('0' + (u % 10)); u /= 10; }
  while (i) write(fd, &b[--i], 1);
}
static int streq(const char *a, const char *b){ while (*a && *a == *b){ a++; b++; } return *a == *b; }
static int my_atoi(const char *s){
  long v = 0; int neg = 0;
  if (!s) return 0;
  if (*s == '-'){ neg = 1; s++; }
  while (*s >= '0' && *s <= '9'){ v = v * 10 + (*s - '0'); s++; }
  return (int)(neg ? -v : v);
}
static void *memset_(void *d, int c, usize n){ unsigned char *p = (unsigned char*)d; usize i;
  for (i = 0; i < n; i++) p[i] = (unsigned char)c; return d; }
static void *memcpy_(void *d, const void *s, usize n){ unsigned char *a = (unsigned char*)d;
  const unsigned char *b = (const unsigned char*)s; usize i; for (i = 0; i < n; i++) a[i] = b[i]; return d; }

#ifdef HOST_TEST
extern int *__error(void);
#define ERRNO() (*__error())
#else
extern int *__errno_location(void);
#define ERRNO() (*__errno_location())
#endif

static int g_quiet;
static void L(const char *tag, const char *msg){
  if (g_quiet) return;
  write(2, "[night] ", 8); P(2, tag); if (msg && msg[0]){ write(2, ": ", 2); P(2, msg); }
  write(2, "\n", 1);
}
static void LV(const char *tag, s64 v){
  if (g_quiet) return;
  write(2, "[night] ", 8); P(2, tag); write(2, "=", 1); PN(2, v); write(2, "\n", 1);
}

/* ---- HTTP/1.1 GET (localhost RSI is plain HTTP; copied from mib3wheel) --- */
static int tcp_connect(const char *host, int port){
  struct { unsigned short fam; unsigned short port; unsigned int ip; char pad[8]; } sa;
  int fd = socket(2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0);
  if (fd < 0) return -1;
  memset_(&sa, 0, sizeof(sa));
  sa.fam = 2; sa.port = (unsigned short)((port << 8) | ((port >> 8) & 0xff));
  { unsigned int ip = 0; int n = 0, part = 0;
    const char *h = host;
    for (;;){ int v = 0, got = 0;
      while (*h >= '0' && *h <= '9'){ v = v * 10 + (*h - '0'); h++; got = 1; }
      if (!got) return (close(fd), -1);
      if (part < 4) ip = (ip << 8) | (unsigned int)(v & 0xff);
      part++; n++;
      if (*h == '.'){ h++; if (n >= 4) return (close(fd), -1); continue; }
      if (*h == 0) break;
      return (close(fd), -1); }
    if (n != 4) return (close(fd), -1);
    sa.ip = ((ip & 0xff) << 24) | ((ip & 0xff00) << 8) | ((ip >> 8) & 0xff00) | ((ip >> 24) & 0xff);
  }
  if (connect(fd, &sa, (unsigned int)sizeof(sa)) != 0){ close(fd); return -1; }
  return fd;
}
static int wfull(int fd, const void *buf, usize n){
  usize o = 0;
  while (o < n){ long r = write(fd, (const char*)buf + o, n - o); if (r <= 0) return 0; o += (usize)r; }
  return 1;
}
static int http_get(const char *host, int port, const char *path, char *out, usize cap, usize *bodylen){
  int fd; char req[512]; usize rl = 0; usize got = 0; char *hdr_end = 0;
  *bodylen = 0;
  if (cap < 64) return 0;
  fd = tcp_connect(host, port);
  if (fd < 0) return 0;
  { struct { long s; long u; } tv; tv.s = 4; tv.u = 0;     /* never wedge on a stuck service */
    setsockopt(fd, 1 /*SOL_SOCKET*/, 20 /*SO_RCVTIMEO*/, &tv, (unsigned int)sizeof(tv)); }
  { const char *a = "GET "; const char *b = path;
    const char *c = " HTTP/1.1\r\nHost: "; const char *d = host;
    const char *e = "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
    const char *parts[10]; usize lens[10]; int i;
    parts[0]=a; lens[0]=slen(a); parts[1]=b; lens[1]=slen(b);
    parts[2]=c; lens[2]=slen(c); parts[3]=d; lens[3]=slen(d);
    parts[4]=e; lens[4]=slen(e); parts[5]=0;
    for (i = 0; parts[i]; i++){
      if (rl + lens[i] >= sizeof(req)) { close(fd); return 0; }
      memcpy_(req + rl, parts[i], lens[i]); rl += lens[i];
    } }
  if (!wfull(fd, req, rl)){ close(fd); return 0; }
  for (;;){
    long r = read(fd, out + got, cap - 1 - got);
    if (r <= 0) break;
    got += (usize)r;
    if (got >= cap - 2) break;
    if (!hdr_end){ usize i; for (i = 0; i + 3 < got; i++) if (out[i]=='\r'&&out[i+1]=='\n'&&out[i+2]=='\r'&&out[i+3]=='\n'){ hdr_end = out + i + 4; break; } }
  }
  close(fd);
  out[got] = 0;
  { int status = 0;
    if (got > 12 && out[0]=='H') status = my_atoi(out + 9);
    /* Return the FULL response (headers included) and its length; the night
     * parser scans the whole buffer, so the body boundary does not matter. */
    *bodylen = got;
    return status;
  }
}

/* ---- the night verdict -----------------------------------------------------
 * The STOCK rule (carplay corpus): night <=> the dayNight collection contains
 * an element whose "name" is "nightmode".  We scan for "name":"nightmode" with
 * the usual JSON whitespace tolerance (space between tokens) and a
 * case-insensitive name compare, because VIWI services have shipped both
 * "nightmode" and "nightMode" spellings.  The scan is bounded and cannot
 * resume inside a string: we only ever look for the literal key "name" and then
 * the value that follows the next ':'.
 * Returns 1 night, 0 day, -1 cannot-tell (no parseable name at all). */
static int ci_prefix(const char *p, usize n, const char *lit){
  usize i;
  for (i = 0; i < n && lit[i]; i++){
    char a = p[i], b = lit[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return 0;
  }
  return lit[i] == 0 || i == n;
}
static int body_says_night(const char *body, usize blen){
  usize i = 0; int any_name = 0;
  if (!body || blen < 8) return -1;
  while (i + 6 < blen){
    if (body[i]=='n' || body[i]=='N'){
      if (ci_prefix(body + i, (usize)4, "name")){
        usize j = i + 4;
        /* the key's closing quote: `"name":"nightmode"` -- a `"` sits between
         * the key letters and the ':' (this exact omission failed the first
         * selftest run; whitespace tolerance is kept for tidy serializers). */
        while (j < blen && (body[j]==' ' || body[j]=='\t')) j++;
        if (j < blen && body[j]=='"') j++;
        while (j < blen && (body[j]==' ' || body[j]=='\t')) j++;
        if (j < blen && body[j]==':'){
          j++;
          while (j < blen && (body[j]==' ' || body[j]=='\t')) j++;
          if (j < blen && body[j]=='"'){
            usize k = j + 1, m = 0;
            const char *lit = "nightmode";
            while (k < blen && body[k] != '"') k++;
            m = k - (j + 1);
            any_name = 1;
            if (ci_prefix(body + j + 1, m, lit) && m == slen(lit)) return 1;  /* exact name match */
          }
        }
      }
    }
    i++;
  }
  return any_name ? 0 : -1;   /* saw element names, none was nightmode -> DAY */
}

/* ---- the shim inbox (line emitter; the mib3wheel emit mould) ------------- */
static int g_sock = -2;              /* -2 = not tried, -1 = disabled/closed */
static const char *g_sockpath = "/dev/shm/mib3zoom.sock";
#ifdef HOST_TEST
typedef struct { unsigned char len; unsigned char fam; char p[104]; } sun_t;
#else
typedef struct { unsigned short fam; char p[108]; } sun_t;
#endif
static void emit_close(void){ if (g_sock >= 0){ close(g_sock); } g_sock = -2; }
static int sun_fill(sun_t *sa, const char *path){
  usize n = 0;
  memset_(sa, 0, sizeof(*sa));
  sa->fam = 1;
  while (path[n] && n < sizeof(sa->p) - 1){ sa->p[n] = path[n]; n++; }
#ifdef HOST_TEST
  sa->len = (unsigned char)(n + 2);
#endif
  return 0;
}
/* Emit "a <target> <mode> <setting>\n".  Reconnects once.  Returns 1 sent. */
static int emit(const char *target, int mode, int setting){
  sun_t sa;
  char line[48]; usize n = 0; int tries;
  if (!g_sockpath || !g_sockpath[0]) return 0;
  { const char *s = "a "; while (*s && n < sizeof(line)-2) line[n++] = *s++;
    s = target;      while (*s && n < sizeof(line)-2) line[n++] = *s++;
    s = " ";         while (*s && n < sizeof(line)-2) line[n++] = *s++;
    { char nb[12]; int k = 0, v = mode; if (v < 0) v = 0;
      if (!v) nb[k++] = '0'; else while (v){ nb[k++] = (char)('0'+v%10); v/=10; }
      while (k && n < sizeof(line)-2) line[n++] = nb[--k]; }
    s = " ";         while (*s && n < sizeof(line)-2) line[n++] = *s++;
    { char nb[12]; int k = 0, v = setting; if (v < 0) v = 0;
      if (!v) nb[k++] = '0'; else while (v){ nb[k++] = (char)('0'+v%10); v/=10; }
      while (k && n < sizeof(line)-2) line[n++] = nb[--k]; }
    line[n++] = '\n'; line[n] = 0; }
  for (tries = 0; tries < 2; tries++){
    if (g_sock == -2){
      int fd = socket(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0);
      if (fd < 0){ L("inbox", "socket() failed"); return 0; }
      sun_fill(&sa, g_sockpath);
      if (connect(fd, &sa, (unsigned int)sizeof(sa)) != 0){
        LV("inbox.connect errno", ERRNO());
        close(fd); return 0;
      }
      g_sock = fd;
      L("inbox", "connected");
    }
    if (g_sock >= 0){
      if (wfull(g_sock, line, n)) return 1;
      emit_close();
    }
  }
  return 0;
}

/* ---- clock --------------------------------------------------------------- */
#ifdef HOST_TEST
#define CLK_MONOTONIC 6
#else
#define CLK_MONOTONIC 1
#endif
static s64 now_ms(void){
  struct { long s; long ns; } ts;
  ts.s = 0; ts.ns = 0;
  if (clock_gettime(CLK_MONOTONIC, &ts) != 0) return (s64)time(0) * 1000;
  return ts.s * 1000 + ts.ns / 1000000;
}

/* ---- probe --------------------------------------------------------------- */
static int do_probe(const char *host, int port, const char *path){
  static char buf[65536]; usize bl = 0;
  int st = http_get(host, port, path, buf, sizeof(buf), &bl);
  P(1, "PROBE "); PN(1, port); P(1, " "); P(1, path); P(1, " status="); PN(1, st);
  P(1, " bytes="); PN(1, (s64)bl); P(1, "\n");
  if (st == 200 && bl){
    int v = body_says_night(buf, bl);
    P(1, "NIGHT verdict="); PN(1, v); P(1, "  (1=night an element is named nightmode, 0=day, -1=no element names seen)\n");
    P(1, "RAW body:\n"); write(1, buf, bl); P(1, "\n");
  } else if (st != 200){
    P(1, "(status != 200: check the port.  49570 is the hmiintegration HMI-state\n");
    P(1, " service on this unit; 49506 is phonemecontentnetwork, a DIFFERENT one.)\n");
  }
  return 0;
}

/* ---- selftest (desk; HOST_TEST build) ------------------------------------ */
static int selftest(void){
  int fails = 0, checks = 0;
  struct { const char *body; int want; const char *why; } cases[] = {
    { "{\"type\":\"dayNight\",\"data\":[{\"id\":\"c0f29d05-fc7b-56ee-ac60-fea70a49e738\",\"name\":\"nightmode\"}]}", 1, "stock rule: element named nightmode" },
    { "{\"data\":[{\"id\":\"a1\",\"name\":\"daymode\"},{\"id\":\"b2\",\"name\":\"nightmode\"}]}", 1, "nightmode anywhere in the collection" },
    { "{\"data\":[{\"id\":\"a1\",\"name\":\"daymode\"}]}", 0, "only daymode -> day" },
    { "", -1, "empty body" },
    { "{\"error\":\"none\"}", -1, "no element names" },
    { "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"data\":[{\"name\":\"nightMode\"}]}", 1, "case-insensitive + whole response scanned" },
  };
  { u32 i;
    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++){
      int got = body_says_night(cases[i].body, slen(cases[i].body));
      checks++;
      if (got != cases[i].want){
        fails++;
        P(2, "[night] SELFTEST FAIL: "); P(2, cases[i].why);
        P(2, " want="); PN(2, cases[i].want); P(2, " got="); PN(2, got); P(2, "\n");
      }
    }
  }
  P(1, "SELFTEST "); PN(1, checks - fails); P(1, "/"); PN(1, checks); P(1, " checks, ");
  PN(1, fails); P(1, " failures\n");
  return fails ? 1 : 0;
}

/* ---- the watcher --------------------------------------------------------- */
static int main_watch(const char *host, int port, const char *path,
                      const char *target, int night_mode, int day_mode, int setting,
                      int invert, int poll_ms, int once)
{
  static char buf[65536];
  int last = -2;            /* -2 = never seen; the FIRST verdict is pushed too */
  s64 next_poll = 0, started = now_ms();
  long changes = 0, pushes = 0, gets = 0, errors = 0;
  L("watch", "start");
  LV("watch.port", port);
  LV("watch.poll_ms", poll_ms);
  for (;;){
    s64 t = now_ms();
    usize bl = 0;
    int st, v, mode;
    if (t < next_poll){ usleep(20000); continue; }
    next_poll = t + poll_ms;
    st = http_get(host, port, path, buf, sizeof(buf), &bl);
    gets++;
    if (st != 200 || !bl){
      errors++;
      if ((errors < 4) || ((errors % 30) == 0)) { LV("http.status", st); LV("http.errors", errors); }
      usleep(200000);
      continue;
    }
    v = body_says_night(buf, bl);
    if (v < 0){ errors++; usleep(200000); continue; }   /* cannot tell: never push garbage */
    if (invert) v = v ? 0 : 1;
    if (v != last){
      mode = v ? night_mode : day_mode;
      last = v; changes++;
      L(v ? "EDGE-night" : "EDGE-day", v ? "car -> NIGHT" : "car -> DAY");
      LV("edge.mode", mode);
      /* push to BOTH the map and (optionally) the ui layer.  The map layer is
       * the broken one (B3); 'both' is for the day a UI tint is wanted too. */
      { int sent1 = emit(target, mode, setting);
        if (streq(target, "both")) sent1 += emit("ui", mode, setting);
        if (sent1) pushes++;
        else L("inbox", "NOT SENT (is the shim's inbox up? it appears at the type-111 SETUP)");
      }
      if (once) break;
    }
    { /* a heartbeat every ~60 s so a stick log shows the watcher is alive */
      s64 up = now_ms() - started;
      if (up > 60000){ started = now_ms();
        LV("hb.gets", gets); LV("hb.changes", changes); LV("hb.pushes", pushes); LV("hb.errors", errors); }
    }
  }
  return 0;
}

/* ---- entry --------------------------------------------------------------- */
int tool_main(int argc, char **argv);
#ifdef HOST_TEST
int main(int argc, char **argv){ return tool_main(argc, argv); }
#else
void _start(void){
  static char cbuf[2048]; static char *av[64], *nav[64];
  int n = 0, argc = 0, i = 0, k, skip = 0;
  int fd = open("/proc/self/cmdline", 0);
  if (fd >= 0){ long r = read(fd, cbuf, (unsigned long)sizeof(cbuf) - 1); if (r > 0) n = (int)r; close(fd); }
  cbuf[n] = 0;
  while (i < n && argc < 63){ av[argc++] = &cbuf[i]; while (i < n && cbuf[i]) i++; i++; }
  av[argc] = 0;
  /* the loader case (see mib3wheel.c): a purely-numeric first token means the
   * program name was dropped (busybox nohup); re-insert a placeholder. */
  { const char *t = av[0];
    if (t && t[0] >= '0' && t[0] <= '9'){
      const char *p; int all = 1;
      for (p = t; *p; p++) if (*p < '0' || *p > '9'){ all = 0; break; }
      if (all && argc < 63){ int j; for (j = argc; j > 0; j--) av[j] = av[j-1];
                             av[0] = "x"; argc++; av[argc] = 0; } } }
  { const char *base = av[0], *p;
    for (p = av[0]; *p; p++) if (*p == '/') base = p + 1;
    if (base[0]=='l' && base[1]=='d' && (base[2]=='-' || base[2]=='.')){
      int j = 1;
      while (j < argc){
        const char *t2 = av[j];
        if (streq(t2, "--library-path") || streq(t2, "--preload") || streq(t2, "--audit") ||
            streq(t2, "--argv0") || streq(t2, "--inhibit-cache")){ j += 2; continue; }
        if (t2[0] == '-'){ j += 1; continue; }
        break;
      }
      skip = j;
    } }
  for (k = 0; skip + k < argc && k < 63; k++) nav[k] = av[skip + k];
  nav[k] = 0;
  _exit(tool_main(k, nav));
}
#endif

int tool_main(int argc, char **argv){
  const char *host = "127.0.0.1";
  const char *path = "/hmiintegration/dayNight";
  const char *target = "map";
  int port = 49570, probe = 0, once = 0, invert = 0, poll_ms = 1000;
  int night_mode = 1, day_mode = 0, setting = 0, selftest_mode = 0;
  int i;
  for (i = 1; i < argc; i++){
    const char *a = argv[i];
    if (streq(a, "--probe")) probe = 1;
    else if (streq(a, "--once")) once = 1;
    else if (streq(a, "--invert")) invert = 1;
    else if (streq(a, "--selftest")) selftest_mode = 1;
    else if (streq(a, "--quiet")) g_quiet = 1;
    else if (streq(a, "--host") && i + 1 < argc) host = argv[++i];
    else if (streq(a, "--port") && i + 1 < argc) port = my_atoi(argv[++i]);
    else if (streq(a, "--path") && i + 1 < argc) path = argv[++i];
    else if (streq(a, "--socket") && i + 1 < argc) g_sockpath = argv[++i];
    else if (streq(a, "--poll-ms") && i + 1 < argc) poll_ms = my_atoi(argv[++i]);
    else if (streq(a, "--target") && i + 1 < argc) target = argv[++i];
    else if (streq(a, "--night-mode") && i + 1 < argc) night_mode = my_atoi(argv[++i]);
    else if (streq(a, "--day-mode") && i + 1 < argc) day_mode = my_atoi(argv[++i]);
    else if (streq(a, "--setting") && i + 1 < argc) setting = my_atoi(argv[++i]);
    else if (streq(a, "--help") || streq(a, "-h")){
      P(1, "mib3night [--probe] [--host H] [--port N] [--path P] [--socket S]\n"
           "          [--poll-ms N] [--target map|ui|both] [--night-mode N] [--day-mode N]\n"
           "          [--setting N] [--invert] [--once] [--quiet] [--selftest]\n"
           "watches the car's dayNight element (hmiintegration :49570) and pushes\n"
           "an appearance update ('a map <mode> <setting>') to the shim's inbox on\n"
           "every day/night change, so the CarPlay cluster map follows the car's\n"
           "theme like the centre screen does.  Night <=> the collection has an\n"
           "element named 'nightmode' (the rule the STOCK carplay binary itself\n"
           "implements for the centre screen).\n");
      return 0; }
    else { P(2, "[night] unknown arg: "); P(2, a); P(2, "\n"); return 2; }
  }
  if (selftest_mode) return selftest();
  P(2, "[night] mib3night 1.0 rsi="); P(2, host); P(2, ":"); PN(2, port);
  P(2, path); P(2, " socket="); P(2, g_sockpath); P(2, " target="); P(2, target); P(2, "\n");
  if (probe) return do_probe(host, port, path);
  return main_watch(host, port, path, target, night_mode, day_mode, setting, invert, poll_ms, once);
}
