/* ============================================================================
 * mib3clusterstream.c -- Path B: publish H.264 directly onto the head unit's
 * cluster video stream (RTP/MPEG-TS over UDP/IPv6 multicast), with NO
 * display-surface / displaymgr / multi-screen-server involvement.
 *
 * See ../../LANE_STREAM_INJECT_PATH_B_2026-10-16.md for the full derivation.
 *
 * The Arteon instrument cluster ("Digital Cockpit Pro") is a SEPARATE ECU fed by
 * the MIB3 head unit over an automotive-Ethernet VLAN (eth0.5).  The unit's
 * `multi-screen-server` grabs a Wayland surface (ivi_share), H.264-encodes it on
 * the Adreno VPU, muxes it to MPEG-TS and sends it as RTP/PT=33 over UDP/IPv6
 * multicast.  We already hold the phone's decrypted H.264 access units (the
 * shim's receiver).  This tool re-muxes those *existing* AUs into the EXACT
 * container the cluster ECU expects, byte-for-byte mirroring what
 * multi-screen-server emits, and sends them to the same group:port.
 *
 * Path B therefore never touches a surface, a GL context, an OMX encoder or
 * displaymgr.  It is one UDP socket.
 *
 * WIRE FORMAT (reverse-engineered from
 *   R8830L_ROOTFS/lge/app_ro/graphics/multi-display/multi-screen-server ; symbol
 *   addresses below are that binary's VAs):
 *
 *   UDP datagram = 12-byte RTP header + N x 188-byte MPEG-TS packets
 *                  (unit sends N=6 -> 1140 B; TransportStream::process @0x1e9d0)
 *   RTP header   (built @0x1ea8c..0x1eb18):
 *        [0]=0x80 (V=2)  [1]=0x21 (M=0, PT=33 == MPEG-2 TS, RFC 2250)
 *        [2..3]=seq (u16 BE, starts 0)   [4..7]=timestamp (u32 BE, milliseconds)
 *        [8..11]=SSRC = 100000 (0x000186A0, u32 BE)
 *   PAT (PID 0)    : transport_stream_id 100, version byte 0xDB, program 1
 *                    -> PMT PID 1000            (getProgramAssociationTable @0x1df40)
 *   PMT (PID 1000) : PCR_PID 17, ES: stream_type 0x1B (H.264/AVC), PID 17
 *                                               (getProgramMapTable @0x1e0b8)
 *   PSI carries an MPEG-2 CRC32 + 0xFF payload stuffing, re-emitted every 500 ms
 *   Video PES (PID 17): 00 00 01 E0 00 00 80 80 05 <PTS:5>  (getStreamPacket @0x1e3b0)
 *        PUSI on the first packet of each AU, adaptation field with PCR where the
 *        unit puts it (first packet of the AU), PTS = 90 kHz ticks.
 *
 * INPUT   --in <file>        Annex-B ES (AUs split here)
 *         --in unix:<path>   shim helper protocol (mib3helper.h): 16-byte header,
 *                            one AU per message -- drop-in for mib3clusterhelper
 *         --in-fmt annexb|avcc
 * OUTPUT  --dst <ipv6>[%iface] (default ff14::2:4%eth0.5)   --src <ipv6>
 *         --port <n> (default 42800)   --out file:<path>  (desk capture)
 *
 * BUILD (desk, native):
 *     cc -O2 -o mib3clusterstream mib3clusterstream.c
 * BUILD (unit, aarch64 freestanding against the unit's own glibc):
 *     clang --target=aarch64-linux-gnu -DMIB3_UNIT_AARCH64 -O2 -nostdlib -fPIE \
 *           -fuse-ld=ld.lld -pie -Wl,-e,_start -o mib3clusterstream \
 *           mib3clusterstream.c <rootfs>/lib64/libc.so.6
 *     (see run_clusterstream_bench.sh, which does both + the desk verification)
 * ==========================================================================*/

#if defined(MIB3_UNIT_AARCH64)
/* ---- freestanding build against the unit's glibc: no host headers ---- */
typedef unsigned long     usize_t;
typedef unsigned long long u64;
typedef unsigned int      u32;
typedef unsigned short    u16;
typedef unsigned char     u8;

