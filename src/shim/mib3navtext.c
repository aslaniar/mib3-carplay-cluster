/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ============================================================================
 * mib3navtext.c -- THE CLUSTER NAV-TEXT GATE (the two bands on the cluster).
 *
 * WHY THIS EXISTS
 * ---------------
 * The instrument cluster is not a television.  On this car it is a small
 * computer with its own navigation frame: it draws its own address/street line
 * across the top and its own distance + ARRIVAL TIME line across the bottom,
 * and it composites the H.264 map window we send inside that frame.  So with
 * Path B up, you see the phone's Apple Maps AND the car's own two bands around
 * it.  No scene URL, safe area, pixel crop or `showETA=` value can remove them:
 * they are painted after our pixels arrive, from TEXT the head unit keeps
 * sending.
 *
 * The chain, all on the head unit:
 *
 *   carplay ──/tmp/Simple_Socket──► simplenav ──RSI "simplenavigation"──►
 *        navsd ──BAP NavigationSd──► carifservice ──► CLUSTER ECU (draws the
 *                                                    street line, the distance
 *                                                    and the arrival time)
 *
 * This file cuts the FIRST link: the socket from carplay to simplenav.  If
 * simplenav is never told about the phone's route, navsd has nothing to publish
 * and the ECU has nothing to print -- while the picture, the socket and
 * everything else on it are untouched.
 *
 * THE PROTOCOL (read from the shipped binaries)
 * --------------------------------------------------------
 * simplenav reads one record per recv() into a 1000-byte struct
 * (`Simple_Socket_Server::Simple_Socket` -> recv(fd, buf, 1000) ->
 * `Receive_Check` -> memcpy 1000 -> `SimpleIPCService::Receive_Parsing`), and
 * Receive_Parsing switches on:
 *
 *     offset 0 : u32 EVENT ID
 *     offset 4 : u32 TECHNOLOGY (0 = androidAuto, 1 = appleCarPlay)
 *     offset 8 : the payload struct
 *
 *     The record's u32@0 is authored by the PHONE and relayed verbatim, so the
 *     gate reads it exactly the way simplenav does (the receiving contract).
 *     Do not infer the layout from carplay's side: its EAP data callback reads a
 *     u16 from its OWN per-connection wrapper struct to choose one of five
 *     destination sockets -- a different object that happens to hold the same
 *     event code.
 *
 *     event 100 (0x64) -> TurnbyTurnCP / TurnbyTurnAA     "Turn by Turn Event"
 *     event 101 (0x65) -> ManeuverUpdateCP / ManeuverUpdateAA
 *     event 102 (0x66) -> Guidance_Route_Check, androidAuto ONLY: the CarPlay
 *                         branch (tech==1) logs and returns without dispatching
 *                         (0x18938..0x18968), so on this build 102 is inert for
 *                         an iPhone.  Dropping it anyway costs nothing and covers
 *                         a build where it is not inert.
 *
 *     (Receive_Parsing @ 0x18860 in simplenav; handlers 0x181d8 / 0x17f78 /
 *      0x17ec8 / 0x17e18 / 0x185f0 are all exported in .dynsym, so this mapping
 *      is read from the binary, not guessed.)
 *
 * carplay sends exactly one whole record per send() (its guidance paths call
 * send(fd, pkt, len, 0) at 0x25f26c and 0x25f564), so a record can be judged and
 * dropped without reassembly.  Anything that does not look like a record -- a
 * short buffer, an unknown event id, an impossible technology -- is FORWARDED.
 * The gate is fail-open by construction: the only thing it can ever lose is nav
 * text.
 *
 * MODES (live -- read per packet, so they can be A/B'd between phone plugs with
 * inject/target_env.py, no rebuild and no re-injection):
 *
 *   MIB3_SHIM_NAVTEXT=off       (default) hooks installed, nothing dropped
 *   MIB3_SHIM_NAVTEXT=filter    drop the events listed in ..._DROP
 *   MIB3_SHIM_NAVTEXT=all       drop every RECORD on that socket (the "is this
 *                               channel the source at all?" experiment).  The
 *                               connect-time handshake is not a record and still
 *                               passes: simplenav reads it as a client name.
 *   MIB3_SHIM_NAVTEXT=capture   drop nothing; log the first records (the probe
 *                               that proves the event ids on your car)
 *
 *   MIB3_SHIM_NAVTEXT_DROP=100,101,102  (default) which event ids to drop
 *   MIB3_SHIM_NAVTEXT_PATH=/tmp/Simple_Socket     (tests point this elsewhere)
 *
 * The socket is the REAL one in every mode: the hook never replaces the fd,
 * never closes it and never invents a socketpair, so a defect here cannot break
 * CarPlay's navigation plumbing -- at worst it drops a record the cluster was
 * going to print, and `MIB3_SHIM_NAVTEXT=off` stops it on the next packet.
 *
 * TESTABILITY
 * -----------
 * This file is compiled twice: into mib3shim.so (aarch64, -nostdlib) and, by
 * harness/run_navtext.sh, into a NATIVE host binary with the host's libc.  The
 * host build drives the same code against a real AF_UNIX socket, which is how
 * the classifier, the fd bookkeeping, the live mode changes and the fd-reuse
 * safety are proved on the desk before any car run.
 * ==========================================================================*/

typedef unsigned char      nt_u8;
typedef unsigned short     nt_u16;
typedef unsigned int       nt_u32;
typedef unsigned long      nt_u64;

/* Only the libc we need.  The shim is built -nostdlib (no headers), so these are
 * declared by hand; the desk harness defines MIB3NAVTEXT_HOST and gets the
 * host's real headers instead (harness/test_navtext.c). */
#if defined(MIB3NAVTEXT_HOST)
#define NT_ERRNO errno
#else
extern char *getenv(const char *name);
extern int   getpeername(int fd, void *addr, unsigned int *len);
extern long  write(int fd, const void *buf, unsigned long n);
extern int  *__errno_location(void);
#define NT_ERRNO (*__errno_location())
#endif
#define NT_EINVAL 22

#define NT_AF_UNIX 1

/* AF_UNIX address.  The SHIPPED layout is Linux aarch64: a 2-byte family at
 * offset 0, then the path.  The desk harness builds this file for macOS, whose
 * struct is {u8 sun_len; u8 sun_family; char sun_path[104]} -- so the host branch
 * exists ONLY to let the harness drive a real socket through the same logic.  The
 * field NAMES stay identical so not one line of the gate differs. */
#if defined(MIB3NAVTEXT_HOST) && defined(__APPLE__)
typedef struct { nt_u8 nlen; nt_u8 family; char path[104]; } nt_sockaddr_un;
#else
typedef struct { nt_u16 family; char path[108]; } nt_sockaddr_un;
#endif

/* ==========================================================================
 * host-provided plumbing
 * ========================================================================*/
typedef void (*nt_log_fn)(const char *msg);
typedef int  (*nt_connect_fn)(int, const void *, unsigned int);
typedef long (*nt_send_fn)(int, const void *, unsigned long, int);
typedef int  (*nt_close_fn)(int);

static nt_log_fn     g_log;
static nt_connect_fn g_real_connect;
static nt_send_fn    g_real_send;
static nt_close_fn   g_real_close;
static volatile int  g_ready;

#define NT_MAX_FD 8

/* modes (see the table at the head of this file) */
#define NT_MODE_OFF     0
#define NT_MODE_FILTER  1
#define NT_MODE_ALL     2
#define NT_MODE_CAPTURE 3

int mib3_navtext_mode(void);      /* used by the arming log line */

/* ---- state --------------------------------------------------------------- */
static int  g_nfds;                       /* live simplenav fds */
static int  g_fds[NT_MAX_FD];
static nt_u64 g_fd_base[NT_MAX_FD];       /* record counter when that fd was armed */
static char g_path[112];                  /* resolved target path */
static int  g_path_len;

/* counters (exposed to the harness through mib3_navtext_stats) */
static nt_u64 g_records, g_dropped, g_passed, g_odd;
static nt_u64 g_connects;                 /* connects seen to the target path */

/* capture-mode budget: the first NT_CAP_LOG_MAX records, PLUS the first record
 * of every event id seen (so a session that opens with a dozen handshake records
 * cannot hide the guidance ids we are there to find). */
static unsigned long g_cap_logged;
#define NT_CAP_LOG_MAX 12
static nt_u32 g_evt_seen[4];             /* 128 event ids */

static int nt_evt_first(nt_u32 evt)
{
  nt_u32 bit, idx;
  if (evt >= 128) return 0;
  idx = evt >> 5; bit = 1u << (evt & 31);
  if (g_evt_seen[idx] & bit) return 0;
  g_evt_seen[idx] |= bit;
  return 1;
}

/* drop-set parse cache: re-parsed whenever the raw string changes */
static char g_drop_raw[96];
static nt_u8  g_drop_ids[8];
static int    g_ndrop;
static int    g_drop_all;

/* ---- tiny string helpers (no libc) -------------------------------------- */
static int nt_len(const char *s){ int n = 0; if (!s) return 0; while (s[n]) n++; return n; }
static int nt_eq(const char *a, const char *b)
{
  int i = 0;
  if (!a || !b) return 0;
  for (;;){ if (a[i] != b[i]) return 0; if (!a[i]) return 1; i++; }
}
static void nt_copy(char *dst, const char *src, int cap)
{
  int i = 0;
  if (cap <= 0) return;
  while (src && src[i] && i < cap - 1){ dst[i] = src[i]; i++; }
  dst[i] = 0;
}

/* ---- logging ------------------------------------------------------------- */
static void nt_log(const char *msg)
{
  nt_log_fn f = g_log;
  if (f) f(msg);
  else   { write(2, msg, (unsigned long)nt_len(msg)); write(2, "\n", 1); }
}

typedef struct { char *p; int cap, n; } nt_buf;
static void nb_c(nt_buf *b, char c){ if (b->n < b->cap - 1){ b->p[b->n++] = c; b->p[b->n] = 0; } }
static void nb_s(nt_buf *b, const char *s){ int i = 0; while (s && s[i]) nb_c(b, s[i++]); }
static void nb_u(nt_buf *b, nt_u64 v)
{
  char t[24]; int k = 0;
  if (!v){ nb_c(b, '0'); return; }
  while (v && k < 24){ t[k++] = (char)('0' + (int)(v % 10)); v /= 10; }
  while (k--) nb_c(b, t[k]);
}
static void nb_hex(nt_buf *b, unsigned v, int digits)
{
  const char *hx = "0123456789abcdef";
  char t[24]; int k = 0;
  while (v && k < 24){ t[k++] = hx[v & 0xf]; v >>= 4; }
  while (k < digits) t[k++] = '0';
  while (k--) nb_c(b, t[k]);
}

/* ==========================================================================
 * fd bookkeeping
 * ========================================================================*/
static int nt_fd_index(int fd)
{
  int i;
  for (i = 0; i < g_nfds; i++) if (g_fds[i] == fd) return i;
  return -1;
}

/* Copy an AF_UNIX address into a zero-padded local so a short or misaligned
 * buffer cannot fault.  Returns 1 and fills `out` (if given) when it IS AF_UNIX. */
static int nt_unix_path(const void *sa, unsigned int len, char *out, int cap)
{
  nt_sockaddr_un t;
  unsigned int i;
  const char *s = (const char *)sa;
  char *d = (char *)&t;
  if (!sa || len < 2) return 0;
  if (len > sizeof(t)) len = (unsigned int)sizeof(t);
  for (i = 0; i < sizeof(t); i++) d[i] = (i < len) ? s[i] : 0;
  if (t.family != NT_AF_UNIX) return 0;
  if (out) nt_copy(out, t.path, cap);
  return 1;
}

static int nt_is_target(const void *sa, unsigned int len)
{
  char p[112];
  int k;
  if (g_path_len <= 0) return 0;
  if (!nt_unix_path(sa, len, p, (int)sizeof(p))) return 0;
  /* the path must match EXACTLY: "/tmp/Simple_SocketX" is a different socket */
  for (k = 0; k < g_path_len; k++) if (p[k] != g_path[k]) return 0;
  return p[g_path_len] == 0;
}

static void nt_fd_add(int fd)
{
  char ln[192]; nt_buf b = { ln, (int)sizeof(ln), 0 };
  if (fd < 0 || nt_fd_index(fd) >= 0) return;
  if (g_nfds >= NT_MAX_FD){
    /* Ring, not a hard stop: if a socket is ever closed through a path we do not
     * hook (a library calling the syscall directly), the oldest entry is stale
     * and keeping it would silently stop the gate from ever arming again.  One
     * fd per session is the expected shape; eight without a close is abnormal
     * and the newest is the live one. */
    int k;
    for (k = 0; k + 1 < g_nfds; k++){ g_fds[k] = g_fds[k + 1]; g_fd_base[k] = g_fd_base[k + 1]; }
    g_nfds--;
  }
  g_fd_base[g_nfds] = g_records;
  g_fds[g_nfds++] = fd;
  nb_s(&b, "armed fd="); nb_u(&b, (nt_u64)fd);
  nb_s(&b, " known="); nb_u(&b, (nt_u64)g_nfds);
  nb_s(&b, " mode="); nb_u(&b, (nt_u64)mib3_navtext_mode());
  nb_s(&b, " path="); nb_s(&b, g_path);
  nt_log(ln);
}

static void nt_fd_forget(int fd)
{
  int i = nt_fd_index(fd), k;
  if (i < 0) return;
  /* A socket that carried NO record for its whole life is a finding, not a
   * detail: either guidance never ran, or the records do not leave carplay
   * through send() on this socket (a write()-based path would look exactly like
   * this).  Say it out loud instead of leaving an empty log to be misread. */
  if (mib3_navtext_mode() != NT_MODE_OFF && g_records == g_fd_base[i]){
    char ln[200]; nt_buf b = { ln, (int)sizeof(ln), 0 };
    nb_s(&b, "fd="); nb_u(&b, (nt_u64)fd);
    nb_s(&b, " closed with NO records seen (mode=");
    nb_u(&b, (nt_u64)mib3_navtext_mode());
    nb_s(&b, ") -- guidance never ran, or it does not leave through send()");
    nt_log(ln);
  }
  for (k = i; k + 1 < g_nfds; k++){ g_fds[k] = g_fds[k + 1]; g_fd_base[k] = g_fd_base[k + 1]; }
  g_nfds--;
}

/* ---- which AF_UNIX sockets does carplay actually connect to? --------------
 * The gate aims at ONE path (/tmp/Simple_Socket).  Rather than trust a comment,
 * every distinct unix path carplay connects to is logged while the gate is armed
 * (mode != off) -- so `MIB3_SHIM_NAVTEXT=capture` answers "is the path what we
 * think it is?" on the car, and MIB3_SHIM_NAVTEXT_PATH fixes a different answer
 * WITHOUT a rebuild.  Four entries is plenty: this process is not a shell. */
#define NT_MAX_PATHS 4
static char g_paths_seen[NT_MAX_PATHS][112];
static int  g_npaths_seen;

int mib3_navtext_paths_seen(void){ return g_npaths_seen; }

static void nt_note_path(const char *p)
{
  int i;
  char ln[200]; nt_buf b;
  if (!p || !p[0]) return;
  for (i = 0; i < g_npaths_seen; i++) if (nt_eq(g_paths_seen[i], p)) return;
  if (g_npaths_seen >= NT_MAX_PATHS) return;
  nt_copy(g_paths_seen[g_npaths_seen], p, (int)sizeof(g_paths_seen[0]));
  g_npaths_seen++;
  b.p = ln; b.cap = (int)sizeof(ln); b.n = 0; ln[0] = 0;
  nb_s(&b, "unix connect path="); nb_s(&b, p);
  nb_s(&b, (nt_eq(p, g_path) ? "   <- the gate's target" : ""));
  nt_log(ln);
}

/* A connection to the target may already exist when the seams are installed
 * into a RUNNING carplay (injection happens mid-boot, the phone connects later
 * -- but a re-injection mid-session must still find it).  getpeername asks the
 * kernel, which is the only authority that matters here. */
static void nt_scan_fds(void)
{
  int fd;
  if (g_path_len <= 0) return;
  /* 1024, not 256: a mid-session re-injection happens in a process that has been
   * running for a while, and RLIMIT_NOFILE on this image allows four figures.
   * Missing the socket here would make `capture` log nothing and send the runbook
   * down a false "the records do not use send()" conclusion.  getpeername is ~1 us,
   * so even the whole range costs about a millisecond, once. */
  for (fd = 0; fd < 1024; fd++){
    nt_sockaddr_un u;
    unsigned int len = (unsigned int)sizeof(u);
    int i, k, ok = 1;
    for (i = 0; i < (int)sizeof(u); i++) ((char *)&u)[i] = 0;
    if (getpeername(fd, (void *)&u, &len) != 0) continue;
    if (len < 2 || u.family != NT_AF_UNIX) continue;
    for (k = 0; k < g_path_len; k++) if (u.path[k] != g_path[k]) { ok = 0; break; }
    if (ok && u.path[g_path_len] == 0) nt_fd_add(fd);
  }
}

/* ==========================================================================
 * install -- idempotent.  The shim calls it on EVERY hook entry, so a hook can
 * never run before the real functions exist.
 * ========================================================================*/
void mib3_navtext_install(void (*log)(const char *),
                          int (*c)(int, const void *, unsigned int),
                          long (*s)(int, const void *, unsigned long, int),
                          int (*cl)(int))
{
  int first = 0;
  if (!g_ready){
    const char *p;
    g_log = log;
    g_real_connect = c;
    g_real_send = s;
    g_real_close = cl;
    p = getenv("MIB3_SHIM_NAVTEXT_PATH");
    if (!p || !p[0]) p = "/tmp/Simple_Socket";
    nt_copy(g_path, p, (int)sizeof(g_path));
    g_path_len = nt_len(g_path);
    __sync_synchronize();
    first = 1;
    g_ready = 1;
  } else {
    /* a re-install (two threads raced, or the shim was reloaded): refresh only */
    g_log = log; g_real_connect = c; g_real_send = s; g_real_close = cl;
  }
  if (first) nt_scan_fds();
}

/* ==========================================================================
 * configuration, read LIVE (so target_env.py can change it between plugs)
 * ========================================================================*/
int mib3_navtext_mode(void)
{
  char *v = getenv("MIB3_SHIM_NAVTEXT");
  if (!v || !v[0]) return NT_MODE_OFF;
  if (nt_eq(v, "0") || nt_eq(v, "off") || nt_eq(v, "no") || nt_eq(v, "none")) return NT_MODE_OFF;
  if (nt_eq(v, "capture") || nt_eq(v, "probe")) return NT_MODE_CAPTURE;
  if (nt_eq(v, "all") || nt_eq(v, "block") || nt_eq(v, "every")) return NT_MODE_ALL;
  return NT_MODE_FILTER;                      /* filter | on | 1 | anything else */
}

/* "100,101,102" -> the id list.  Re-parsed only when the string changes. */
static void nt_drop_set(void)
{
  char *raw = getenv("MIB3_SHIM_NAVTEXT_DROP");
  int i;
  if (!raw || !raw[0]) raw = "100,101,102";
  if (nt_eq(raw, g_drop_raw)) return;
  nt_copy(g_drop_raw, raw, (int)sizeof(g_drop_raw));
  g_ndrop = 0; g_drop_all = 0;
  if (nt_eq(raw, "all") || nt_eq(raw, "block")){ g_drop_all = 1; return; }
  for (i = 0; raw[i] && g_ndrop < (int)sizeof(g_drop_ids); ){
    int val = 0, any = 0;
    while (raw[i] >= '0' && raw[i] <= '9'){ val = val * 10 + (raw[i] - '0'); i++; any = 1; }
    if (any && val >= 0 && val < 256) g_drop_ids[g_ndrop++] = (nt_u8)val;
    while (raw[i] && raw[i] != ',') i++;
    if (raw[i] == ',') i++;
  }
}

static int nt_wants_event(nt_u32 evt)
{
  int i;
  nt_drop_set();
  if (g_drop_all) return 1;
  for (i = 0; i < g_ndrop; i++) if ((nt_u32)g_drop_ids[i] == evt) return 1;
  return 0;
}

/* ==========================================================================
 * THE HOOKS -- thin.  Nothing here blocks, allocates, or logs on a non-target.
 * ========================================================================*/

/* connect(): learn the fd.  The real connect ALWAYS runs and its result is
 * returned unchanged -- the gate never changes whether a socket connects. */
int mib3_navtext_connect(int fd, const void *sa, unsigned int len)
{
  int target = 0;
  int rc;
  if (!g_real_connect){ NT_ERRNO = NT_EINVAL; return -1; }
  if (g_ready && g_path_len > 0){
    char p[112];
    if (nt_unix_path(sa, len, p, (int)sizeof(p))){
      /* while armed, remember every distinct unix path: this is how the car run
       * confirms (or corrects) the target without a rebuild */
      if (mib3_navtext_mode() != NT_MODE_OFF) nt_note_path(p);
      target = nt_is_target(sa, len);
      if (target) g_connects++;
    }
  }
  rc = g_real_connect(fd, sa, len);
  if (rc == 0 && target) nt_fd_add(fd);
  return rc;
}

/* send(): judge one record.  Fast path (not our socket) is one array scan. */
long mib3_navtext_send(int fd, const void *buf, unsigned long n, int flags)
{
  if (!g_real_send){ NT_ERRNO = NT_EINVAL; return -1; }
  if (g_nfds > 0 && nt_fd_index(fd) >= 0 && buf && n >= 8){
    const nt_u8 *p = (const nt_u8 *)buf;
    nt_u32 evt  = (nt_u32)p[0] | ((nt_u32)p[1] << 8) | ((nt_u32)p[2] << 16) | ((nt_u32)p[3] << 24);
    nt_u32 tech = (nt_u32)p[4] | ((nt_u32)p[5] << 8) | ((nt_u32)p[6] << 16) | ((nt_u32)p[7] << 24);
    int mode = mib3_navtext_mode();
    if (mode == NT_MODE_OFF) return g_real_send(fd, buf, n, flags);

    g_records++;
    if (tech > 1 || evt > 4095){
      g_odd++;                        /* not a record we understand -> forward */
    } else if (mode == NT_MODE_CAPTURE){
      if (g_cap_logged < NT_CAP_LOG_MAX || nt_evt_first(evt)){
        char ln[220]; nt_buf b = { ln, (int)sizeof(ln), 0 };
        int k;
        g_cap_logged++;
        nb_s(&b, "capture n="); nb_u(&b, (nt_u64)n);
        nb_s(&b, " evt="); nb_u(&b, (nt_u64)evt);
        nb_s(&b, " tech="); nb_u(&b, (nt_u64)tech);
        nb_s(&b, " head=");
        for (k = 0; k < 12 && (unsigned long)k < n; k++){ if (k) nb_c(&b, '.'); nb_hex(&b, p[k], 2); }
        nt_log(ln);
      }
    } else if (mode == NT_MODE_ALL || (mode == NT_MODE_FILTER && nt_wants_event(evt))){
      g_dropped++;
      if (g_dropped <= 8 || (g_dropped % 64) == 0){
        char ln[176]; nt_buf b = { ln, (int)sizeof(ln), 0 };
        nb_s(&b, (mode == NT_MODE_ALL) ? "DROP(all)" : "DROP");
        nb_s(&b, " evt="); nb_u(&b, (nt_u64)evt);
        nb_s(&b, " tech="); nb_u(&b, (nt_u64)tech);
        nb_s(&b, " n="); nb_u(&b, (nt_u64)n);
        nb_s(&b, " dropped="); nb_u(&b, g_dropped);
        nt_log(ln);
      }
      return (long)n;                 /* the sender sees a clean, full write */
    } else {
      g_passed++;
    }
  }
  return g_real_send(fd, buf, n, flags);
}

/* close(): forget the fd, so a later socket that REUSES the number is never
 * mistaken for the nav socket -- the one way this gate could eat other traffic. */
int mib3_navtext_close(int fd)
{
  if (!g_real_close){ NT_ERRNO = NT_EINVAL; return -1; }
  if (g_nfds > 0 && nt_fd_index(fd) >= 0) nt_fd_forget(fd);
  return g_real_close(fd);
}

/* ---- bench/harness surface ---------------------------------------------- */
void mib3_navtext_stats(nt_u64 *records, nt_u64 *dropped, nt_u64 *passed,
                        nt_u64 *odd, nt_u64 *connects, int *nfds)
{
  if (records)  *records  = g_records;
  if (dropped)  *dropped  = g_dropped;
  if (passed)   *passed   = g_passed;
  if (odd)      *odd      = g_odd;
  if (connects) *connects = g_connects;
  if (nfds)     *nfds     = g_nfds;
}
const char *mib3_navtext_path(void){ return g_path; }
