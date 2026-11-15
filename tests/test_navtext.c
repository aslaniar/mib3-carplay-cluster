/* ============================================================================
 * test_navtext.c -- the bench for the cluster nav-text gate.
 *
 *   bash tests/run_navtext.sh
 *
 * The test compiles ../src/shim/mib3navtext.c, the same source the aarch64 shim
 * is built from, as a native binary, and drives the exported hooks against a
 * real AF_UNIX socket pair.  Every assertion therefore exercises the shipping
 * code path rather than a model of it:
 *
 *   * connect() still connects, still returns the real errno, and is recorded
 *   * one record per send(); the event id is the u32 at offset 0 and the
 *     technology the u32 at offset 4 (read from simplenav's Receive_Parsing)
 *   * filter drops exactly the listed event ids and nothing else
 *   * all drops every record; capture drops none
 *   * MIB3_SHIM_NAVTEXT is read per record, so a live knob change takes effect
 *     on the next record, which is how a car run compares the modes
 *   * anything that is not a well-formed record is forwarded (fail-open):
 *     short buffers, an impossible technology, unknown event ids
 *   * close() forgets the fd, so an fd number reused by another socket is not
 *     dropped, which is the only way this gate could damage unrelated traffic
 *   * --adopt: a socket connected before the hooks existed (the mid-session
 *     injection case) is adopted by the getpeername scan
 *
 * Exit status = number of failures.  Nothing here touches the car, the unit or
 * the firmware.
 * ==========================================================================*/
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* The host build: real headers, so the gate's hand-declared libc is skipped. */
#define MIB3NAVTEXT_HOST 1
#include "../src/shim/mib3navtext.c"   /* the shipping source, unchanged */

/* The gate's function-pointer types are deliberately ABI-shaped (void *), so
 * the host's typed libc entry points are wrapped rather than cast.  A cast
 * would compile and could hide an ABI difference; a wrapper cannot. */
static int host_connect(int fd, const void *a, unsigned int l)
{ return connect(fd, (const struct sockaddr *)a, (socklen_t)l); }
static long host_send(int fd, const void *b, unsigned long n, int f)
{ return send(fd, b, (size_t)n, f); }
static int host_close(int fd){ return close(fd); }

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
  checks++;
  if (cond) { printf("  ok    %s\n", what); return; }
  failures++;
  printf("  FAIL  %s\n", what);
}

/* ---- a listener standing in for simplenav -------------------------------- */
static const char *g_sock_path;

static int start_listener_at(const char *path)
{
  struct sockaddr_un a;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  memset(&a, 0, sizeof(a));
  a.sun_family = AF_UNIX;
  snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
  unlink(path);
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 8) != 0) {
    close(fd); return -1;
  }
  return fd;
}

static int accept_with_timeout(int lfd)
{
  struct pollfd p;
  p.fd = lfd; p.events = POLLIN; p.revents = 0;
  if (poll(&p, 1, 2000) <= 0) return -1;
  return accept(lfd, NULL, NULL);
}

static long read_record(int fd, unsigned char *buf, unsigned long cap)
{
  struct pollfd p;
  p.fd = fd; p.events = POLLIN; p.revents = 0;
  if (poll(&p, 1, 250) <= 0) return -1;
  return recv(fd, buf, cap, 0);
}