typedef struct { u16 family; u16 port; u32 flow; u8 addr[16]; u32 scope; } in6_t;
typedef struct { u16 family; char path[108]; } un_t;
typedef struct { long sec; long nsec; } tspec_t;

extern int   open(const char *, int, ...);
extern long  read(int, void *, usize_t);
extern long  write(int, const void *, usize_t);
extern int   close(int);
extern int   unlink(const char *);
extern int   socket(int, int, int);
extern int   setsockopt(int, int, int, const void *, unsigned);
extern int   bind(int, const void *, unsigned);
extern int   connect(int, const void *, unsigned);
extern int   listen(int, int);
extern int   accept(int, void *, unsigned *);
extern void *malloc(usize_t);
extern void *realloc(void *, usize_t);
extern void  free(void *);
extern void *memset(void *, int, usize_t);
extern void *memcpy(void *, const void *, usize_t);
extern int   strcmp(const char *, const char *);
extern int   strncmp(const char *, const char *, usize_t);
extern usize_t strlen(const char *);
extern int   atoi(const char *);
extern int   inet_pton(int, const char *, void *);
extern unsigned if_nametoindex(const char *);
extern int   clock_gettime(int, void *);
extern int   usleep(unsigned);
extern u16   htons(u16);
extern void  _exit(int);

#define AF_UNIX_         1
#define AF_INET6_        10
#define SOCK_STREAM_     1
#define SOCK_DGRAM_      2
#define SOL_SOCKET_      1
#define SO_BINDTODEVICE_ 25
#define IPPROTO_IPV6_    41
#define IPV6_MULTICAST_IF_   17
#define IPV6_MULTICAST_HOPS_ 18
#define IPV6_MULTICAST_LOOP_ 19
#define IPV6_JOIN_GROUP_     20
#define SO_REUSEADDR_        2
#define SO_REUSEPORT_        15
#define CLOCK_MONOTONIC_ 1
#define O_RDONLY_        0
#define O_WRONLY_        1
#define O_CREAT_         0x40
#define O_TRUNC_         0x200
#define STDERR_          2
/* our own struct field names (see in6_t/un_t/tspec_t above) */
#define FAMILY_   family
#define PORT_     port
#define ADDR_     addr
#define UFAMILY_  family
#define UPATH_    path
#define SEC_      sec
#define NSEC_     nsec
#define SLEN_SET6(s) ((void)0)
#define SLEN_SETU(s) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
typedef unsigned long usize_t;
typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
typedef struct sockaddr_in6 in6_t;
typedef struct sockaddr_un  un_t;
typedef struct timespec     tspec_t;
#define FAMILY_   sin6_family
#define PORT_     sin6_port
#define ADDR_     sin6_addr
#define PATH_     sun_path
#define UFAMILY_  sun_family
#define UPATH_    sun_path
#define SEC_      tv_sec
#define NSEC_     tv_nsec
#if defined(__APPLE__)
#define SLEN_SET6(s) ((s).sin6_len = sizeof(s))
#define SLEN_SETU(s) ((s).sun_len = sizeof(s))
#else
#define SLEN_SET6(s) ((void)0)
#define SLEN_SETU(s) ((void)0)
#endif
#define AF_UNIX_  AF_UNIX
#define AF_INET6_ AF_INET6
#define SOCK_STREAM_ SOCK_STREAM
#define SOCK_DGRAM_  SOCK_DGRAM
#define SOL_SOCKET_  SOL_SOCKET
#define SO_BINDTODEVICE_ SO_BINDTODEVICE
#define IPPROTO_IPV6_    IPPROTO_IPV6
#define IPV6_MULTICAST_IF_   IPV6_MULTICAST_IF
#define IPV6_MULTICAST_HOPS_ IPV6_MULTICAST_HOPS
#define IPV6_MULTICAST_LOOP_ IPV6_MULTICAST_LOOP
#define IPV6_JOIN_GROUP_     IPV6_JOIN_GROUP
#define SO_REUSEADDR_        SO_REUSEADDR
#define SO_REUSEPORT_        SO_REUSEPORT
#define CLOCK_MONOTONIC_ CLOCK_MONOTONIC
#define O_RDONLY_ O_RDONLY
#define O_WRONLY_ O_WRONLY
#define O_CREAT_  O_CREAT
#define O_TRUNC_  O_TRUNC
#define STDERR_   2
#endif

