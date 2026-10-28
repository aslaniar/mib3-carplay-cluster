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
 *         --sport <n>  bind the UDP SOURCE port (0 = let the kernel choose).
 *                      The cluster receiver drops video from any port except the
 *                      one multi-screen-server sends from, so this is mandatory
 *                      for a live run.  Discovery is `pathb/msport.py`
 *                      (`--proc`, read-only, /proc/<pid>/fd + /proc/net/udp6);
 *                      this tool refuses to guess and exits 3 if the bind fails.
 *
 * LIVE MODE (2026-10-26 and later)
 *   unix input now RE-ACCEPTS forever: the shim reconnects freely and each new
 *   connection continues the same RTP sequence and PTS clock (PSI is re-emitted
 *   first, so the receiver re-acquires).  It also FOLDS the shim's parameter-set
 *   records into the following frame, so one PES carries SPS+PPS+slice -- the
 *   only framing the ECU has ever decoded.  See the folding block below.
 *   `--no-fold` reproduces the old behaviour and exists ONLY so the bench can
 *   prove its framing assertion detects the difference.
 *
 * START GATE (2026-10-26 later).  The ECU's map app is a waiting state that a
 * stream has to *start* correctly: it shows its placeholder when it acquires a
 * stream it cannot decode, and it does not retry within that acquisition (see
 * FINDINGS_CLUSTER_PLACEHOLDER_2026-10-26.md).  So: hold everything until the
 * pending sequence header AND an IDR slice are both in hand, emit that first
 * complete access unit, and only then stream continuously.  This is the same
 * invariant the decoder has enforced since 2026-10-24 ("never feed a P-slice
 * before a keyframe") and the one thing the 2026-10-26 live run did not do.
 * `--no-idr-gate` disables it (bench comparison / old behaviour).
 *
 * The gate is NOT reset when the shim reconnects -- a transient socket reconnect
 * does not invalidate reference frames, and resetting it once cost 1749 AUs on
 * the decode path.  It is reset when we detect we have fallen behind (see
 * MAX_PENDING below), because then the reference chain is genuinely broken.
 *
 * BACKLOG GUARD.  If more than MAX_PENDING bytes are queued on the AU socket when
 * we finish an AU, we are behind: drain forward and wait for the next IDR instead
 * of dumping stale frames at the ECU.  A burst is *measured* to knock the cluster
 * into its placeholder, so flooding is never the lesser evil.
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
extern int   chmod(const char *, unsigned);
extern int   ioctl(int, unsigned long, ...);
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
extern int   memcmp(const void *, const void *, usize_t);
extern int   strcmp(const char *, const char *);
extern int   strncmp(const char *, const char *, usize_t);
extern usize_t strlen(const char *);
extern int   atoi(const char *);
extern int   inet_pton(int, const char *, void *);
extern unsigned if_nametoindex(const char *);
extern int   clock_gettime(int, void *);
extern int   poll(void *, unsigned long, int);
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
#define FIONREAD_        0x541B   /* asm-generic (aarch64) */
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
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <poll.h>
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
#define FIONREAD_ FIONREAD
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

/* TS payload capacity, and the trap that used to live here (fixed 2026-10-27).
 *
 *   afc=01 (no adaptation field)  payload starts at 4,   capacity 184, and it
 *                                 MUST occupy all 184 -- a short payload without
 *                                 an adaptation field is not legal stuffing.
 *   afc=11 with LEN adaptation    payload starts at 5+LEN, capacity 183-LEN.
 *
 * So a payload of exactly 183 bytes has NO legal encoding: afc=01 needs 184 and
 * afc=11 spends one byte on the adaptation field, leaving 182.  The old code
 * computed afb = 183 - plen = 0, then forced it to 1, and copied 183 bytes at
 * offset 6 -- one byte past the end of a 188-byte stack array, with the last
 * payload byte silently dropped (mux_push copies only TS_LEN).  Every access
 * unit whose length left a 183-byte tail therefore lost its final byte.  The
 * clamp below is what makes 183 impossible: 182 + a 1-byte adaptation field. */
static int ts_payload_capacity(int want_pcr) { return want_pcr ? 176 : 184; }

/* AF flags, matched to multi-screen-server (measured 2026-10-27).
 *
 * The car's own stream, the only one this ECU animates, sets
 *     random_access_indicator = 1 on EVERY access unit
 * and emits PCR on a ~100 ms cadence, not per AU.  Its 81 AUs in one capture:
 *     73 x  afb=7 flags=0x50   (RAI + PCR)
 *      8 x  afb=1 flags=0x40   (RAI only, no PCR)
 * Ours were 50 of 50 with flags=0x10 (PCR only, NO RAI) -- the one systematic
 * wire difference left after the framing, boundary, pacing and clock fixes.
 *
 * RAI is the demuxer's "a presentable picture starts here" marker.  Without it
 * the only pictures this ECU paints are the ones that flush the decoder anyway
 * -- IDRs.  Measured on the car: an all-I stream animates smoothly at 30 fps, a
 * keyframe-every-10-frames stream paints once a second, and the phone's stream
 * (one IDR per ~88 s) paints once.  That is the whole symptom.
 */
#define AF_RAI 0x40
#define AF_PCR 0x10

static void pack_ts(u8 *o, int pid, int pusi, int cc,
                    const u8 *pay, int plen, int want_pcr, u64 pcr_90k, int rai)
{
    int cap = ts_payload_capacity(want_pcr);
    if (plen > cap) plen = cap;
    if (!want_pcr && !rai && plen == 183) plen = 182;  /* 183 has no encoding */

    memset(o, 0xFF, TS_LEN);
    o[0] = 0x47;
    o[1] = (u8)((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    o[2] = (u8)(pid & 0xFF);

    int afb = want_pcr ? 7 : (rai ? 1 : (183 - plen));
    if (afb < 0) afb = 0;

    if (afb == 0) {
        o[3] = (u8)(0x10 | (cc & 0x0F));
        if (pay && plen) memcpy(o + 4, pay, plen);
    } else {
        int off = 5 + afb;
        o[3] = (u8)(0x30 | (cc & 0x0F));
        o[4] = (u8)afb;
        if (want_pcr) {
            u64 base = pcr_90k & 0x1FFFFFFFFull; u32 ext = 0;
            o[5]  = (u8)((rai ? AF_RAI : 0) | AF_PCR);
            o[6]  = (u8)(base >> 25);
            o[7]  = (u8)(base >> 17);
            o[8]  = (u8)(base >> 9);
            o[9]  = (u8)(base >> 1);
            o[10] = (u8)(((base & 1) << 7) | 0x7E | (ext >> 8));
            o[11] = (u8)(ext & 0xFF);
            if (plen > 176) plen = 176;
        } else if (rai) {
            o[5] = AF_RAI;                    /* adaptation_field_length = 1 */
            if (plen > 182) plen = 182;
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
    pack_ts(o, 0, 1, cc, pay, 184, 0, 0, 0);
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
    pack_ts(o, pmt_pid, 1, cc, pay, 184, 0, 0, 0);
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
    u64      t0_ms, last_psi_ms, pts_last, last_pcr_ms;
    int      out_fd, to_file;
    u32      ssrc;
    int      tsid, pmt_pid, es_pid, pcr_pid;
    u64      nulls;              /* TS null packets emitted to end AUs on a boundary */
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

/* TS null packet (PID 0x1FFF).  A receiver MUST ignore these; the payload is
 * stuffing.  multi-screen-server emits them with an uninitialised payload (its
 * padding is a bare 4-byte header write into a reused buffer -- measured, 206
 * distinct "payloads" in one capture), so the bytes are not part of the format.
 * We write 0xFF because that is deterministic, which our byte-exact tests need. */
static void mux_push_null(Mux *m)
{
    u8 pkt[TS_LEN];
    pkt[0] = 0x47;
    pkt[1] = 0x1F; pkt[2] = 0xFF;           /* PID 0x1FFF */
    pkt[3] = 0x10;                          /* payload only */
    memset(pkt + 4, 0xFF, TS_LEN - 4);
    mux_push(m, pkt);
    m->nulls++;
}

/* END EVERY ACCESS UNIT ON A DATAGRAM BOUNDARY (2026-10-27, measured).
 *
 * multi-screen-server never lets one datagram carry the tail of an access unit
 * and the head of the next: in the car's own nav capture, 16 of 1586 datagrams
 * straddle a boundary (1%, and those are the PSI insertions) and 76 datagrams
 * are padded with null packets to fill the last one.  Our sender straddled in
 * 1669 of 8948 (19%) and used no padding at all, because packets were packed
 * greedily and the buffer was only flushed when it reached six.
 *
 * That is worse than cosmetic.  An AU whose packet count is not a multiple of
 * six left its final packets in the buffer until the NEXT AU arrived, so the
 * last picture before the phone went quiet -- which, with a static map, is most
 * of them -- sat on the wire incomplete for up to the length of the silence
 * (600-840 ms was measured).  The receiver could not finish that PES until the
 * next AU started.
 *
 * So: pad and flush.  Same total AUs, same order, same rate; every AU now
 * completes on the wire the moment it is emitted, and the wire matches MS-S. */
static void mux_end_au(Mux *m)
{
    while (m->ndg != 0) mux_push_null(m);
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
    /* PCR CADENCE, as the unit's own encoder does it: every ~90 ms, not on every
     * access unit (measured: 73 of 81 AUs carry one).  The accession unit always
     * carries the random_access_indicator, so a packet without a PCR still has a
     * 1-byte adaptation field to hold it. */
    int want_pcr = (m->last_pcr_ms == 0 || ms - m->last_pcr_ms >= 90);
    int room = (want_pcr ? 176 : 182) - hn;
    int first = len < room ? len : room;
    memcpy(es + hn, au, (usize_t)first);
    pack_ts(pkt, m->es_pid, 1, m->cc_es++ & 0x0F, es, hn + first, want_pcr, pts90k, 1);
    if (want_pcr) m->last_pcr_ms = ms;
    mux_push(m, pkt);

    int off = first;
    while (off < len) {
        int n = len - off;
        if (n > 184) n = 184;
        if (n == 183) n = 182;           /* 183 has no legal TS encoding */
        pack_ts(pkt, m->es_pid, 0, m->cc_es++ & 0x0F, au + off, n, 0, 0, 0);
        mux_push(m, pkt);
        off += n;
    }
    mux_end_au(m);
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

/* ------------------------------------------- parameter-set folding (unix mode)
 * The shim stamps the sequence header ahead of every AU as its OWN record:
 *
 *     sink_feed(g_cl_cfg_annexb, ...)   -> record: SPS+PPS          (no VCL)
 *     sink_feed(rbuf, pl, ...)          -> record: the frame        (VCL)
 *
 * A record-per-PES muxer therefore emits two PES per frame:
 *
 *     [PES: SPS+PPS] [PES: IDR] [PES: SPS+PPS] [PES: P] ...
 *
 * Every stream this ECU has ever decoded carried the parameter sets INSIDE the
 * same access unit as the slice -- that is how file mode splits Annex-B (see
 * nal_is_new_au: SPS/PPS before the first slice do not open a new AU).  The
 * 2026-10-26 live run framed them separately and the cluster answered with its
 * "Starting map display..." placeholder.  So: hold a VCL-free record and
 * prepend it to the next record that carries VCL, giving one PES per frame and
 * the same AU boundaries as file mode.
 *
 * Bounded on purpose: the shim re-sends an identical header every frame, so an
 * unconditional append would grow without limit.  A record carrying SPS/PPS
 * REPLACES the pending header when it already holds one of the same kind (that
 * is also what makes an SPS change take effect); anything else (SEI/AUD)
 * appends.  Cap is defensive.
 */
#define CFG_CAP_MAX (256u * 1024u)

typedef struct { u8 *b; usize_t len, cap; int has_sps, has_pps; } Cfg;

static int nal_scan(const u8 *d, usize_t n, int *has_vcl, int *has_sps, int *has_pps)
{
    usize_t i = 0;
    *has_vcl = *has_sps = *has_pps = 0;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) {
            usize_t hdr = i + 3;
            if (hdr < n) {
                int t = d[hdr] & 0x1F;
                if (t == 1 || t == 5) *has_vcl = 1;
                else if (t == 7)      *has_sps = 1;
                else if (t == 8)      *has_pps = 1;
            }
            i = hdr;
        } else i++;
    }
    return 0;
}

static int cfg_put(Cfg *c, const u8 *d, usize_t n)
{
    if (c->len + n > CFG_CAP_MAX) { c->len = 0; c->has_sps = c->has_pps = 0; }
    if (c->len + n > c->cap) {
        usize_t nc = (c->len + n) * 2;
        u8 *nb = (u8 *)realloc(c->b, nc);
        if (!nb) return -1;
        c->b = nb; c->cap = nc;
    }
    memcpy(c->b + c->len, d, n); c->len += n;
    return 0;
}

/* Offset of the LAST start code in `d` whose NAL is `type`; `n` when absent. */
static usize_t last_nal_of(const u8 *d, usize_t n, int type)
{
    usize_t i = 0, found = n;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) {
            usize_t sc = i;
            while (sc > 0 && d[sc-1] == 0) sc--;
            if (i + 3 < n && (d[i+3] & 0x1F) == type) found = sc;
            i += 3;
        } else i++;
    }
    return found;
}

/* returns 1 when the record was absorbed as a pending header (nothing to send) */
/* `*changed` = the stored header is DIFFERENT from the one we already held.
 * The caller uses it to decide whether a new sequence header owes a ride on the
 * next VCL access unit (see the FOLD POLICY note above the call site). */
static int cfg_absorb(Cfg *c, const u8 *rec, usize_t len, int *changed)
{
    int vcl, sps, pps;
    nal_scan(rec, len, &vcl, &sps, &pps);
    if (vcl) return 0;                                 /* caller emits it       */
    if (sps) {
        /* Keep exactly ONE sequence header: if the record carries several
         * SPS/PPS pairs (an over-eager feeder, or a stream that repeats them),
         * the active one is the LAST -- drop everything before it.  The ECU has
         * only ever been fed one header per access unit. */
        usize_t off = last_nal_of(rec, len, 7);
        if (off > 0 && off < len) { rec += off; len -= off; }
    }
    /* Compare BEFORE the supersede block below clears the buffer -- doing it
     * after makes every repeat look "changed", which silently restored the
     * old put-a-header-on-every-frame behaviour on the car (2026-10-27). */
    *changed = !(c->len == len && len > 0 && memcmp(c->b, rec, len) == 0);
    if (sps || pps) {
        /* A new/refreshed sequence header supersedes the pending one. */
        if (c->len && ((sps && c->has_sps) || (pps && c->has_pps))) {
            c->len = 0; c->has_sps = c->has_pps = 0;
        }
    }
    if (cfg_put(c, rec, len) < 0) { *changed = 0; return 1; }  /* OOM: drop, do not die */
    if (sps) c->has_sps = 1;
    if (pps) c->has_pps = 1;
    return 1;
}

static void cfg_reset(Cfg *c) { c->len = 0; c->has_sps = c->has_pps = 0; }

/* --------------------------------------------------------------- start gate ---
 * See the header.  `gate_on` = hold until a decodable start; `streaming` = 1 once
 * the first complete (params + IDR) access unit has been emitted.  Both live for
 * the process, NOT per connection: a shim reconnect must not throw away the
 * reference chain (that mistake cost 1749 AUs on the decode path).
 */
#define MAX_PENDING (512u * 1024u)

static int au_has_idr(const u8 *d, usize_t n)
{
    return last_nal_of(d, n, 5) != n;          /* a type-5 NAL is present */
}

static long pending_bytes(int fd)
{
    int avail = 0;
    if (ioctl(fd, (unsigned long)FIONREAD_, &avail) < 0) return -1;
    return (long)avail;
}

/* ------------------------------------------------------------- keepalive -----
 * The phone STOPS SENDING when its picture is static.  multi-screen-server never
 * does: it re-encodes the Weston surface every 100 ms regardless, so on the old
 * path the ECU never sees silence.  Measured on the car 2026-10-27: the phone's
 * map rendered on the cluster for ~1 s, the phone went quiet, and the ECU's map
 * app restarted into "Starting map display..." and stayed there.
 *
 * So: when no AU has arrived for `ka_ms`, re-send the last frame that carries a
 * REFERENCE slice (nal_ref_idc != 0).  Re-decoding the same reference frame is
 * harmless -- it reproduces the same picture -- and it keeps the receiver's
 * pipeline fed so it does not decide the stream died.  Non-reference (B) frames
 * are never replayed: they depend on pictures that may no longer be there.
 */

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
 * Join the group and dump whatever is already on it.  Two uses: *   (1) prove the car's own sender is live and see its exact RTP/SSRC/seq;
 *   (2) capture its datagrams so our output can be diffed against them
 *       byte-for-byte -- the only real oracle for "same container".
 * Writes raw datagrams to --out file:<path>; prints a one-line summary. */
typedef struct { u8 addr[16]; unsigned idx; } mreq_t;

/* poll()'s struct, spelled out in both builds (the freestanding one has no
 * <poll.h>).  Used by the pacer loop and by probe's wall-clock bound. */
typedef struct { int fd_; short ev_, rev_; } pfd_t;

static int probe(const char *dst, int port, const char *out, int count, int seconds, int video_only)
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
    u64 t_start = now_ms(); u64 skipped = 0;
    while (n < count) {
        /* A COUNT-ONLY bound is a trap: while the nav is parked the idle PT=98
         * beacon alone can fill every slot before the phone ever connects, so the
         * capture ends holding zero video (cost a whole session 2026-10-27).  So
         * honour `--probe-secs` as a wall-clock bound, and let --probe-video-only
         * skip the beacon entirely.
         *
         * The timeout on the wait is what makes that bound real: a blocking read()
         * on a silent group never returns, so the clock was only checked after a
         * datagram arrived -- i.e. `verify` on a dark group hung for ever. */
        pfd_t pfd; pfd.fd_ = fd; pfd.ev_ = 1; pfd.rev_ = 0;
        int pr = poll((void *)&pfd, 1, 200);
        if (pr == 0) {
            if (seconds > 0 && now_ms() - t_start >= (u64)seconds * 1000ull) {
                say("probe: window elapsed ("); say_int("", seconds); say(" s)\n");
                break;
            }
            continue;
        }
        if (pr < 0) break;
        long r = read(fd, buf, sizeof buf);
        if (r <= 0) break;
        if (r < RTP_LEN + TS_LEN) continue;
        if (video_only && (buf[1] & 0x7f) != 33) { skipped++; continue; }
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
    if (video_only) { say("probe: skipped non-video="); say_int("", (long)skipped); }
    say("probe: seq first="); say_int("", (long)first_seq);
    say("probe: seq last=");  say_int("", (long)last_seq);
    say("probe: ssrc=");      say_int("", (long)ssrc);
    (void)seconds;
    if (ofd >= 0) close(ofd);
    close(fd);
    return 0;
}

/* ------------------------------------------------------- output pacer / fill ---
 * WHY (2026-10-27, measured on the car from OUR OWN wire): the phone does not
 * send a stream, it sends BURSTS.  In 41 s of captured stream: 226 frames at
 * 60 fps (16 ms apart), 53 frames stamped inside the same millisecond, and 99
 * stalls longer than 34 ms -- including 600-840 ms holes.  The car's own stream
 * has none of that: multi-screen-server re-encodes its surface every ~100 ms for
 * as long as it is up, so the ECU's pipeline is never starved and its clock never
 * jumps.  With the framing fixed the cluster drew a fully rendered frame and then
 * fell back to "Starting map display..." twice, which is a starved/desynced
 * receiver, not a decode error.
 *
 * So the sender behaves like the unit's own encoder:
 *   --pace-ms N  (default 10)  floor between datagram groups: a 5-frame burst
 *                              leaves as an even trickle instead of a spike.
 *   --ka-ms N    (default 0 = OFF) idle fill: if nothing has arrived for N ms,
 *                              re-send the last reference frame.
 * Nothing is dropped, no frame rate is reduced: same AUs, same order, same
 * average rate -- only the delivery shape changes.
 *
 * IDLE FILL IS OFF BY DEFAULT (2026-10-27, after the measurement).  It was built
 * on the theory that the ECU needs a continuous feed, and run 5 tested that
 * theory and refuted it: with the pacer and the fill the wire had ZERO gaps
 * > 150 ms and 429 byte-identical repeats, and the cluster still showed a map
 * for ~0.5 s and fell back.  Pacing alone (run 3, which also produced a labelled
 * map) behaved the same.  Meanwhile the fill has a real cost: re-sending a
 * P-frame byte-for-byte repeats its frame_num, which a tolerant decoder treats
 * as a redundant picture and a strict one may latch as an error.  So the fill is
 * not in the default path -- it stays as an experiment behind --ka-ms, so the
 * next session can still attribute with it.
 *
 * The AU boundary fix in mux_end_au is what actually removed the starvation the
 * fill was meant to cover: an AU's tail used to wait in the buffer for the next
 * AU, so the last picture of every burst sat incomplete on the wire.
 */
#define QCAP 8

typedef struct {
    Mux *m; u64 t0; u64 frame; u64 last_emit;
    u8 *ref; usize_t ref_len, ref_cap;
    u64 step;                 /* nominal PTS step per AU (90 kHz), from arrival EWMA */
} Pace;

static int au_is_ref(const u8 *d, usize_t n)
{
    usize_t i = 0;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) {
            usize_t h = i + 3;
            if (h < n) {
                u8 b = d[h]; int t = b & 0x1F;
                if ((t == 1 || t == 5) && ((b >> 5) & 3) != 0) return 1;
            }
            i = h;
        } else i++;
    }
    return 0;
}

static void pace_emit(Pace *p, const u8 *au, usize_t len)
{
    /* A STEADY CLOCK, NOT A WALL CLOCK (2026-10-27, measured against the car).
     *
     * Our timestamps used to be `now_ms() - t0`, so every time the phone went
     * quiet the next frame carried a jump.  Measured on the live wire during a
     * real session: 5 jumps of 162-382 ms in 5 seconds.  The car's own stream,
     * the one this ECU actually decodes, has NONE -- its largest step is 103 ms
     * because multi-screen-server re-encodes on a fixed ~100 ms cadence whether
     * or not the picture changed.
     *
     * The PCR rides on every access unit and equals the PTS, so a jump in the
     * phone's silence became a jump in the PCR.  That is a clock discontinuity to
     * a demuxer, and a demuxer that resynchronises its clock restarts the decode
     * pipeline -- which is what "the map shows one frame then goes back to
     * Starting map display..." looks like.
     *
     * So the timeline advances by a BOUNDED step per access unit instead:
     *   floor  ~6.7 ms (a 150 fps burst cannot make us run backwards)
     *   ceiling 100 ms (exactly MS-S's own cadence, and it satisfies the PCR
     *           interval limit from ISO 13818-1)
     * The timeline therefore tracks roughly real time, is always monotonic, and
     * NEVER jumps.  Over a long session with many pauses it drifts behind real
     * time, which is harmless here: this is a video-only surface with no audio to
     * keep in sync, and lagging is strictly better than resynchronising. */
    /* A CONSTANT STEP, NOT A WALL-CLOCK DELTA (2026-10-27, second measurement).
     *
     * The clamp above bounds the jumps but does not remove them: the phone
     * delivers in bursts, so consecutive AUs got 990 / 1440 / 4500 / 5040 / 9000
     * ticks while the frames themselves arrive every ~32 ms.  A decoder that
     * schedules off PTS/PCR sees a clock that lurches.  The car's own stream --
     * the one this ECU renders perfectly -- is the opposite: 9000 ticks for
     * every single access unit (measured: min 8640, max 9270 over 81 AUs), i.e.
     * a fixed-rate encoder's constant step.
     *
     * So: give every AU the SAME step, calibrated from the long-run arrival
     * interval (EWMA, fed by the reader).  The timeline then advances exactly as
     * fast as pictures arrive, with no jitter at all, which is what the receiver
     * is built for.  Bounded to [20 ms, 100 ms] so a pathological measurement
     * cannot freeze or race the clock. */
    u64 step = p->step;
    if (step < 1800) step = 1800;             /* 20 ms  floor */
    if (step > 9000) step = 9000;             /* 100 ms ceiling */
    p->m->pts_last += step;
    u64 pts = p->m->pts_last;
    mux_au(p->m, au, (int)len, pts);
    p->frame++;
    if (au_is_ref(au, len)) {                 /* remember a fill frame */
        if (len > p->ref_cap) {
            u8 *nb = (u8 *)realloc(p->ref, len);
            if (nb) { p->ref = nb; p->ref_cap = len; }
        }
        if (len <= p->ref_cap) { memcpy(p->ref, au, len); p->ref_len = len; }
    }
    p->last_emit = now_ms();
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
    int probe_secs = 0, probe_video = 0;
    int pace_ms = 10, ka_ms = 0;   /* output shape: see the pacer block */
    int gap_ms = 0, gap_every = 0; /* --gap-ms/--gap-every: simulate the phone's silence */
    u64 gaps_sent = 0;
    int sport = 0;
    int nofold = 0;      /* regression switch ONLY: pre-2026-10-26 framing */
    int noidr = 0;       /* regression switch ONLY: no start gate */
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
        else if (!strcmp(a, "--sport") && v) {
            if (!strcmp(v, "auto")) {
                say("--sport auto: discovery is pathb/msport.py (it reads /proc/<pid>/fd\n"
                    "  and /proc/net/udp6 and prints the port); pass --sport <n>.  Refusing to guess.\n");
                return 2;
            }
            sport = atoi(v); i++;
        }
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
        else if (!strcmp(a, "--no-fold")) nofold = 1;   /* bench comparison only */
        else if (!strcmp(a, "--no-idr-gate")) noidr = 1;/* bench comparison only */
        else if (!strcmp(a, "--probe-count") && v) { probe_n = atoi(v); i++; }
        else if (!strcmp(a, "--probe-secs") && v) { probe_secs = atoi(v); i++; }
        else if (!strcmp(a, "--probe-video-only")) probe_video = 1;
        else if (!strcmp(a, "--pace-ms") && v) { pace_ms = atoi(v); i++; }
        else if (!strcmp(a, "--no-pace")) pace_ms = 0;
        else if (!strcmp(a, "--ka-ms") && v) { ka_ms = atoi(v); i++; }
        else if (!strcmp(a, "--no-ka")) ka_ms = 0;
        else if (!strcmp(a, "--gap-ms") && v) { gap_ms = atoi(v); i++; }
        else if (!strcmp(a, "--gap-every") && v) { gap_every = atoi(v); i++; }
        else { say("unknown arg"); return 2; }
    }
    if (m.npk < 1 || m.npk > MAX_PKTS) { say("bad --pkts-per-dgram"); return 2; }
    if (do_probe) return probe(dst, port, out, probe_n, probe_secs, probe_video);

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
        if (sport > 0 || (src && *src)) {
            /* The receiver only accepts video from the port multi-screen-server
             * uses, so this bind IS the takeover.  Deliberately NO SO_REUSEADDR:
             * if MS-S still holds the port we must fail loudly here rather than
             * silently share it and produce a contested group. */
            in6_t s; memset(&s, 0, sizeof s);
            s.FAMILY_ = AF_INET6_; s.PORT_ = htons((u16)sport); SLEN_SET6(s);
            if (src && *src && inet_pton(AF_INET6_, src, &s.ADDR_) != 1) { say("bad --src"); return 2; }
            if (bind(fd, (const void *)&s, sizeof s) < 0) {
                say("SPORT_BUSY: cannot bind source port "); say_int("", sport);
                say("  (multi-screen-server still holds it -- park it first)\n");
                return 3;
            }
            if (sport > 0) { say("bound source port "); say_int("", sport); }
        }
        in6_t d; memset(&d, 0, sizeof d);
        d.FAMILY_ = AF_INET6_; d.PORT_ = htons((u16)port); SLEN_SET6(d);
        if (inet_pton(AF_INET6_, host, &d.ADDR_) != 1) { say("bad --dst"); return 2; }
        if (connect(fd, (const void *)&d, sizeof d) < 0) { say("connect failed"); return 1; }
        m.out_fd = fd;
        say("-> "); say(host); say(" port "); say_int("", port);
    }

    int uni_mode = 0, ls = -1;
    if (!strncmp(in, "unix:", 5)) {
        un_t su; memset(&su, 0, sizeof su);
        su.UFAMILY_ = AF_UNIX_; SLEN_SETU(su);
        usize_t pl = strlen(in + 5); if (pl > 107) pl = 107;
        memcpy(su.UPATH_, in + 5, pl); su.UPATH_[pl] = 0;
        ls = socket(AF_UNIX_, SOCK_STREAM_, 0);
        if (ls < 0) { say("socket(unix) failed"); return 1; }
        unlink(su.UPATH_);
        if (bind(ls, (const void *)&su, sizeof su) < 0) { say("bind(unix) failed"); return 1; }
        /* The shim runs as `carplay` (uid 986) and our socket is created by root:
         * a default umask leaves 0755, and connect() fails EACCES with the right
         * SMACK label but the wrong mode (measured 2026-10-26).  Both are needed. */
        chmod(su.UPATH_, 0777);
        listen(ls, 4);
        say("listening "); say(su.UPATH_); say(" (0777, re-accepting)\n");
        uni_mode = 1;
    }

    u8 *blob = 0; usize_t blen = 0;
    int naus = 0;
    if (!uni_mode) {
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
    static u8 *fo = 0; static usize_t fcap = 0;      /* folded AU (cfg + frame) */
    Cfg cfg; memset(&cfg, 0, sizeof cfg);
    int streaming = 0;                 /* start gate: 1 once params+IDR were emitted */
    /* FOLD POLICY (2026-10-27, measured on the car).
     * The shim re-sends an identical sequence header *before every access unit*,
     * and the first live build folded it into every frame -- so 90 of 90 PES
     * carried SPS+PPS+slice.  Neither stream this ECU has ever decoded looks like
     * that: the car's own nav stream (capture_mss_nav) and our known-good test
     * pattern (capture_paced_working) both put the parameter sets ONLY on the
     * access unit that carries an IDR -- (5,7,8) for keyframes, (1,) alone for
     * everything else.  With a header in front of every frame the cluster decodes
     * the opening IDR, then never updates the picture again (observed 2026-10-27:
     * the map grid froze for 1-2 min and then fell back to "Starting map
     * display...").  So: carry the header only when it is NEW (a real SPS/PPS
     * change, or the first frame of a connection) or when the AU is an IDR. */
    int hdr_pending = 1;
    u64 held = 0, backlog = 0;

    if (!uni_mode) {
        /* --gap-ms / --gap-every: SIMULATE THE PHONE'S SILENCE (2026-10-27).
         * The phone's encoder emits nothing while its map is static -- 600-840 ms
         * measured.  Every synthetic file we ever played is metronomic, so this
         * difference has never been tested.  With a gap the AU timeline keeps
         * stepping at the nominal frame interval (the pacer's steady-clock
         * behaviour) while NOTHING is written for gap_ms: that isolates "silence
         * on the wire" from "PTS jump", which the pacer already fixed. */
        for (;;) {
            const u8 *au = aus[frame % (u64)naus]; int n = alen[frame % (u64)naus];
            u64 pts;
            if (gap_ms > 0) {
                pts = m.pts_last ? m.pts_last + (90000ull / (u64)(fps > 0 ? fps : 30))
                                 : 90000ull;
            } else {
                pts = (now_ms() - m.t0_ms) * 90ull;   /* 90 kHz, as the unit does */
                if (pts <= m.pts_last) pts = m.pts_last + 1;
            }
            m.pts_last = pts;
            mux_au(&m, au, n, pts);
            frame++;

            if (realtime) {
                u64 want = m.t0_ms + frame * (1000000ull / (fps > 0 ? fps : 30)) / 1000ull;
                u64 cur = now_ms();
                if (want > cur) usleep((unsigned)((want - cur) * 1000ull));
            }
            if (gap_ms > 0 && gap_every > 0 && (frame % (u64)gap_every) == 0) {
                gaps_sent++;
                if ((gaps_sent & 0x07) == 1) {
                    say("gap "); say_int("", (long)gaps_sent);
                    say(" after frame "); say_int("", (long)frame);
                    say(" ("); say_int("", gap_ms); say(" ms of silence)\n");
                }
                usleep((unsigned)gap_ms * 1000u);
            }
            if (!loop && frame >= (u64)naus) break;
        }
    } else {
        /* One boot = one shim, but many connections: the shim reconnects on every
         * helper restart, decoder restart and phone session.  Exiting at the
         * first EOF (the pre-2026-10-26 behaviour) killed delivery permanently
         * for the rest of the boot, so re-accept forever.  `seq` and `t0_ms`
         * live in Mux and therefore continue across connections: a reset there
         * would look like a new stream to the receiver mid-flight. */
        say("pacer: floor="); say_int("", pace_ms); say(" ms/group  idle-fill=");
        say_int("", ka_ms); say(" ms\n");
        for (;;) {
            int uni = accept(ls, 0, 0);
            if (uni < 0) { say("accept failed -- stopping\n"); break; }
            m.last_psi_ms = 0;          /* PSI first: let the receiver re-acquire */
            cfg_reset(&cfg);            /* a fresh connection owes us a header   */
            hdr_pending = 1;            /* ...which must ride its next VCL AU     */
            say("conn open  frame=");   say_int("", (long)frame);

            /* ---- PACER + IDLE FILL (see the block above) ---------------------- */
            u8 *qb[QCAP]; usize_t ql[QCAP]; usize_t qcap[QCAP];
            int qh = 0, qn = 0;         /* ring head + count: never shift pointers */
            for (int k = 0; k < QCAP; k++) { qb[k] = 0; ql[k] = 0; qcap[k] = 0; }
            Pace pc; memset(&pc, 0, sizeof pc);
            pc.m = &m; pc.t0 = m.t0_ms; pc.frame = frame;
            u64 next_emit = 0, ka_count = 0, gaps = 0, last_arrive = 0;

            for (;;) {
                pfd_t pf; pf.fd_ = uni; pf.ev_ = 1; pf.rev_ = 0;
                int pr = poll((void *)&pf, 1, 2);
                int ready = (pr > 0 && (pf.rev_ & 1));

                if (ready) {
                u8 hdr[MIB3_HELP_HDRLEN];
                long r = read(uni, hdr, MIB3_HELP_HDRLEN);
                if (r <= 0) break;
                if (r != (long)MIB3_HELP_HDRLEN) { say("short header\n"); break; }
                u32 magic, len; u64 rseq;
                memcpy(&magic, hdr, 4); memcpy(&len, hdr + 4, 4); memcpy(&rseq, hdr + 8, 8);
                (void)rseq;
                if (magic != MIB3_HELP_MAGIC || len > MIB3_HELP_MAXLEN) { say("bad helper frame\n"); break; }
                if ((usize_t)len > ucap) {
                    u8 *nb = (u8 *)realloc(uo, len);
                    if (!nb) { say("oom\n"); break; }
                    uo = nb; ucap = len;
                }
                usize_t got = 0;
                while (got < (usize_t)len) {
                    long rr = read(uni, uo + got, (usize_t)len - got);
                    if (rr <= 0) break;
                    got += (usize_t)rr;
                }
                if (got != (usize_t)len) { say("short payload -- connection lost\n"); break; }

                /* arrival-gap accounting: this is the evidence the pacer acts on */
                u64 nar = now_ms();
                if (last_arrive && nar - last_arrive > 250) gaps++;
                /* CALIBRATE THE PTS STEP from the phone's own cadence (EWMA).
                 * This is what makes the emitted timeline constant instead of
                 * lumpy -- see pace_emit.  Ignore silences (they are not the
                 * frame interval) and absurd samples. */
                if (last_arrive) {
                    u64 d = nar - last_arrive;
                    if (d >= 5 && d <= 200)
                        pc.step = pc.step ? (pc.step * 7 + d * 90) / 8 : d * 90;
                }
                last_arrive = nar;

                /* Fold a VCL-free record into the next frame (see the block above). */
                if (!nofold) {
                    int ch = 0;
                    if (cfg_absorb(&cfg, uo, (usize_t)len, &ch)) {
                        if (ch) hdr_pending = 1;   /* a real header change rides the next AU */
                        continue;
                    }
                }

                /* START GATE: hold until params + an IDR are both in hand. */
                if (!noidr && !streaming) {
                    if (!au_has_idr(uo, (usize_t)len)) {
                        held++;
                        if ((held & 0x3f) == 1) {
                            say("gate: waiting for a decodable start (IDR); held ");
                            say_int("", (long)held);
                        }
                        continue;
                    }
                    streaming = 1;
                    say("gate: IDR in hand after "); say_int("", (long)held);
                    say(" held AU(s) -- starting the stream\n");
                }

                /* Carry the sequence header only where the ECU has always seen it:
                 * on an IDR, or when it is new (see FOLD POLICY above).  Emptying
                 * this is what makes every later frame a bare slice. */
                usize_t cl = 0;
                if (!nofold && cfg.len && (hdr_pending || au_has_idr(uo, (usize_t)len))) {
                    cl = cfg.len;
                    hdr_pending = 0;
                }
                usize_t tot = cl + (usize_t)len;
                if (tot > fcap) {
                    u8 *nb = (u8 *)realloc(fo, tot);
                    if (!nb) { say("oom\n"); break; }
                    fo = nb; fcap = tot;
                }
                if (cl) memcpy(fo, cfg.b, cl);
                memcpy(fo + cl, uo, (usize_t)len);

                /* Emit on a schedule instead of immediately (see the pacer block). */
                if (qn == QCAP) {                  /* queue full: bound the latency */
                    pace_emit(&pc, qb[qh], ql[qh]);
                    qh = (qh + 1) % QCAP;
                    qn--;
                }
                int qi = (qh + qn) % QCAP;
                if (tot > qcap[qi]) {
                    u8 *nb = (u8 *)realloc(qb[qi], tot);
                    if (!nb) { say("oom\n"); break; }
                    qb[qi] = nb; qcap[qi] = tot;
                }
                memcpy(qb[qi], fo, tot);
                ql[qi] = tot; qn++;

                /* pending bytes on the socket are the real backlog signal */
                if (!noidr && qn > 3) {
                    long pend = pending_bytes(uni);
                    if (pend > (long)MAX_PENDING) {
                        qh = 0; qn = 0;
                        streaming = 0;
                        backlog++;
                        say("gate: backlog "); say_int("", pend);
                        say(" bytes -- draining forward to the next IDR\n");
                    }
                }
                }   /* end: a record was ready */

                /* Emit at most one group per pace_ms, and fill silence with the
                 * last reference frame so the ECU's pipeline is never starved. */
                u64 now2 = now_ms();
                if (qn > 0) {
                    if (now2 >= next_emit) {
                        pace_emit(&pc, qb[qh], ql[qh]);
                        qh = (qh + 1) % QCAP;
                        qn--;
                        next_emit = now2 + (u64)pace_ms;
                    }
                } else if (streaming && pc.ref_len && ka_ms && pc.last_emit &&
                           now2 - pc.last_emit >= (u64)ka_ms) {
                    pace_emit(&pc, pc.ref, pc.ref_len);      /* idle fill: same picture */
                    ka_count++;
                    next_emit = now2 + (u64)pace_ms;
                }
            }
            frame = pc.frame;
            close(uni);
            /* DRAIN WHAT WE ALREADY ACCEPTED.  The pacer holds up to QCAP access
             * units; closing with them still queued silently discarded the phone's
             * last frames (measured: 7 AUs of a 2077-AU feed never reached the
             * wire, and nothing anywhere said so).  Draining is bounded -- QCAP
             * entries, ~80 ms of picture -- and strictly better than dropping
             * them: a receiver that is handed the tail keeps a complete chain. */
            { int drained = 0;
              while (qn > 0) {
                pace_emit(&pc, qb[qh], ql[qh]);
                qh = (qh + 1) % QCAP;
                qn--;
                drained++;
              }
              frame = pc.frame;
              mux_flush(&m);            /* never leave a partial datagram behind */
              say("conn close frame=");   say_int("", (long)frame);
              if (drained) { say("  pacer: drained-at-close="); say_int("", drained); }
            }
            say("  pacer: idle-fills="); say_int("", (long)ka_count);
            say("  arrival-gaps>250ms="); say_int("", (long)gaps); say("\n");
            { int k2; for (k2 = 0; k2 < QCAP; k2++) { free(qb[k2]); } }
            free(pc.ref); pc.ref = 0; pc.ref_len = pc.ref_cap = 0;
            if (held || backlog) {
                say("  gate: held=");   say_int("", (long)held);
                say("  backlog drains="); say_int("", (long)backlog);
                say("  streaming=");    say_int("", (long)streaming);
            }
        }
    }
    mux_flush(&m);
    say("datagrams: "); say_int("", m.seq);
    if (ls >= 0) close(ls);
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