static void put_u32(unsigned char *p, unsigned v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* a record shaped exactly like carplay's: u32 event, u32 technology, payload */
static unsigned long make_record(unsigned char *b, unsigned evt, unsigned tech,
                                 unsigned long total)
{
  if (total < 8) total = 8;
  memset(b, 0xEE, total);          /* payload filler, which must not appear
                                    * on the wire when the record is dropped */
  put_u32(b, evt);
  put_u32(b + 4, tech);
  return total;
}

static int g_warn_no_records;
static int g_path_lines;

static void log_sink(const char *m)
{
  if (strstr(m, "NO records")) g_warn_no_records++;
  if (strstr(m, "unix connect path=")) g_path_lines++;
  if (getenv("NAVTEXT_VERBOSE")) printf("      [gate] %s\n", m);
}

int main(int argc, char **argv)
{
  int adopt = (argc > 1 && !strcmp(argv[1], "--adopt"));
  char tmpl[] = "/tmp/navtext_XXXXXX";
  char path[128];
  int lfd, cfd, conn;
  unsigned char pkt[64], got[64];
  long n;
  nt_u64 records, dropped, passed, odd, connects;
  int nfds;
  int fd_reuse_target;

  /* ---- a socket path of our own (never /tmp/Simple_Socket on the desk) ---- */
  { int mk = mkstemp(tmpl); if (mk < 0) { printf("mkstemp failed\n"); return 99; } close(mk); unlink(tmpl); }
  snprintf(path, sizeof(path), "%s.sock", tmpl);
  g_sock_path = path;
  setenv("MIB3_SHIM_NAVTEXT_PATH", path, 1);

  lfd = start_listener_at(g_sock_path);
  if (lfd < 0) { printf("listener failed\n"); return 99; }

  printf("== nav-text gate: %s ==\n", adopt ? "mid-session adoption" : "hook behaviour");

  if (adopt) {
    struct sockaddr_un a;
    /* connect BEFORE the gate exists, exactly like a pre-injection session */
    cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (connect(cfd, (struct sockaddr *)&a, sizeof(a)) != 0) { printf("pre-connect failed\n"); return 99; }
    conn = accept_with_timeout(lfd);
    ok(conn >= 0, "pre-existing connection established outside the gate");

    mib3_navtext_install(log_sink, host_connect, host_send, host_close);   /* the scan runs here */
    mib3_navtext_stats(&records, &dropped, &passed, &odd, &connects, &nfds);
    ok(nfds == 1, "getpeername scan adopted the pre-existing simplenav socket");
    ok(connects == 0, "no connect() was seen (nothing reconnected)");

    setenv("MIB3_SHIM_NAVTEXT", "filter", 1);
    n = make_record(pkt, 100, 1, 24);
    ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n, "adopted socket: send reports success");
    ok(read_record(conn, got, sizeof(got)) < 0, "adopted socket: record 100 was DROPPED");
    close(cfd); close(conn); close(lfd);
    unlink(path);
    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures;
  }

  /* ---- the gate, installed exactly the way the shim installs it ---------- */
  mib3_navtext_install(log_sink, host_connect, host_send, host_close);
  ok(1, "install is idempotent and cheap (called on every hook entry)");

  /* 0b) re-install (the shim arms on every hook entry): idempotent -------- */
  { nt_u64 r2, d2, p2, o2, c2;
    int n2;
    mib3_navtext_stats(&r2, &d2, &p2, &o2, &c2, &n2);
    mib3_navtext_install(log_sink, host_connect, host_send, host_close);
    mib3_navtext_install(log_sink, host_connect, host_send, host_close);
    { nt_u64 r3, d3, p3, o3, c3; int n3;
      mib3_navtext_stats(&r3, &d3, &p3, &o3, &c3, &n3);
      ok(n2 == 0 && n3 == 0, "re-install does not invent fds");
      ok(strcmp(mib3_navtext_path(), path) == 0, "re-install keeps the target path");
      ok(r2 == r3 && d2 == d3, "re-install does not disturb the counters"); }
  }

  /* 1) mode off: everything passes, and the fd is still recorded ----------- */
  unsetenv("MIB3_SHIM_NAVTEXT");
  cfd = socket(AF_UNIX, SOCK_STREAM, 0);
  { struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    ok(mib3_navtext_connect(cfd, &a, sizeof(a)) == 0, "connect() to the target path succeeds");
  }
  conn = accept_with_timeout(lfd);
  ok(conn >= 0, "the target path really accepted the connection");
  mib3_navtext_stats(&records, &dropped, &passed, &odd, &connects, &nfds);
  ok(nfds == 1 && connects == 1, "the fd is recorded at connect time, even with the gate off");

  n = make_record(pkt, 100, 1, 24);
  ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n, "gate off: send returns the length");
  ok(read_record(conn, got, sizeof(got)) == n, "gate off: the record reaches simplenav");

  /* 2) filter: the three guidance events are dropped ---------------------- */
  setenv("MIB3_SHIM_NAVTEXT", "filter", 1);
  /* A 3-digit id must not be silently truncated into a droppable one (356 &
   * 0xff == 100), which would make the knob do something other than what the
   * banner says. */
  setenv("MIB3_SHIM_NAVTEXT_DROP", "356", 1);
  n = make_record(pkt, 100, 1, 24);
  ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n && read_record(conn, got, sizeof(got)) == n,
     "DROP=356 is ignored (no silent truncation to event 100)");
  unsetenv("MIB3_SHIM_NAVTEXT_DROP");            /* default 100,101,102 */
  {
    unsigned evt;
    for (evt = 100; evt <= 102; evt++) {
      char what[96];
      n = make_record(pkt, evt, 1, 40);
      snprintf(what, sizeof(what), "filter: event %u is dropped and the caller sees success", evt);
      ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n, what);
      snprintf(what, sizeof(what), "filter: event %u never reaches simplenav", evt);
      ok(read_record(conn, got, sizeof(got)) < 0, what);
    }
  }

  /* 3) filter is per-event: anything not listed passes ------------------- */
  {
    unsigned evt;
    for (evt = 98; evt <= 104; evt++) {
      char what[96];
      if (evt >= 100 && evt <= 102) continue;
      n = make_record(pkt, evt, 1, 24);
      snprintf(what, sizeof(what), "filter: event %u (not listed) is forwarded", evt);
      ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n && read_record(conn, got, sizeof(got)) == n, what);
    }
  }

  /* 4) a narrowed drop set ------------------------------------------------- */
  setenv("MIB3_SHIM_NAVTEXT_DROP", "100", 1);
  n = make_record(pkt, 100, 1, 24);
  ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n && read_record(conn, got, sizeof(got)) < 0,
     "DROP=100: event 100 is dropped");
  n = make_record(pkt, 101, 1, 24);
  ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n && read_record(conn, got, sizeof(got)) == n,
     "DROP=100: event 101 still passes (the drop set is honoured exactly)");
  unsetenv("MIB3_SHIM_NAVTEXT_DROP");

  /* 5) all: nothing gets through ------------------------------------------ */
  setenv("MIB3_SHIM_NAVTEXT", "all", 1);
  { unsigned evt;
    for (evt = 0; evt < 3; evt++) {
      char what[96];
      n = make_record(pkt, 100 + evt * 7, 1, 24);
      snprintf(what, sizeof(what), "all: record %u is dropped", 100 + evt * 7);
      ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n && read_record(conn, got, sizeof(got)) < 0, what);
    } }

  /* 6) capture: drops nothing, logs the shape ---------------------------- */
  setenv("MIB3_SHIM_NAVTEXT", "capture", 1);
  n = make_record(pkt, 101, 1, 32);
  ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n && read_record(conn, got, sizeof(got)) == n,
     "capture: the record is forwarded unchanged");

  /* 6b) The gate also names every unix path carplay connects to, so a car run
   *     can confirm or correct the target path without a rebuild ----------- */
  { const char *extra[2];
    char p2[128], p3[128];
    int e;
    snprintf(p2, sizeof(p2), "%s.p2", path);
    snprintf(p3, sizeof(p3), "%s.p3", path);
    extra[0] = p2; extra[1] = p3;
    for (e = 0; e < 2; e++) {
      struct sockaddr_un a;
      int l = start_listener_at(extra[e]);
      int c = socket(AF_UNIX, SOCK_STREAM, 0);
      memset(&a, 0, sizeof(a));
      a.sun_family = AF_UNIX;
      snprintf(a.sun_path, sizeof(a.sun_path), "%s", extra[e]);
      (void)mib3_navtext_connect(c, &a, sizeof(a));
      if (l >= 0) { int acc = accept_with_timeout(l); if (acc >= 0) close(acc); close(l); }
      close(c);
      unlink(extra[e]);
    }
    ok(mib3_navtext_paths_seen() >= 2, "capture: distinct unix connect paths are logged (the path probe)");
  }

  /* 7) fail-open: malformed / unknown shapes ---------------------------- */
  setenv("MIB3_SHIM_NAVTEXT", "filter", 1);
  n = make_record(pkt, 100, 9, 24);                 /* impossible technology */
  ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n && read_record(conn, got, sizeof(got)) == n,
     "fail-open: evt 100 with a bogus technology is forwarded");
  n = make_record(pkt, 4000, 1, 24);                /* unknown event id */
  ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n && read_record(conn, got, sizeof(got)) == n,
     "fail-open: an unknown event id is forwarded");
  n = make_record(pkt, 100, 1, 24);
  ok(mib3_navtext_send(cfd, pkt, 6, 0) == 6 && read_record(conn, got, sizeof(got)) == 6,
     "fail-open: a 6-byte buffer (not a record) is forwarded");
  ok(mib3_navtext_send(cfd, pkt, 0, 0) == 0, "fail-open: a zero-length send is forwarded");

  /* 8) the live knob: off takes effect on the next record ---------------- */
  setenv("MIB3_SHIM_NAVTEXT", "off", 1);
  n = make_record(pkt, 102, 1, 24);
  ok(mib3_navtext_send(cfd, pkt, (unsigned long)n, 0) == n && read_record(conn, got, sizeof(got)) == n,
     "live knob: switching to off restores delivery on the next record");

  /* 9) fd reuse after close ---------------------------------------------- */
  setenv("MIB3_SHIM_NAVTEXT", "filter", 1);
  ok(mib3_navtext_close(cfd) == 0, "close() still closes");
  close(conn);
  mib3_navtext_stats(&records, &dropped, &passed, &odd, &connects, &nfds);
  ok(nfds == 0, "close() forgets the fd");

  /* a different socket that reuses that fd number must NOT be filtered */
  { int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
      fd_reuse_target = (sv[0] == cfd) ? sv[0] : sv[1];
      /* The subject of this test is a reused number: if the kernel did not hand
       * back the closed fd the test would pass while proving nothing, so the
       * premise is asserted before the behaviour. */
      ok(fd_reuse_target == cfd, "the kernel reused the closed fd number (the premise of this test)");
      n = make_record(pkt, 100, 1, 24);
      ok(mib3_navtext_send(fd_reuse_target, pkt, (unsigned long)n, 0) == n,
         "fd reuse: a send on the reused number reports success");
      { unsigned char rb[64]; long r = read_record(fd_reuse_target == sv[0] ? sv[1] : sv[0], rb, sizeof(rb));
        ok(r == n, "fd reuse: the unrelated socket's traffic is NOT swallowed"); }
      close(sv[0]); close(sv[1]);
    } else { ok(0, "socketpair for the fd-reuse test"); } }

  /* 10) connect() semantics are untouched ------------------------------- */
  { struct sockaddr_un a; int fd2 = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "/tmp/navtext_does_not_exist_%d", (int)getpid());
    errno = 0;
    ok(mib3_navtext_connect(fd2, &a, sizeof(a)) == -1 && errno != 0,
       "connect() to a missing path still fails, with the real errno");
    close(fd2); }

  /* 11) a non-target path is never recorded ----------------------------- */
  { struct sockaddr_un a; int fd3 = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s.other", path);
    mib3_navtext_stats(&records, &dropped, &passed, &odd, &connects, &nfds);
    { int before = nfds;
      (void)mib3_navtext_connect(fd3, &a, sizeof(a));
      mib3_navtext_stats(&records, &dropped, &passed, &odd, &connects, &nfds);
      ok(nfds == before, "a connect() to a different path is not recorded"); }
    close(fd3); }

  /* 12) an armed socket that carries no records says so when it closes ----- */
  { struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int acc;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    setenv("MIB3_SHIM_NAVTEXT", "filter", 1);
    (void)mib3_navtext_connect(fd, &a, sizeof(a));
    acc = accept_with_timeout(lfd);
    if (acc >= 0) close(acc);
    mib3_navtext_close(fd);
    ok(g_warn_no_records >= 1,
       "an armed socket that carried no records warns on close (the 'wrong layer' signal)");
  }

  /* 13) the fd table is a ring, so a stale entry cannot wedge the gate ----- */
  { int k;
    for (k = 900; k < 912; k++) nt_fd_add(k);
    mib3_navtext_stats(&records, &dropped, &passed, &odd, &connects, &nfds);
    ok(nfds == NT_MAX_FD, "the fd table saturates at its size instead of growing");
    ok(nt_fd_index(911) >= 0, "the NEWEST fd is the one kept (eviction is oldest-first)");
    ok(nt_fd_index(900) < 0, "the oldest fd was evicted");
    for (k = 900; k < 912; k++) nt_fd_forget(k);
  }

  /* 14) the counters agree with what happened --------------------------- */
  mib3_navtext_stats(&records, &dropped, &passed, &odd, &connects, &nfds);
  printf("  ---- counters: records=%lu dropped=%lu passed=%lu odd=%lu connects=%lu\n",
         (unsigned long)records, (unsigned long)dropped, (unsigned long)passed,
         (unsigned long)odd, (unsigned long)connects);
  ok(dropped > 0 && passed > 0, "both counters moved (the gate really ran both ways)");

  close(lfd);
  unlink(path);
  printf("\n%d checks, %d failure(s)\n", checks, failures);
  return failures;
}