/* ---- tiny logger (works in both builds; no printf dependency) ---- */
static void say(const char *s) { usize_t n = strlen(s); write(STDERR_, s, n); }
static void say_int(const char *tag, long v)
{
    char b[96]; int k = 0;
    while (*tag) b[k++] = *tag++;
    if (v < 0) { b[k++] = '-'; v = -v; }
    char t[24]; int j = 0;
    if (!v) t[j++] = '0';
    while (v) { t[j++] = (char)('0' + v % 10); v /= 10; }
    while (j--) b[k++] = t[j];
    b[k++] = '\n';
    write(STDERR_, b, k);
}

/* ------------------------------------------------------------------ defaults */
#define DEF_DST        "ff14::2:4%eth0.5"
#define DEF_PORT       42800
#define DEF_SSRC       100000u
#define DEF_TSID       100
#define DEF_PMT_PID    1000
#define DEF_ES_PID     17
#define DEF_PCR_PID    17
#define DEF_NDGRAM     6

#define MIB3_HELP_MAGIC   0x4d334331u
#define MIB3_HELP_HDRLEN  16u
#define MIB3_HELP_MAXLEN  (8u << 20)

#define TS_LEN   188
#define RTP_LEN  12
#define MAX_PKTS 32
#define DGRAM_CAP (RTP_LEN + MAX_PKTS * TS_LEN)

/* ---------------------------------------------------------------- ts writer */
static u32 crc32_mpeg(const u8 *p, usize_t n)
{
    u32 crc = 0xFFFFFFFFu;
    for (usize_t i = 0; i < n; i++) {
        crc ^= (u32)p[i] << 24;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
    }
    return crc;
}

