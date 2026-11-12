/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ============================================================================
 * mib3wheel.c -- STEERING-WHEEL up/down -> CarPlay cluster zoom.
 *
 *   *** runs ON THE UNIT (aarch64); the same source is benchable on the desk ***
 *
 * WHY THIS TOOL EXISTS
 * --------------------
 * On this unit the steering-wheel roller never reaches CarPlay.  It is a CAN key
 * (MFL_Tasten_Kon_01) that arrives as a BAP `NavigationSd.MapScale` SetGet, and
 * `navsd_app` (libMIBNavSDClientLib_CarIF, Viwi::NavSd::FctMapScale) turns it into
 * a POST of `{relativeSteps}` to the car's own maprenderer
 * ("/maprenderer/discreteInputs/<renderer>"): the car's nav zooms, navsd reports
 * the new BAP MapScale Status, and the CLUSTER paints its own scale overlay.
 * Because our CarPlay video replaced that map, the wheel moves a map nobody sees.
 *
 * The CarPlay side has a first-class command for exactly this case:
 *     changeMapZoomLevel { uuid: <cluster display uuid>, zoomDirection: 0|1 }
 * (0 = zoom in, non-zero = zoom out; DashBoard turns it into
 * CRSUIClusterZoomAction for the cluster scene).  Only the CarPlay process can
 * send it, so the shim sends it -- see mib3shim.c "ZOOM: the STEERING-WHEEL
 * up/down mirror".  THIS tool tells the shim the wheel moved: it watches the
 * unit's own renderer input and writes "+1"/"-1" lines into the shim's AF_UNIX
 * inbox (/dev/shm/mib3zoom.sock by default).
 *
 * TWO READ-ONLY TAPS (both RSI/VIWI over localhost, no writes, no state)
 * --------------------------------------------------------------------
 *  A. /maprenderer/discreteInputs  (AUTHORITATIVE) -- an element event when
 *     navsd posts relativeSteps.  `relativeSteps: -1|+1` is the step.
 *  B. /maprenderer/renderers       (BACKSTOP)      -- the renderer's own
 *     `distancePerPixel` changes when its map zooms.  Used only when `zoomMode`
 *     is NOT "auto", because auto-zoom moves the scale with vehicle speed.
 * Both are subscriptions; nothing is consumed, so the car's nav keeps working and
 * a wrong guess here cannot break it.  (On this unit the renderers tap does not
 * push -- see poll_renderers -- so a GET diff carries it.)
 *
 * MODE PROBE (start here on the car; zero risk, writes nothing)
 * ------------------------------------------------------------
 *   mib3wheel --probe          GET /maprenderer/renderers + /discreteInputs,
 *                              print the renderers (name/id/zoomMode/
 *                              distancePerPixel/discreteInput) and every
 *                              discreteInput element: the evidence that says
 *                              WHICH tap the wheel shows up on.
 *   mib3wheel --probe-all      additionally sweep the RSI port range and the
 *                              known service names (finds `input`, if that
 *                              service is up; its rotaryEncoderStates would be
 *                              the centre-console knob's encoder).
 *
 * USAGE
 *   mib3wheel [--probe|--probe-all] [--host H] [--port N]
 *             [--socket PATH] [--dump] [--invert] [--min-gap-ms N]
 *             [--no-renderers] [--once N] [--selftest] [--quiet]
 *
 * SAFETY: read-only RSI.  No service is touched, nothing is restarted, no display
 * call is made.  Killing it stops all zoom mirroring instantly.
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

/* ---- tiny libc-free helpers ---------------------------------------------- */
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

/* logging (stderr so stdout stays machine-readable) */
static int g_quiet;
static void L(const char *tag, const char *msg){
  if (g_quiet) return;
  write(2, "[wheel] ", 8); P(2, tag); if (msg && msg[0]){ write(2, ": ", 2); P(2, msg); }
  write(2, "\n", 1);
}
static void LV(const char *tag, s64 v){
  if (g_quiet) return;
  write(2, "[wheel] ", 8); P(2, tag); write(2, "=", 1); PN(2, v); write(2, "\n", 1);
}

/* ---- HTTP/1.1 GET body fetch (RSI is plain HTTP on localhost) ------------ */
static int tcp_connect(const char *host, int port){
  struct { unsigned short fam; unsigned short port; unsigned int ip; char pad[8]; } sa;
  int fd = socket(2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0);
  if (fd < 0) return -1;
  memset_(&sa, 0, sizeof(sa));
  sa.fam = 2; sa.port = (unsigned short)((port << 8) | ((port >> 8) & 0xff));
  { /* 127.0.0.1 only unless --host is a literal we can parse */ unsigned int ip = 0; int n = 0, part = 0;
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
/* GET path; returns HTTP status (0 = transport error), body in out (NUL-terminated).
 * Sets *bodylen.  Handles Content-Length and (unlikely) chunked-free case. */
static int http_get(const char *host, int port, const char *path, char *out, usize cap, usize *bodylen){
  int fd; char req[512]; usize rl = 0; usize got = 0; char *hdr_end = 0;
  *bodylen = 0;
  if (cap < 64) return 0;
  fd = tcp_connect(host, port);
  if (fd < 0) return 0;
  { /* a read timeout makes a stuck/keep-alive service harmless on the car */
    struct { long s; long u; } tv; tv.s = 4; tv.u = 0;
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
    if (hdr_end){
      usize bodyoff = (usize)(hdr_end - out);
      usize n = got - bodyoff;
      usize i; for (i = 0; i < n; i++) out[i] = out[bodyoff + i];
      out[n] = 0; *bodylen = n;
    } else *bodylen = got;
    return status; }
}

/* ---- WebSocket client (RFC6455, server->client frames only) -------------- */
/* Fixed 16-byte nonce in base64 (the value is irrelevant to the server). */
static const char *WS_KEY = "dGhlIHNhbXBsZSBub25jZQ==";
static int ws_open(const char *host, int port, const char *path){
  int fd = tcp_connect(host, port);
  char req[640]; usize rl = 0; char resp[512]; usize got = 0;
  if (fd < 0) return -1;
  { const char *parts[12]; int i;
    parts[0]="GET "; parts[1]=path; parts[2]=" HTTP/1.1\r\nHost: "; parts[3]=host;
    parts[4]="\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ";
    parts[5]=WS_KEY;
    parts[6]="\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: rsi\r\n\r\n";
    parts[7]=0;
    for (i = 0; parts[i]; i++){ usize n = slen(parts[i]);
      if (rl + n >= sizeof(req)){ close(fd); return -1; }
      memcpy_(req + rl, parts[i], n); rl += n; } }
  if (!wfull(fd, req, rl)){ close(fd); return -1; }
  /* read the 101 response headers */
  for (;;){
    long r = read(fd, resp + got, sizeof(resp) - 1 - got);
    if (r <= 0){ close(fd); return -1; }
    got += (usize)r; resp[got] = 0;
    { usize i; for (i = 0; i + 3 < got; i++)
        if (resp[i]=='\r'&&resp[i+1]=='\n'&&resp[i+2]=='\r'&&resp[i+3]=='\n') goto handshake_done; }
    if (got >= sizeof(resp) - 2){ close(fd); return -1; }
  }
handshake_done:
  if (my_atoi(resp + 9) != 101){
    char msg[128]; int k = 0; const char *s = "ws handshake status ";
    while (*s) msg[k++] = *s++;
    { char t[8]; int j = 0; int v = my_atoi(resp + 9);
      if (!v) t[j++] = '0'; while (v){ t[j++] = (char)('0' + v % 10); v /= 10; }
      while (j) msg[k++] = t[--j]; }
    msg[k] = 0; L("ws", msg);
    close(fd); return -1;
  }
  { /* a 5 s read timeout keeps the loop responsive to signals/heartbeats */
    struct { long s; long u; } tv; tv.s = 5; tv.u = 0;
    setsockopt(fd, 1 /*SOL_SOCKET*/, 20 /*SO_RCVTIMEO*/, &tv, (unsigned int)sizeof(tv)); }
  return fd;
}
static int rd_full(int fd, void *buf, usize n){
  usize o = 0;
  while (o < n){ long r = read(fd, (char*)buf + o, n - o); if (r <= 0) return 0; o += (usize)r; }
  return 1;
}
static int ws_send(int fd, int opcode, const void *payload, usize n){
  unsigned char h[8]; int hl;
  h[0] = (unsigned char)(0x80 | (opcode & 0x0f));          /* FIN + opcode */
  if (n < 126){ h[1] = (unsigned char)(0x80 | n); hl = 2; }
  else { h[1] = 0x80 | 126; h[2] = (unsigned char)((n >> 8) & 0xff); h[3] = (unsigned char)(n & 0xff); hl = 4; }
  { unsigned char mask[4]; int i; for (i = 0; i < 4; i++) mask[i] = (unsigned char)(0x5a + i);
    h[hl] = mask[0]; h[hl+1] = mask[1]; h[hl+2] = mask[2]; h[hl+3] = mask[3]; }
  if (!wfull(fd, h, (usize)(hl + 4))) return 0;
  /* client->server frames MUST be masked; write the masked payload in chunks */
  { unsigned char tmp[256]; usize off = 0;
    while (off < n){
      usize k = n - off; usize i;
      if (k > sizeof(tmp)) k = sizeof(tmp);
      for (i = 0; i < k; i++) tmp[i] = (unsigned char)(*((const unsigned char*)payload + off + i) ^ (unsigned char)(0x5a + (int)((off + i) & 3)));
      if (!wfull(fd, tmp, k)) return 0;
      off += k; } }
  return 1;
}
/* Read one message (assembling continuations).  Returns the payload length
 * (>=0), -1 on close/error.  Text/continuation payload is NUL-terminated. */
static long ws_read(int fd, char *out, usize cap){
  usize total = 0; int first = 1;
  for (;;){
    unsigned char h[2]; long plen; unsigned char opcode; int fin, masked;
    if (!rd_full(fd, h, 2)) return -1;
    fin = (h[0] >> 7) & 1; opcode = (unsigned char)(h[0] & 0x0f); masked = (h[1] >> 7) & 1;
    plen = h[1] & 0x7f;
    if (plen == 126){ unsigned char b[2]; if (!rd_full(fd, b, 2)) return -1; plen = ((long)b[0] << 8) | b[1]; }
    else if (plen == 127){ unsigned char b[8]; int i; plen = 0; if (!rd_full(fd, b, 8)) return -1;
      for (i = 0; i < 8; i++) plen = (plen << 8) | b[i]; }
    { unsigned char mk[4]; if (masked && !rd_full(fd, mk, 4)) return -1;
      if (masked){ /* server frames should not be masked; unmask defensively */
        long i;
        /* read into out then unmask */
        if (first && plen > 0 && (usize)plen <= cap - 1){ if (!rd_full(fd, out, (usize)plen)) return -1;
          for (i = 0; i < plen; i++) out[i] = (char)((unsigned char)out[i] ^ mk[i & 3]);
          total = (usize)plen; }
        else { unsigned char sink[256]; long left = plen; while (left > 0){ usize k = (usize)(left > (long)sizeof(sink) ? (long)sizeof(sink) : left);
            if (!rd_full(fd, sink, k)) return -1; left -= (long)k; } }
      }
      else if (plen > 0){
        if (total + (usize)plen < cap){
          if (!rd_full(fd, out + total, (usize)plen)) return -1;
          total += (usize)plen;
        } else { /* too big: drain */
          unsigned char sink[512]; long left = plen;
          while (left > 0){ usize k = (usize)(left > (long)sizeof(sink) ? (long)sizeof(sink) : left);
            if (!rd_full(fd, sink, k)) return -1; left -= (long)k; }
        }
      }
    }
    first = 0;
    if (opcode == 8){ return -1; }                                  /* close */
    if (opcode == 9){ ws_send(fd, 10, out, 0); continue; }           /* ping -> pong */
    if (opcode == 10) continue;                                      /* pong */
    if (fin){ out[total < cap ? total : cap - 1] = 0; return (long)total; }
  }
}

/* ---- JSON scanning (pragmatic: we only need a few scalars) --------------
 * Two rules learned on the bench:
 *  (1) a scan must ALWAYS START at a proper token boundary (the beginning of the
 *      text, or the start of an object).  Resuming mid-value flips the quote
 *      parity and the scanner then reads punctuation as keys -- it silently found
 *      only the FIRST renderer of a two-renderer array.
 *  (2) so the multi-object walk below enumerates the array's objects by brace
 *      depth and scans each object from ITS OWN START. */
static const char *find_key(const char *j, const char *key){
  usize kl = slen(key);
  while (*j){
    if (*j == '"'){
      const char *k = j + 1; usize n = 0;
      while (k[n] && k[n] != '"') n++;
      if (n == kl){
        usize i; int same = 1;
        for (i = 0; i < kl; i++) if (k[i] != key[i]){ same = 0; break; }
        if (same){
          const char *v = k + n + 1;
          while (*v == ' ' || *v == '\t') v++;
          if (*v == ':'){ v++; while (*v == ' ' || *v == '\t') v++; return v; }
        }
      }
      j = k + n + 1;
      continue;
    }
    j++;
  }
  return 0;
}
/* Find the idx-th '{...}' object inside the JSON array that starts at or after
 * `p` (p must be a proper boundary).  Returns its start, sets *len to its
 * length (including braces), or 0 when there is no idx-th object. */
static const char *json_array_object(const char *p, int idx, usize *len){
  int depth = 0, instr = 0, seen = 0; const char *start = 0;
  *len = 0;
  if (!p) return 0;
  while (*p && *p != '[') p++;
  if (*p != '[') return 0;
  p++;
  for (; *p; p++){
    char c = *p;
    if (instr){
      if (c == '\\' && p[1]){ p++; continue; }
      if (c == '"') instr = 0;
      continue;
    }
    if (c == '"'){ instr = 1; continue; }
    if (c == '{'){ if (depth == 0) start = p; depth++; continue; }
    if (c == '}'){
      if (depth > 0) depth--;
      if (depth == 0 && start){
        if (seen == idx){ *len = (usize)(p - start) + 1; return start; }
        seen++; start = 0;
      }
      continue;
    }
    if (c == ']' && depth == 0) break;
  }
  return 0;
}
static int get_int(const char *j, const char *key, s64 *out){
  const char *v = find_key(j, key); long n = 0; int neg = 0, got = 0;
  if (!v) return 0;
  if (*v == '-'){ neg = 1; v++; }
  while (*v >= '0' && *v <= '9'){ n = n * 10 + (*v - '0'); v++; got = 1; }
  if (!got) return 0;
  *out = neg ? -n : n; return 1;
}
static int get_str(const char *j, const char *key, char *out, usize cap){
  const char *v = find_key(j, key); usize n = 0;
  if (!v || *v != '"') return 0;
  v++;
  while (v[n] && v[n] != '"' && n < cap - 1){ out[n] = v[n]; n++; }
  out[n] = 0; return 1;
}
/* distancePerPixel is a float; parse to milli-units to stay integer-only. */
static int get_float_milli(const char *j, const char *key, s64 *out){
  const char *v = find_key(j, key); s64 ip = 0, frac = 0, scale = 1; int neg = 0, got = 0;
  if (!v) return 0;
  if (*v == '-'){ neg = 1; v++; }
  while (*v >= '0' && *v <= '9'){ ip = ip * 10 + (*v - '0'); v++; got = 1; }
  if (*v == '.'){ v++; while (*v >= '0' && *v <= '9' && scale < 1000){ frac = frac * 10 + (*v - '0'); scale *= 10; v++; got = 1; } }
  if (!got) return 0;
  while (scale < 1000){ frac *= 10; scale *= 10; }
  *out = (ip * 1000 + frac) * (neg ? -1 : 1);
  return 1;
}

/* ---- the shim inbox (step emitter) ------------------------------------- */
static int g_sock = -2;              /* -2 = not tried, -1 = disabled/closed */
static const char *g_sockpath = "/dev/shm/mib3zoom.sock";
static void emit_close(void){ if (g_sock >= 0){ close(g_sock); } g_sock = -2; }
/* sockaddr_un family layout differs: Linux has a 16-bit sun_family at 0, BSD
 * (macOS host build) has sun_len then sun_family.  The unit path is the Linux
 * one; the host variant only exists so the desk can exercise the tool. */
#ifdef HOST_TEST
typedef struct { unsigned char len; unsigned char fam; char p[104]; } sun_t;
#else
typedef struct { unsigned short fam; char p[108]; } sun_t;
#endif
static int sun_fill(sun_t *sa, const char *path){
  usize n = 0;
  memset_(sa, 0, sizeof(*sa));
#ifdef HOST_TEST
  sa->fam = 1;
  while (path[n] && n < sizeof(sa->p) - 1){ sa->p[n] = path[n]; n++; }
  sa->len = (unsigned char)(n + 2);
#else
  sa->fam = 1;
  while (path[n] && n < sizeof(sa->p) - 1){ sa->p[n] = path[n]; n++; }
#endif
  return 0;
}
static int emit(int dir_in){
  sun_t sa;
  int tries;
  if (!g_sockpath || !g_sockpath[0]) return 0;
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
      const char *m = dir_in ? "+1\n" : "-1\n";
      if (wfull(g_sock, m, 3)){ return 1; }
      emit_close();
    }
  }
  return 0;
}

/* ---- step policy: rate limit + last-value coalescing ------------------- */
static s64 g_last_emit_ms = 1;   /* 1, not 0: a zero clock must not look like "just emitted" */
static int g_pending_dir, g_pending_n, g_min_gap_ms = 60;
/* CLOCK_MONOTONIC is 1 on Linux, 6 on Darwin -- passing Linux's id on the host
 * silently returns EINVAL and (with this program's fallback) froze the pacing
 * clock, so nothing ever left the queue. */
#ifdef HOST_TEST
#define CLK_MONOTONIC 6
extern int *__error(void);
#define ERRNO() (*__error())
#else
#define CLK_MONOTONIC 1
extern int *__errno_location(void);
#define ERRNO() (*__errno_location())
#endif
static s64 now_ms(void){
  struct { long s; long ns; } ts;
  ts.s = 0; ts.ns = 0;
  if (clock_gettime(CLK_MONOTONIC, &ts) != 0){
    long t = time(0);              /* last resort: second resolution */
    return (s64)t * 1000;
  }
  return ts.s * 1000 + ts.ns / 1000000;
}
static int flush_pending(void){
  int k, sent = 0;
  if (!g_pending_n) return 0;
  { int d = g_pending_dir, n = g_pending_n;
    g_pending_n = 0;
    for (k = 0; k < n; k++) sent += emit(d > 0); }
  g_last_emit_ms = now_ms();
  return sent;
}
/* Accept a signed step; emit immediately if the pacing gap allows, else keep
 * the pending tail.  A REVERSAL DROPS the unsent tail in the old direction:
 * the user's latest intent wins, and a stale direction is never replayed at the
 * phone. */
static int step(int dir){
  s64 t = now_ms();
  if (!dir) return 0;
  if (g_pending_n && (g_pending_dir != dir)) g_pending_n = 0;   /* drop unsent tail */
  g_pending_dir = dir;
  if (g_pending_n < 8) g_pending_n++;                            /* bounded intent */
  if (t - g_last_emit_ms >= g_min_gap_ms) return flush_pending();
  return 0;
}

/* ---- renderers tap state ---------------------------------------------- */
#define MAXR 8
static struct { char name[48]; char uri[80]; s64 dpp; int have; int auto_mode; } g_r[MAXR];
static int g_nr;

/* The renderer tap's decision, kept pure so the selftest can pin it without a
 * socket.  Returns 1 when the change is a wheel step (*dir: +1 in, -1 out),
 * 0 when it must be ignored:
 *   - the first sample (no baseline);
 *   - a zoomMode EDGE (auto<->manual).  MEASURED 2026-10-19: the first wheel
 *     roll flips the car's kombi renderer auto -> manual AND jumps
 *     distancePerPixel 3.838 -> 26.312 at once.  That jump is manual mode
 *     starting at its own scale, not one step, so adopting it as a baseline is
 *     the only honest reading -- the direction is simply not in the number.
 *   - a change below the 15% threshold (rescale noise);
 *   - any change while zoomMode is auto (that is speed, not the wheel). */
static int dpp_decide(s64 prev, int prev_auto, s64 now, int now_auto, int *dir){
  s64 hi, lo;
  if (dir) *dir = 0;
  if (prev <= 0 || now <= 0) return 0;
  if (prev_auto != now_auto) return 0;
  hi = prev * 115 / 100; lo = prev * 100 / 115;
  if (now <= hi && now >= lo) return 0;
  if (now_auto) return 0;
  if (dir) *dir = (now < prev) ? 1 : -1;      /* smaller m/px = zoomed in */
  return 1;
}

static int renderer_slot(const char *name, const char *uri){
  int i;
  for (i = 0; i < g_nr; i++){
    if (name[0] && streq(g_r[i].name, name)) return i;
    if (uri[0] && streq(g_r[i].uri, uri)) return i;
  }
  if (g_nr >= MAXR) return -1;
  memset_(&g_r[g_nr], 0, sizeof(g_r[0]));
  { usize n = 0; while (name[n] && n < sizeof(g_r[g_nr].name) - 1){ g_r[g_nr].name[n] = name[n]; n++; } }
  { usize n = 0; while (uri[n] && n < sizeof(g_r[g_nr].uri) - 1){ g_r[g_nr].uri[n] = uri[n]; n++; } }
  return g_nr++;
}

/* ---- selftest: the parsers over fixtures that mirror the shipped spec --- */
static int g_fail;
static void ck(const char *name, int got, int exp){
  P(2, "WHEEL selftest exp="); PN(2, exp); P(2, " got="); PN(2, got);
  P(2, got == exp ? " PASS\n" : " FAIL\n");
  if (got != exp) g_fail++;
}
static int selftest(void){
  s64 v = 0;
  /* navsd's POST as an RSI element event (viwi.service.maprenderer spec:
   * discreteInputs properties currentValue/relativeSteps/decrementContinuously) */
  const char *ev1 = "{\"resource\":\"maprenderer.discreteInputs\",\"element\":{"
                    "\"id\":\"2c095e2f-9ba3-4d82-9b4c-b94d8b72fc97\",\"name\":\"kombi\","
                    "\"uri\":\"/maprenderer/discreteInputs/2c095e2f\",\"relativeSteps\":-1}}";
  ck("selftest.discrete_relativeSteps_present", get_int(ev1, "relativeSteps", &v), 1);
  ck("selftest.discrete_relativeSteps_value", (int)v, -1);

  const char *ev2 = "{\"state\":\"changed\",\"properties\":[\"currentValue\"],\"relativeSteps\":2}";
  ck("selftest.discrete_relativeSteps_2", get_int(ev2, "relativeSteps", &v) ? (int)v : 99, 2);

  /* the renderers element (from the live ILM dump: name mainunit/kombi,
   * zoomMode auto, distancePerPixel 3.83…, discreteInput {…}) */
  const char *ev3 = "{\"element\":{\"name\":\"kombi\",\"zoomMode\":\"auto\","
                    "\"distancePerPixel\":3.838582677165354,\"zoomAreaWidth\":298,"
                    "\"discreteInput\":{\"name\":\"kombi\",\"uri\":\"/maprenderer/discreteInputs/x\"}}}";
  { char nm[32]; s64 mm = 0;
    ck("selftest.renderer_name", get_str(ev3, "name", nm, sizeof(nm)) && streq(nm, "kombi"), 1);
    ck("selftest.renderer_dpp_parsed", get_float_milli(ev3, "distancePerPixel", &mm), 1);
    ck("selftest.renderer_dpp_milli", (int)(mm / 1000), 3);
    ck("selftest.renderer_zoomArea", get_int(ev3, "zoomAreaWidth", &v) ? (int)v : 0, 298); }

  /* no key -> must not invent one */
  ck("selftest.absent_key", get_int("{\"a\":1}", "relativeSteps", &v), 0);

  /* integer extraction must not stop at a digit inside a string */
  ck("selftest.string_digit_not_a_number",
     get_int("{\"name\":\"2c095e2f-9ba3\",\"relativeSteps\":1}", "relativeSteps", &v) ? (int)v : 0, 1);

  /* float variant seen in the wild (0.5 = zoomed in a long way) */
  { s64 mm = 0; ck("selftest.dpp_fraction", get_float_milli("{\"distancePerPixel\":0.5}", "distancePerPixel", &mm) ? (int)mm : 0, 500); }

  /* TWO objects in one array: the SECOND must be found.  The scanner must
   * never resume inside a value string (quote-parity bug: it used to print
   * only the first renderer of a two-renderer array). */
  { const char *two = "{\"data\":[{\"name\":\"mainunit\",\"uri\":\"/x\"},"
                      "{\"name\":\"kombi\",\"uri\":\"/y\"}]}";
    const char *arr = find_key(two, "data");
    const char *o0 = json_array_object(arr, 0, (usize[]){0});
    usize l1 = 0;
    const char *o1 = json_array_object(arr, 1, &l1);
    char nm[32] = "";
    ck("selftest.obj0", (o0 && get_str(o0, "name", nm, sizeof(nm)) && streq(nm, "mainunit")) ? 1 : 0, 1);
    ck("selftest.obj1", (o1 && get_str(o1, "name", nm, sizeof(nm)) && streq(nm, "kombi")) ? 1 : 0, 1);
    ck("selftest.obj1_len", (int)l1, 27); }

  /* the poll-path decision (added 2026-10-19): the car's kombi renderer flipped
   * auto -> manual with a 6.9x jump when the wheel was first rolled, so the EDGE
   * must be adopted silently, and only manual-mode deltas are steps. */
  { int d = 0;
    ck("selftest.decide_edge_auto_to_manual", dpp_decide(3838, 1, 26312, 0, &d), 0);
    ck("selftest.decide_edge_no_dir", d, 0);
    ck("selftest.decide_manual_out", dpp_decide(26312, 0, 40000, 0, &d) ? d : 0, -1);
    ck("selftest.decide_manual_in", dpp_decide(26312, 0, 12000, 0, &d) ? d : 0, 1);
    ck("selftest.decide_below_threshold", dpp_decide(26312, 0, 27000, 0, &d), 0);
    ck("selftest.decide_auto_ignored", dpp_decide(3000, 1, 6000, 1, &d), 0); }

  if (!g_fail) P(2, "TOTAL: PASS=19 FAIL=0\n");
  return g_fail ? 1 : 0;
}

/* One renderer element -> at most one zoom step.  Used by BOTH the WS event
 * path and the poll path (they can race harmlessly: whoever sees the new
 * distancePerPixel first emits and moves the baseline, the other sees no
 * change). */
static int tapb_element(const char *obj, int invert, int *accepted, int *steps){
  char nm[48] = ""; char zom[24] = "";
  s64 dpp = 0; int slot, auto_mode, emit_dir = 0, s;
  if (!obj || !get_str(obj, "name", nm, sizeof(nm))) return 0;
  slot = renderer_slot(nm, "");
  if (slot < 0) return 0;
  get_str(obj, "zoomMode", zom, sizeof(zom));
  auto_mode = streq(zom, "auto");
  if (!get_float_milli(obj, "distancePerPixel", &dpp)) return 0;
  if (!g_r[slot].have){
    g_r[slot].dpp = dpp; g_r[slot].auto_mode = auto_mode; g_r[slot].have = 1;
    return 0;
  }
  if (g_r[slot].auto_mode != auto_mode){
    P(2, "[wheel] tapB zoomMode edge ");
    P(2, g_r[slot].auto_mode ? "auto->manual" : "manual->auto");
    P(2, " renderer="); P(2, g_r[slot].name);
    LV(" baseline_milli", dpp);
    g_r[slot].dpp = dpp; g_r[slot].auto_mode = auto_mode;
    return 0;
  }
  if (dpp_decide(g_r[slot].dpp, g_r[slot].auto_mode, dpp, auto_mode, &emit_dir)){
    int dir = emit_dir;
    if (invert) dir = -dir;
    P(2, "[wheel] tapB renderer="); P(2, g_r[slot].name); LV(" dpp_milli", dpp);
    if (accepted) (*accepted)++;
    g_r[slot].dpp = dpp;
    s = step(dir);
    if (steps) *steps += s;
    return 1;
  }
  if (auto_mode){
    s64 hi = g_r[slot].dpp * 115 / 100, lo = g_r[slot].dpp * 100 / 115;
    if (dpp > hi || dpp < lo) L("tapB", "dpp moved under zoomMode=auto -> ignored (speed zoom, not the wheel)");
  }
  g_r[slot].dpp = dpp;
  return 0;
}

/* ---- renderer POLL (added after measuring the car) ----------------------
 * RSI answers GET for /maprenderer/renderers but does NOT push these changes over
 * the WS subscription: a wheel step arrived, the car's own `kombi` renderer
 * went distancePerPixel 3.838 -> 26.312 and zoomMode auto -> manual, and mib3wheel
 * logged NOTHING.  A GET diff is the reliable instrument; the WS taps stay because
 * they cost nothing and may push on another build. */
static int poll_renderers(const char *host, int port, int invert, int *accepted, int *steps){
  static char buf[65536]; usize bl = 0; const char *arr; int k, handled = 0;
  if (http_get(host, port, "/maprenderer/renderers", buf, sizeof(buf), &bl) != 200 || !bl) return 0;
  arr = find_key(buf, "data");
  for (k = 0; k < MAXR; k++){
    usize ol = 0; const char *o = json_array_object(arr, k, &ol);
    if (!o) break;
    handled += tapb_element(o, invert, accepted, steps);
  }
  return handled;
}

/* ---- probe ------------------------------------------------------------- */
static void probe_one(const char *host, int port, const char *svc){
  static char buf[65536]; usize bl = 0;
  char path[96]; usize n = 0; const char *s = "/";
  while (*s && n < sizeof(path) - 2) path[n++] = *s++;
  s = svc; while (*s && n < sizeof(path) - 1) path[n++] = *s++;
  path[n] = 0;
  { int st = http_get(host, port, path, buf, sizeof(buf), &bl);
    P(1, "PROBE "); PN(1, port); P(1, " "); P(1, svc); P(1, " status="); PN(1, st);
    P(1, " bytes="); PN(1, (s64)bl); P(1, "\n"); }
}
static int do_probe(const char *host, int port, int all){
  static char buf[262144]; usize bl = 0; int st;
  st = http_get(host, port, "/maprenderer/renderers", buf, sizeof(buf), &bl);
  P(1, "PROBE /maprenderer/renderers status="); PN(1, st); P(1, " bytes="); PN(1, (s64)bl); P(1, "\n");
  if (st == 200 && bl){
    /* Summary lines first (what the operator needs to read), then the raw body
     * (what we need to keep).  Each renderer object carries name, zoomMode,
     * distancePerPixel, distanceUnit, zoomArea fields and discreteInput.
     * Objects are enumerated by brace depth, so the scanner never resumes in
     * the middle of a value (that bug printed only the first renderer). */
    const char *arr = find_key(buf, "data");
    const char *obj; usize olen = 0; int k = 0, seen = 0;
    while ((obj = json_array_object(arr, k, &olen)) != 0){
      char nm[48] = ""; char zom[24] = ""; char du[24] = ""; s64 dpp = 0;
      if (get_str(obj, "name", nm, sizeof(nm)) && nm[0]){
        get_float_milli(obj, "distancePerPixel", &dpp);
        get_str(obj, "zoomMode", zom, sizeof(zom));
        get_str(obj, "distanceUnit", du, sizeof(du));
        P(1, "RENDERER name="); P(1, nm);
        P(1, " zoomMode="); P(1, zom[0] ? zom : "?");
        P(1, " distancePerPixel_milli="); PN(1, dpp);
        P(1, " unit="); P(1, du[0] ? du : "?");
        P(1, "\n");
        seen++;
      }
      k++;
      if (k > MAXR * 2) break;
    }
    P(1, "RENDERER count="); PN(1, seen); P(1, "\n");
    P(1, "RAW /maprenderer/renderers:\n"); write(1, buf, bl); P(1, "\n");
  }
  st = http_get(host, port, "/maprenderer/discreteInputs", buf, sizeof(buf), &bl);
  P(1, "PROBE /maprenderer/discreteInputs status="); PN(1, st); P(1, " bytes="); PN(1, (s64)bl); P(1, "\n");
  if (st == 200 && bl){ P(1, "RAW /maprenderer/discreteInputs:\n"); write(1, buf, bl); P(1, "\n"); }
  if (all){
    int ports[12]; int np = 0, i;
    const char *svcs[6]; int ns = 0;
    for (i = 49500; i <= 49510; i++) ports[np++] = i;
    ports[np++] = 49600;
    svcs[ns++] = "maprenderer"; svcs[ns++] = "input"; svcs[ns++] = "navsystem";
    svcs[ns++] = "navdata"; svcs[ns++] = "displaymanagement"; svcs[ns++] = "serviceregistry";
    { int p2, s2; for (p2 = 0; p2 < np; p2++) for (s2 = 0; s2 < ns; s2++) probe_one(host, ports[p2], svcs[s2]); }
  }
  return 0;
}

/* ---- poll() over the subscriptions -------------------------------------
 * A blocking read on ONE subscription starves the others (the first version
 * read them in sequence: the tool saw at most one event per 5 s timeout).
 * poll() is used rather than select(): its struct layout is identical on the
 * unit's Linux and on the desk's macOS, whereas fd_set's is not. */
struct wpoll { int fd; short events; short revents; };
extern int poll(void *, unsigned long, int);

/* ---- the watcher ------------------------------------------------------- */
struct tag { const char *path; int kind; int fd; s64 last_try_ms; };   /* kind 0 = discreteInputs, 1 = renderers */
#define WATCH_MAX 4

static int main_watch(const char *host, int port, int use_renderers, int dump, int invert, int once, int poll_ms)
{
  struct tag w[WATCH_MAX]; int nw = 0, i, steps = 0, accepted = 0;
  s64 next_poll = 0;
  static char payload[65536];
  w[nw].path = "/maprenderer/discreteInputs"; w[nw].kind = 0; w[nw].fd = -1; w[nw].last_try_ms = 0; nw++;
  if (use_renderers){ w[nw].path = "/maprenderer/renderers"; w[nw].kind = 1; w[nw].fd = -1; w[nw].last_try_ms = 0; nw++; }
  for (i = 0; i < nw; i++){
    w[i].fd = ws_open(host, port, w[i].path);
    w[i].last_try_ms = now_ms();
    if (w[i].fd < 0) L("ws", w[i].path);
    else { P(2, "[wheel] ws: "); P(2, w[i].path); P(2, " open\n"); }
  }
  { int any = 0; for (i = 0; i < nw; i++) if (w[i].fd >= 0) any = 1;
    if (!any){ L("fatal", "no RSI subscription could be opened"); return 2; } }
  for (;;){
    struct wpoll pf[WATCH_MAX]; int npf = 0, nready = 0, j;
    for (i = 0; i < nw; i++){
      if (w[i].fd < 0){
        /* reconnect, but never faster than every 2 s */
        s64 t = now_ms();
        if (t - w[i].last_try_ms >= 2000){
          w[i].last_try_ms = t;
          w[i].fd = ws_open(host, port, w[i].path);
          if (w[i].fd >= 0){ P(2, "[wheel] ws: reconnected "); P(2, w[i].path); P(2, "\n"); }
        }
        continue;
      }
      pf[npf].fd = w[i].fd; pf[npf].events = 1 /*POLLIN*/; pf[npf].revents = 0;
      npf++;
    }
    if (npf) nready = poll(pf, (unsigned long)npf, 50 /* ms */);
    else { usleep(50000); nready = 0; }
    for (i = 0; i < nw && nready > 0; i++){
      long n; int ready = 0;
      if (w[i].fd < 0) continue;
      for (j = 0; j < npf; j++) if (pf[j].fd == w[i].fd && (pf[j].revents & (1 | 8 | 16))) ready = 1;
      if (!ready) continue;
      n = ws_read(w[i].fd, payload, sizeof(payload));
      if (n < 0){ close(w[i].fd); w[i].fd = -1; w[i].last_try_ms = now_ms(); L("ws", "closed -- will reconnect"); continue; }
      if (!n) continue;
      if (dump){ P(1, "EV "); P(1, w[i].path); P(1, " "); write(1, payload, (usize)n); P(1, "\n"); }
      if (w[i].kind == 0){
        s64 rs = 0;
        if (get_int(payload, "relativeSteps", &rs) && rs != 0){
          int dir = rs > 0 ? 1 : -1;
          if (invert) dir = -dir;
          LV("tapA.relativeSteps", rs);
          accepted++;
          steps += step(dir);
          if (once && accepted >= once) goto done;
        }
      } else {
        if (tapb_element(payload, invert, &accepted, &steps)){
          if (once && accepted >= once) goto done;
        }
      }
    }
    { /* poll the renderers: RSI does not push these changes (measured on the car
       * 2026-10-19 -- the wheel turned, the renderer changed, nothing arrived). */
      s64 t = now_ms();
      if (use_renderers && poll_ms > 0 && t >= next_poll){
        next_poll = t + poll_ms;
        if (poll_renderers(host, port, invert, &accepted, &steps)){
          if (once && accepted >= once) goto done;
        }
      }
    }
    { /* pacing: flush a pending tail once the gap has elapsed */
      s64 t = now_ms();
      if (g_pending_n && (t - g_last_emit_ms) >= g_min_gap_ms){ int s = flush_pending(); if (s) steps += s; }
    }
  }
done:
  P(2, "[wheel] steps_emitted="); PN(2, steps); P(2, " steps_accepted="); PN(2, accepted); P(2, "\n");
  return 0;
}

/* ---- entry ------------------------------------------------------------- */
int tool_main(int argc, char **argv);
#ifdef HOST_TEST
/* Desk build: the same source compiled for the host so the parsers, the WS
 * client and the emit policy can be exercised without an aarch64 run.  The
 * container bench still runs the REAL aarch64 binary (see run_wheel.sh). */
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
  /* If we were started through the unit's ld-2.23.so ("ld --library-path X prog"),
   * argv[0] is the loader: skip its options so nav[0] is ALWAYS the program.
   * (rgba_feed.c gets this wrong and needs a dummy first argument; do not copy.) */
  { const char *base = av[0]; const char *p;
    for (p = av[0]; *p; p++) if (*p == '/') base = p + 1;
    if (base[0] == 'l' && base[1] == 'd' && (base[2] == '-' || base[2] == '.')){
      int j = 1;
      while (j < argc){
        const char *t = av[j];
        if (streq(t, "--library-path") || streq(t, "--preload") || streq(t, "--audit") ||
            streq(t, "--argv0") || streq(t, "--inhibit-cache")){ j += 2; continue; }
        if (t[0] == '-'){ j += 1; continue; }
        break;
      }
      skip = j;
    } }
  for (k = 0; skip + k < argc && k < 63; k++) nav[k] = av[skip + k];
  nav[k] = 0;
  _exit(tool_main(k, nav));
}
#endif /* HOST_TEST */