static void pack_ts(u8 *o, int pid, int pusi, int cc,
                    const u8 *pay, int plen, int want_pcr, u64 pcr_90k)
{
    int cap = want_pcr ? 176 : 184;         /* payload capacity */
    if (plen > cap) plen = cap;

    memset(o, 0xFF, TS_LEN);
    o[0] = 0x47;
    o[1] = (u8)((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    o[2] = (u8)(pid & 0xFF);

    int afb = want_pcr ? 7 : ((plen < 184) ? (183 - plen) : 0);
    if (afb == 0 && plen < 184) afb = 1;

    if (afb == 0) {
        o[3] = (u8)(0x10 | (cc & 0x0F));
        if (pay && plen) memcpy(o + 4, pay, plen);
    } else {
        int off = 5 + afb;
        o[3] = (u8)(0x30 | (cc & 0x0F));
        o[4] = (u8)afb;
        if (want_pcr) {
            u64 base = pcr_90k & 0x1FFFFFFFFull; u32 ext = 0;
            o[5]  = 0x10;
            o[6]  = (u8)(base >> 25);
            o[7]  = (u8)(base >> 17);
            o[8]  = (u8)(base >> 9);
            o[9]  = (u8)(base >> 1);
            o[10] = (u8)(((base & 1) << 7) | 0x7E | (ext >> 8));
            o[11] = (u8)(ext & 0xFF);
        } else {
            o[5] = 0x00;
        }
        if (pay && plen) memcpy(o + off, pay, plen);
    }
}

static void pack_pat(u8 *o, int tsid, int pmt_pid, int cc)
{
    u8 sec[16]; int n = 0;
    sec[n++] = 0x00;
    sec[n++] = 0xB0; sec[n++] = 0x0D;
    sec[n++] = (u8)(tsid >> 8); sec[n++] = (u8)tsid;
    sec[n++] = 0xDB;
    sec[n++] = 0x00; sec[n++] = 0x00;
    sec[n++] = 0x00; sec[n++] = 0x01;
    sec[n++] = (u8)(0xE0 | ((pmt_pid >> 8) & 0x1F));
    sec[n++] = (u8)(pmt_pid & 0xFF);
    u32 c = crc32_mpeg(sec, (usize_t)n);
    sec[n++] = (u8)(c >> 24); sec[n++] = (u8)(c >> 16);
    sec[n++] = (u8)(c >> 8);  sec[n++] = (u8)c;

    u8 pay[184];
    memset(pay, 0xFF, sizeof pay);
    pay[0] = 0x00;                     /* pointer_field */
    memcpy(pay + 1, sec, (usize_t)n);
    pack_ts(o, 0, 1, cc, pay, 184, 0, 0);
}

static void pack_pmt(u8 *o, int pmt_pid, int pcr_pid, int es_pid, int cc)
{
    u8 sec[32]; int n = 0;
    sec[n++] = 0x02;
    sec[n++] = 0xB0; sec[n++] = 0x12;
    sec[n++] = 0x00; sec[n++] = 0x01;
    sec[n++] = 0xC3;
    sec[n++] = 0x00; sec[n++] = 0x00;
    sec[n++] = (u8)(0xE0 | ((pcr_pid >> 8) & 0x1F));
    sec[n++] = (u8)(pcr_pid & 0xFF);
    sec[n++] = 0xF0; sec[n++] = 0x00;
    sec[n++] = 0x1B;                   /* H.264 */
    sec[n++] = (u8)(0xE0 | ((es_pid >> 8) & 0x1F));
    sec[n++] = (u8)(es_pid & 0xFF);
    sec[n++] = 0xF0; sec[n++] = 0x00;
    u32 c = crc32_mpeg(sec, (usize_t)n);
    sec[n++] = (u8)(c >> 24); sec[n++] = (u8)(c >> 16);
    sec[n++] = (u8)(c >> 8);  sec[n++] = (u8)c;

    u8 pay[184];
    memset(pay, 0xFF, sizeof pay);
    pay[0] = 0x00;
    memcpy(pay + 1, sec, (usize_t)n);
    pack_ts(o, pmt_pid, 1, cc, pay, 184, 0, 0);
}

static int pes_header(u8 *o, u64 pts)
{
    o[0] = 0x00; o[1] = 0x00; o[2] = 0x01; o[3] = 0xE0;
    o[4] = 0x00; o[5] = 0x00;
    o[6] = 0x80; o[7] = 0x80; o[8] = 0x05;
    o[9]  = (u8)(0x21 | (((pts >> 30) & 0x07) << 1));
    o[10] = (u8)((pts >> 22) & 0xFF);
    o[11] = (u8)(((pts >> 14) & 0xFE) | 0x01);
    o[12] = (u8)((pts >> 7) & 0xFF);
    o[13] = (u8)(((pts << 1) & 0xFE) | 0x01);
    return 14;
}

/* ------------------------------------------------------------------ muxer */
typedef struct {
    u8       dg[DGRAM_CAP];
    int      ndg, npk;
    int      cc_pat, cc_pmt, cc_es;
    u16      seq;
    u64      t0_ms, last_psi_ms;
    int      out_fd, to_file;
    u32      ssrc;
    int      tsid, pmt_pid, es_pid, pcr_pid;
} Mux;

static u64 now_ms(void)
{
    tspec_t ts; clock_gettime(CLOCK_MONOTONIC_, &ts);
    return (u64)ts.SEC_ * 1000u + (u64)ts.NSEC_ / 1000000u;
}

static void mux_flush(Mux *m)
{
    if (m->ndg == 0) return;
    int total = RTP_LEN + m->ndg * TS_LEN;
    u32 t = (u32)(now_ms() & 0xFFFFFFFFu);
    m->dg[0] = 0x80;
    m->dg[1] = 0x21;                                    /* PT 33 = MPEG-TS */
    m->dg[2] = (u8)(m->seq >> 8); m->dg[3] = (u8)(m->seq & 0xFF);
    m->dg[4] = (u8)(t >> 24); m->dg[5] = (u8)(t >> 16);
    m->dg[6] = (u8)(t >> 8);  m->dg[7] = (u8)t;
    m->dg[8]  = (u8)(m->ssrc >> 24); m->dg[9]  = (u8)(m->ssrc >> 16);
    m->dg[10] = (u8)(m->ssrc >> 8);  m->dg[11] = (u8)m->ssrc;
    m->seq++;
    write(m->out_fd, m->dg, (usize_t)total);
    m->ndg = 0;
}

static void mux_push(Mux *m, const u8 *pkt)
{
    memcpy(m->dg + RTP_LEN + m->ndg * TS_LEN, pkt, TS_LEN);
    if (++m->ndg >= m->npk) mux_flush(m);
}

static void mux_au(Mux *m, const u8 *au, int len, u64 pts90k)
{
    u8 pkt[TS_LEN];
    u64 ms = now_ms();
    if (m->last_psi_ms == 0 || ms - m->last_psi_ms >= 500) {
        pack_pat(pkt, m->tsid, m->pmt_pid, m->cc_pat++ & 0x0F);
        mux_push(m, pkt);
        pack_pmt(pkt, m->pmt_pid, m->pcr_pid, m->es_pid, m->cc_pmt++ & 0x0F);
        mux_push(m, pkt);
        m->last_psi_ms = ms;
    }

    u8 es[2048];
    int hn = pes_header(es, pts90k);
    int room = 176 - hn;                 /* PCR adaptation costs 7 bytes */
    int first = len < room ? len : room;
    memcpy(es + hn, au, (usize_t)first);
    pack_ts(pkt, m->es_pid, 1, m->cc_es++ & 0x0F, es, hn + first, 1, pts90k);
    mux_push(m, pkt);

    int off = first;
    while (off < len) {
        int n = len - off; if (n > 184) n = 184;
        pack_ts(pkt, m->es_pid, 0, m->cc_es++ & 0x0F, au + off, n, 0, 0);
        mux_push(m, pkt);
        off += n;
    }
}

/* ------------------------------------------------------- Annex-B AU splitter */
/* ISO 14496-10 7.4.1.2.4: a new AU begins at an AUD, or at any NAL that follows
 * a VCL NAL in the current AU.  SPS/PPS preceding the first slice belong to the
 * AU being opened, so they must not split it.  (The socket input needs none of
 * this: there each message is already exactly one AU.) */
static int nal_is_new_au(u8 h, int have_slice)
{
    int t = h & 0x1F;
    if (t == 9) return 1;
    if (!have_slice) return 0;
    if (t == 7 || t == 8 || t == 6) return 1;
    if (t == 1 || t == 5) return 1;
    return 0;
}

static u8 *avcc_to_annexb(const u8 *d, usize_t n, usize_t *outlen);

static int split_annexb(const u8 *d, usize_t n, const u8 **aus, int *alen, int maxau)
{
    usize_t i = 0; int naus = 0;
    usize_t au_start = 0; int have_slice = 0, started = 0;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) {
            usize_t sc = i;
            while (sc > 0 && d[sc-1] == 0) sc--;
            int hdr = (int)(i + 3);
            if (hdr < (int)n) {
                u8 h = d[hdr];
                if (!started) { au_start = sc; started = 1; }
                else if (nal_is_new_au(h, have_slice)) {
                    usize_t sz = sc - au_start;
                    if (sz && naus < maxau) { aus[naus] = d + au_start; alen[naus] = (int)sz; naus++; }
                    au_start = sc; have_slice = 0;
                }
                if ((h & 0x1F) == 1 || (h & 0x1F) == 5) {
                    int n2 = (int)(n - (usize_t)hdr - 1);
                    if (n2 > 0 && (d[hdr+1] & 0x80)) have_slice = 1;
                }
            }
            i += 3;
        } else i++;
    }
    if (started && au_start < n && naus < maxau) {
        aus[naus] = d + au_start; alen[naus] = (int)(n - au_start); naus++;
    }
    return naus;
}

static u8 *avcc_to_annexb(const u8 *d, usize_t n, usize_t *outlen)
{
    usize_t cap = n + 1024, o = 0, i = 0;
    u8 *b = (u8 *)malloc(cap);
    if (!b) return (u8 *)0;
    while (i + 4 <= n) {
        u32 l = ((u32)d[i] << 24) | ((u32)d[i+1] << 16) | ((u32)d[i+2] << 8) | d[i+3];
        i += 4;
        if (l == 0 || i + l > n) break;
        if (o + 4 + l > cap) {
            cap = (o + 4 + l) * 2;
            b = (u8 *)realloc(b, cap);
            if (!b) return (u8 *)0;
        }
        b[o++] = 0; b[o++] = 0; b[o++] = 0; b[o++] = 1;
        memcpy(b + o, d + i, l); o += l; i += l;
    }
    *outlen = o;
    return b;
}

static int read_all_fd(int fd, u8 **buf, usize_t *len)
{
    usize_t cap = 1 << 20, n = 0;
    u8 *b = (u8 *)malloc(cap);
    if (!b) return -1;
    for (;;) {
        if (n + 65536 > cap) {
            cap *= 2; b = (u8 *)realloc(b, cap);
            if (!b) return -1;
        }
        long r = read(fd, b + n, 65536);
        if (r < 0) return -1;
        if (r == 0) break;
        n += (usize_t)r;
    }
    *buf = b; *len = n;
    return 0;
}