int tool_main(int argc, char **argv){
  const char *host = "127.0.0.1";
  int port = 49500, probe = 0, probe_all = 0, dump = 0, invert = 0, once = 0, use_renderers = 1;
  int poll_ms = 250;
  int i, selftest_mode = 0;
  for (i = 1; i < argc; i++){
    const char *a = argv[i];
    if (streq(a, "--probe")) probe = 1;
    else if (streq(a, "--probe-all")){ probe = 1; probe_all = 1; }
    else if (streq(a, "--dump")) dump = 1;
    else if (streq(a, "--invert")) invert = 1;
    else if (streq(a, "--no-renderers")) use_renderers = 0;
    else if (streq(a, "--no-poll")) poll_ms = 0;
    else if (streq(a, "--poll-ms") && i + 1 < argc) poll_ms = my_atoi(argv[++i]);
    else if (streq(a, "--selftest")) selftest_mode = 1;
    else if (streq(a, "--quiet")) g_quiet = 1;
    else if (streq(a, "--host") && i + 1 < argc) host = argv[++i];
    else if (streq(a, "--port") && i + 1 < argc) port = my_atoi(argv[++i]);
    else if (streq(a, "--socket") && i + 1 < argc) g_sockpath = argv[++i];
    else if (streq(a, "--min-gap-ms") && i + 1 < argc) g_min_gap_ms = my_atoi(argv[++i]);
    else if (streq(a, "--once") && i + 1 < argc) once = my_atoi(argv[++i]);
    else if (streq(a, "--help") || streq(a, "-h")){
      P(1, "mib3wheel [--probe|--probe-all] [--host H] [--port N] [--socket PATH]\n"
           "          [--dump] [--invert] [--min-gap-ms N] [--no-renderers] [--once N]\n"
           "          [--poll-ms N | --no-poll] [--selftest] [--quiet]\n"
           "watches the unit's maprenderer RSI for steering-wheel zoom steps and\n"
           "forwards them to the shim's inbox so CarPlay's cluster map zooms.\n"
           "The renderer state is POLLED (default 250 ms) because RSI does not push it.\n");
      return 0; }
    else { P(2, "[wheel] unknown arg: "); P(2, a); P(2, "\n"); return 2; }
  }
  if (selftest_mode) return selftest();
  P(2, "[wheel] mib3wheel 1.0 rsi="); P(2, host); P(2, ":"); PN(2, port);
  P(2, " socket="); P(2, g_sockpath ? g_sockpath : "(none)"); P(2, "\n");
  if (probe) return do_probe(host, port, probe_all);
  return main_watch(host, port, use_renderers, dump, invert, once, poll_ms);
}