/* ---------------------------------------------------------------- probe ---
 * Join the group and dump whatever is already on it.  Two uses:
 *   (1) prove the car's own sender is live and see its exact RTP/SSRC/seq;
 *   (2) capture its datagrams so our output can be diffed against them
 *       byte-for-byte -- the only real oracle for "same container".
 * Writes raw datagrams to --out file:<path>; prints a one-line summary. */
typedef struct { u8 addr[16]; unsigned idx; } mreq_t;

static int probe(const char *dst, int port, const char *out, int count, int seconds)
{
    char host[128]; const char *ifname = "eth0.5";
    usize_t dl = strlen(dst); if (dl >= sizeof host) dl = sizeof host - 1;
    memcpy(host, dst, dl); host[dl] = 0;
    for (usize_t k = 0; k < dl; k++) if (host[k] == '%') { host[k] = 0; ifname = host + k + 1; break; }

    int fd = socket(AF_INET6_, SOCK_DGRAM_, 0);
    if (fd < 0) { say("probe: socket failed"); return 1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET_, SO_REUSEPORT_, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET_, SO_REUSEADDR_, &one, sizeof one);

    in6_t a; memset(&a, 0, sizeof a);
    a.FAMILY_ = AF_INET6_; a.PORT_ = htons((u16)port); SLEN_SET6(a);
    if (bind(fd, (const void *)&a, sizeof a) < 0) { say("probe: bind failed"); return 1; }

    mreq_t mr; memset(&mr, 0, sizeof mr);
    if (inet_pton(AF_INET6_, host, mr.addr) != 1) { say("probe: bad group"); return 1; }
    mr.idx = if_nametoindex(ifname);
    if (setsockopt(fd, IPPROTO_IPV6_, IPV6_JOIN_GROUP_, &mr, (unsigned)sizeof mr) < 0) {
        say("probe: join failed"); return 1;
    }
    say("probe: joined "); say(host); say(" port "); say_int("", port);

    int ofd = -1;
    if (out && !strncmp(out, "file:", 5)) ofd = open(out + 5, O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0644);

    u8 buf[4096];
    long n = 0; u64 first_seq = 0, last_seq = 0, ssrc = 0; int have = 0;
    while (n < count) {
        long r = read(fd, buf, sizeof buf);
        if (r <= 0) break;
        if (r < RTP_LEN + TS_LEN) continue;
        u64 seq = ((u64)buf[2] << 8) | buf[3];
        u64 ss  = ((u64)buf[8] << 24) | ((u64)buf[9] << 16) | ((u64)buf[10] << 8) | buf[11];
        if (!have) { first_seq = seq; ssrc = ss; have = 1; }
        last_seq = seq; n++;
        if (ofd >= 0) write(ofd, buf, (usize_t)r);
        if (n == 1) {
            say("probe: first datagram rtp="); 
            char hx[40]; int k = 0; const char *H = "0123456789abcdef";
            for (int i = 0; i < 12; i++) { hx[k++] = H[buf[i] >> 4]; hx[k++] = H[buf[i] & 15]; }
            hx[k++] = '\n'; write(STDERR_, hx, k);
        }
    }
    say("probe: datagrams="); say_int("", n);
    say("probe: seq first="); say_int("", (long)first_seq);
    say("probe: seq last=");  say_int("", (long)last_seq);
    say("probe: ssrc=");      say_int("", (long)ssrc);
    (void)seconds;
    if (ofd >= 0) close(ofd);
    close(fd);
    return 0;
}

/* -------------------------------------------------------------------- main */
#define MAXAU (1 << 18)
static const u8 *aus[MAXAU];
static int alen[MAXAU];

int mib3_run(int argc, char **argv) __attribute__((used));
int mib3_run(int argc, char **argv)
{
    const char *in = "-", *dst = DEF_DST, *src = 0, *out = 0, *infmt = "annexb";
    int port = DEF_PORT, loop = 0, realtime = 0, fps = 30, do_probe = 0, probe_n = 200;
    Mux m; memset(&m, 0, sizeof m);
    m.npk = DEF_NDGRAM; m.ssrc = DEF_SSRC;
    m.tsid = DEF_TSID; m.pmt_pid = DEF_PMT_PID; m.es_pid = DEF_ES_PID; m.pcr_pid = DEF_PCR_PID;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i+1] : 0;
        if (!strcmp(a, "--in") && v) { in = v; i++; }
        else if (!strcmp(a, "--in-fmt") && v) { infmt = v; i++; }
        else if (!strcmp(a, "--dst") && v) { dst = v; i++; }
        else if (!strcmp(a, "--src") && v) { src = v; i++; }
        else if (!strcmp(a, "--port") && v) { port = atoi(v); i++; }
        else if (!strcmp(a, "--out") && v) { out = v; i++; }
        else if (!strcmp(a, "--pkts-per-dgram") && v) { m.npk = atoi(v); i++; }
        else if (!strcmp(a, "--ssrc") && v) { m.ssrc = (u32)atoi(v); i++; }
        else if (!strcmp(a, "--tsid") && v) { m.tsid = atoi(v); i++; }
        else if (!strcmp(a, "--pmt-pid") && v) { m.pmt_pid = atoi(v); i++; }
        else if (!strcmp(a, "--es-pid") && v) { m.es_pid = atoi(v); m.pcr_pid = atoi(v); i++; }
        else if (!strcmp(a, "--fps") && v) { fps = atoi(v); i++; }
        else if (!strcmp(a, "--loop")) loop = 1;
        else if (!strcmp(a, "--realtime")) realtime = 1;
        else if (!strcmp(a, "--probe")) do_probe = 1;
        else if (!strcmp(a, "--probe-count") && v) { probe_n = atoi(v); i++; }
        else { say("unknown arg"); return 2; }
    }
    if (m.npk < 1 || m.npk > MAX_PKTS) { say("bad --pkts-per-dgram"); return 2; }
    if (do_probe) return probe(dst, port, out, probe_n, 0);

    if (out && !strncmp(out, "file:", 5)) {
        m.to_file = 1;
        m.out_fd = open(out + 5, O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0644);
        if (m.out_fd < 0) { say("cannot open out"); return 1; }
    } else {
        char host[128]; const char *ifname = "eth0.5";
        usize_t dl = strlen(dst); if (dl >= sizeof host) dl = sizeof host - 1;
        memcpy(host, dst, dl); host[dl] = 0;
        for (usize_t k = 0; k < dl; k++) if (host[k] == '%') { host[k] = 0; ifname = host + k + 1; break; }

        int fd = socket(AF_INET6_, SOCK_DGRAM_, 0);
        if (fd < 0) { say("socket() failed"); return 1; }
        unsigned idx = if_nametoindex(ifname);
        if (idx) setsockopt(fd, IPPROTO_IPV6_, IPV6_MULTICAST_IF_, &idx, sizeof idx);
        int hop = 1;  setsockopt(fd, IPPROTO_IPV6_, IPV6_MULTICAST_HOPS_, &hop, sizeof hop);
        int lou = 1;  setsockopt(fd, IPPROTO_IPV6_, IPV6_MULTICAST_LOOP_, &lou, sizeof lou);
        if (ifname && *ifname) setsockopt(fd, SOL_SOCKET_, SO_BINDTODEVICE_, ifname, (unsigned)strlen(ifname));
        if (src && *src) {
            in6_t s; memset(&s, 0, sizeof s);
            s.FAMILY_ = AF_INET6_; s.PORT_ = 0; SLEN_SET6(s);
            if (inet_pton(AF_INET6_, src, &s.ADDR_) != 1) { say("bad --src"); return 2; }
            bind(fd, (const void *)&s, sizeof s);
        }
        in6_t d; memset(&d, 0, sizeof d);
        d.FAMILY_ = AF_INET6_; d.PORT_ = htons((u16)port); SLEN_SET6(d);
        if (inet_pton(AF_INET6_, host, &d.ADDR_) != 1) { say("bad --dst"); return 2; }
        if (connect(fd, (const void *)&d, sizeof d) < 0) { say("connect failed"); return 1; }
        m.out_fd = fd;
        say("-> "); say(host); say(" port "); say_int("", port);
    }

    int uni = -1;
    if (!strncmp(in, "unix:", 5)) {
        un_t su; memset(&su, 0, sizeof su);
        su.UFAMILY_ = AF_UNIX_; SLEN_SETU(su);
        usize_t pl = strlen(in + 5); if (pl > 107) pl = 107;
        memcpy(su.UPATH_, in + 5, pl); su.UPATH_[pl] = 0;
        int ls = socket(AF_UNIX_, SOCK_STREAM_, 0);
        if (ls < 0) { say("socket(unix) failed"); return 1; }
        unlink(su.UPATH_);
        if (bind(ls, (const void *)&su, sizeof su) < 0) { say("bind(unix) failed"); return 1; }
        listen(ls, 1);
        say("listening "); say(su.UPATH_);
        uni = accept(ls, 0, 0);
        if (uni < 0) { say("accept failed"); return 1; }
    }

    u8 *blob = 0; usize_t blen = 0;
    int naus = 0;
    if (uni < 0) {
        int fd = (!strcmp(in, "-")) ? 0 : open(in, O_RDONLY_);
        if (fd < 0) { say("cannot open in"); return 1; }
        if (read_all_fd(fd, &blob, &blen) < 0) { say("read failed"); return 1; }
        if (fd) close(fd);
        if (!strcmp(infmt, "avcc")) {
            usize_t al = 0;
            u8 *ab = avcc_to_annexb(blob, blen, &al);
            if (!ab) { say("avcc convert failed"); return 1; }
            blob = ab; blen = al;
        }
        naus = split_annexb(blob, blen, aus, alen, MAXAU);
        say("access units: "); say_int("", naus);
        if (naus == 0) return 1;
    }

    m.t0_ms = now_ms();
    u64 frame = 0;
    static u8 *uo = 0; static usize_t ucap = 0;
    for (;;) {
        const u8 *au; int n;
        if (uni >= 0) {
            u8 hdr[MIB3_HELP_HDRLEN];
            long r = read(uni, hdr, MIB3_HELP_HDRLEN);
            if (r <= 0) break;
            u32 magic, len; u64 seq;
            memcpy(&magic, hdr, 4); memcpy(&len, hdr + 4, 4); memcpy(&seq, hdr + 8, 8);
            if (magic != MIB3_HELP_MAGIC || len > MIB3_HELP_MAXLEN) { say("bad helper frame"); break; }
            if (len > ucap) { uo = (u8 *)realloc(uo, len); ucap = len; }
            usize_t got = 0;
            while (got < len) {
                long rr = read(uni, uo + got, len - got);
                if (rr <= 0) { say("short payload"); goto done; }
                got += (usize_t)rr;
            }
            au = uo; n = (int)len;
        } else {
            au = aus[frame % (u64)naus]; n = alen[frame % (u64)naus];
        }

        u64 pts = (now_ms() - m.t0_ms) * 90ull;   /* 90 kHz, as the unit does */
        mux_au(&m, au, n, pts);
        frame++;

        if (realtime && uni < 0) {
            u64 want = m.t0_ms + frame * (1000000ull / (fps > 0 ? fps : 30)) / 1000ull;
            u64 cur = now_ms();
            if (want > cur) usleep((unsigned)((want - cur) * 1000ull));
        }
        if (uni < 0 && !loop && frame >= (u64)naus) break;
    }
done:
    mux_flush(&m);
    say("datagrams: "); say_int("", m.seq);
    if (uni >= 0) close(uni);
    close(m.out_fd);
    return 0;
}

#if defined(MIB3_UNIT_AARCH64)
/* Freestanding entry: the unit's ld has no crt for us.  MUST be naked -- a
 * compiled prologue would move sp before we could read the kernel's argv. */
__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile(
        "mov x9, sp        \n\t"   /* x9 = &argc (kernel stack) */
        "ldr w0, [x9]      \n\t"   /* argc */
        "add x1, x9, #8    \n\t"   /* argv */
        "bl  mib3_run      \n\t"
        "bl  _exit         \n\t"   /* w0 = return */
        "b   .             \n\t");
}
#endif

#if !defined(MIB3_UNIT_AARCH64)
int main(int argc, char **argv) { return mib3_run(argc, argv); }
#endif
