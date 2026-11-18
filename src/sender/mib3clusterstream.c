/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ============================================================================
 * mib3clusterstream.c -- Path B: publish the phone's H.264 directly onto the
 * head unit's cluster video stream (RTP / MPEG-TS over UDP/IPv6 multicast),
 * with no display surface, display manager or multi-screen-server involved.
 *
 * The Arteon cluster ("Digital Cockpit Pro") is a separate ECU fed by the MIB3
 * head unit over an automotive-Ethernet VLAN (eth0.5). The unit's own
 * multi-screen-server grabs a Wayland surface, encodes it on the Adreno VPU,
 * muxes it to MPEG-2 TS and sends it as RTP/PT=33 over UDP/IPv6 multicast. The
 * shim already holds the phone's decrypted access units; this programme
 * re-muxes those existing units into the exact container the cluster expects
 * and sends them to the same group and port. Path B is one UDP socket.
 *
 *   OUT  --dst <ipv6>[%iface]  (default ff14::2:4%eth0.5)   --src <ipv6>
 *        --port <n> (default 42800)   --sport <n>   --out file:<path>
 *   IN   --in <file> | --in unix:<path>   --in-fmt annexb|avcc
 *
 * The wire format, the pacing model and the supervisor hand-over are documented
 * with their measurements:
 *   docs/development/pathb-wire-format.md
 *   docs/development/pathb-pacing-and-timing.md
 *   docs/development/pathb-supervisor-handover.md
 *
 * BUILD (desk):
 *     cc -O2 -o mib3clusterstream mib3clusterstream.c
 * BUILD (unit: aarch64, freestanding against the unit's own glibc):
 *     clang --target=aarch64-linux-gnu -DMIB3_UNIT_AARCH64 -O2 -nostdlib -fPIE \
 *           -fuse-ld=ld.lld -pie -Wl,-e,_start -o mib3clusterstream \
 *           mib3clusterstream.c <rootfs>/lib64/libc.so.6
 * ==========================================================================*/

/* unit_compat first: it provides every libc symbol the toolkit's printers use.
 * toolkit.h declares that set by hand for the small tools that include it alone,
 * and those declarations collide with the real headers (Darwin aliases write(),
 * and <string.h> turns memset into a macro), so it is told to skip them here. */
#include "unit_compat.h"        /* the only header here that includes anything   */
#define MIB3_TOOLKIT_NO_LIBC_DECLS 1
#include "toolkit.h"            /* the on-unit printing primitives               */
#include "mib3clusterstream.h"
#include "../mib3_shared.h"     /* the access-unit frame protocol                */

/* --------------------------------------------------------------- 1. entry */

int mib3_run(int argc, char **arguments) __attribute__((used));

#if defined(MIB3_UNIT_AARCH64)
__attribute__((naked, noreturn)) void _start(void);
#else
int main(int argc, char **arguments);
#endif

/* ------------------------------------------------------------- 2. logging
 * One formatter, two sinks: stderr (log_message) and the supervisor's state
 * file (format_line straight into a buffer). The toolkit supplies the write;
 * a line is assembled first so a multi-field line is ONE write, not fifteen. */

static void append_character(char *out, usize_t capacity, usize_t *used, char character)
{
    if (*used + 1 < capacity) {
        out[(*used)++] = character;
        out[*used] = 0;
    }
}

static void append_text(char *out, usize_t capacity, usize_t *used, const char *text)
{
    if (!text) text = "(null)";
    while (*text) append_character(out, capacity, used, *text++);
}

static void append_unsigned(char *out, usize_t capacity, usize_t *used, unsigned long value, int base)
{
    static const char digits[] = "0123456789abcdef";
    char reversed[24];
    int count = 0;
    if (value == 0) reversed[count++] = '0';
    while (value) {
        reversed[count++] = digits[value % (unsigned long)base];
        value /= (unsigned long)base;
    }
    while (count > 0) append_character(out, capacity, used, reversed[--count]);
}

static void append_signed(char *out, usize_t capacity, usize_t *used, long value)
{
    if (value < 0) {
        append_character(out, capacity, used, '-');
        /* (value + 1) first so the most negative long cannot overflow */
        append_unsigned(out, capacity, used, (unsigned long)(-(value + 1)) + 1ul, 10);
    } else {
        append_unsigned(out, capacity, used, (unsigned long)value, 10);
    }
}

/* %s, %d, %ld, %u, %lu, %x, %lx and %%. Long is 64-bit on both builds, so a u64
 * counter goes through %ld. Unknown conversions are printed literally. */
static usize_t format_arguments(char *out, usize_t capacity, const char *format, va_list arguments)
{
    usize_t used = 0;
    if (!capacity) return 0;
    out[0] = 0;
    for (const char *cursor = format; *cursor; cursor++) {
        if (*cursor != '%') { append_character(out, capacity, &used, *cursor); continue; }
        cursor++;
        if (!*cursor) break;
        int wide = 0;
        while (*cursor == 'l') { wide = 1; cursor++; }
        switch (*cursor) {
        case 's': append_text(out, capacity, &used, va_arg(arguments, const char *)); break;
        case 'd': {
            long value = wide ? va_arg(arguments, long) : (long)va_arg(arguments, int);
            append_signed(out, capacity, &used, value);
            break;
        }
        case 'u': {
            unsigned long value = wide ? va_arg(arguments, unsigned long)
                                       : (unsigned long)va_arg(arguments, unsigned);
            append_unsigned(out, capacity, &used, value, 10);
            break;
        }
        case 'x': {
            unsigned long value = wide ? va_arg(arguments, unsigned long)
                                       : (unsigned long)va_arg(arguments, unsigned);
            append_unsigned(out, capacity, &used, value, 16);
            break;
        }
        case '%': append_character(out, capacity, &used, '%'); break;
        default:
            append_character(out, capacity, &used, '%');
            append_character(out, capacity, &used, *cursor);
            break;
        }
    }
    return used;
}

usize_t format_line(char *out, usize_t capacity, const char *format, ...)
{
    va_list arguments;
    usize_t used;
    va_start(arguments, format);
    used = format_arguments(out, capacity, format, arguments);
    va_end(arguments);
    return used;
}

void log_message(const char *format, ...)
{
    char line[LOG_LINE_BYTES];
    va_list arguments;
    usize_t used;
    va_start(arguments, format);
    used = format_arguments(line, sizeof line, format, arguments);
    va_end(arguments);
    append_character(line, sizeof line, &used, '\n');
    Pe(line);
}

/* ------------------------------------------------------------------ 3. clocks
 * Two clocks on purpose: the release loop, the PTS step, the arrival meter and
 * everything settled before 2026-10-30 use milliseconds; the emitter uses
 * nanoseconds because a datagram deadline is ~3.2 ms out and a millisecond
 * quantisation would be 30% of the spacing. */

static u64 current_time_ms(void)
{
    UnitTime now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)timespec_seconds(now) * 1000u + (u64)timespec_nanoseconds(now) / 1000000u;
}

static u64 current_time_ns(void)
{
    UnitTime now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)timespec_seconds(now) * 1000000000ull + (u64)timespec_nanoseconds(now);
}

/* ------------------------------------------------------------------- 4. rings
 * One implementation for the arrival cadence, the emission cadence, the latency
 * samples and the timing histogram's peak window. */

void ring_reset(Ring *ring)
{
    ring->index = 0;
    ring->count = 0;
}

void ring_push(Ring *ring, u64 value)
{
    ring->values[ring->index] = value;
    ring->index++;
    if (ring->index >= ring->capacity) ring->index = 0;
    if (ring->count < ring->capacity) ring->count++;
}

void ring_percentiles(const Ring *ring, u64 *p50, u64 *p95, u64 *maximum)
{
    u64 sorted[TIMING_RING_CAPACITY];
    int count = ring->count;
    if (count > TIMING_RING_CAPACITY) count = TIMING_RING_CAPACITY;
    if (count <= 0) { *p50 = *p95 = *maximum = 0; return; }
    for (int i = 0; i < count; i++) sorted[i] = ring->values[i];
    for (int i = 1; i < count; i++) {          /* insertion sort: the sets are tiny */
        u64 value = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > value) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = value;
    }
    *p50 = sorted[count / 2];
    *p95 = sorted[(count * 95) / 100];
    *maximum = sorted[count - 1];
}

/* How many of the newest samples fall inside `window` of `newest`. Walks back in
 * time from the newest slot and stops at the first older sample. */
int ring_values_within(const Ring *ring, u64 newest, u64 window)
{
    int count = 0;
    for (int step = 1; step <= ring->count; step++) {
        int position = (ring->index - step + ring->capacity) % ring->capacity;
        if (newest - ring->values[position] <= window) count++;
        else break;
    }
    return count;
}

/* -------------------------------------------------- 5. supervisor's state line
 * One line, rewritten by rename so a reader never sees a blank. Counters rather
 * than a flag because on wireless CarPlay the phone's socket outlives the
 * session: what a session is, is pictures arriving. See
 * docs/development/pathb-supervisor-handover.md. */

static Supervisor g_supervisor;
/* The output settings and the control channel are process-wide and defined here
 * because the session loop and the control path both reach them. */
static OutputSettings g_output;
static ControlChannel g_control;

void supervisor_publish(int force)
{
    char line[STATE_LINE_BYTES];
    char temporary[STATE_PATH_BYTES + 8];
    long written;
    usize_t length;
    int descriptor;

    if (!g_supervisor.path[0]) return;
    if (!force && current_time_ms() - g_supervisor.lastPublishMs < SUPERVISOR_PUBLISH_INTERVAL_MS) return;

    g_supervisor.sequence++;
    length = format_line(line, sizeof line,
                         "conn=%d ready=%d bound=%d au=%ld out=%ld ms=%ld seq=%lu\n",
                         g_supervisor.connection, g_supervisor.ready, g_supervisor.bound,
                         (long)g_supervisor.accessUnitsIn, (long)g_supervisor.accessUnitsOut,
                         (long)current_time_ms(), (unsigned long)g_supervisor.sequence);

    format_line(temporary, sizeof temporary, "%s.tmp", g_supervisor.path);
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor >= 0) {
        written = write(descriptor, line, length);
        close(descriptor);
        if (written == (long)length) rename(temporary, g_supervisor.path);
        else unlink(temporary);
    }
    g_supervisor.lastPublishMs = current_time_ms();
}

void supervisor_set_connection(int connected)
{
    g_supervisor.connection = connected ? 1 : 0;
    if (!connected) g_supervisor.ready = 0;
    supervisor_publish(1);
}

void supervisor_set_ready(int ready)
{
    g_supervisor.ready = ready ? 1 : 0;
}

void supervisor_set_bound(int port)
{
    g_supervisor.bound = port;
}

/* ------------------------------------------------------- 6. emission timing
 * --stats-timing records the monotonic timestamp of every write and reports the
 * distribution at exit. A datagram capture can prove WHAT was sent; it carries
 * no timestamps, so this is the only way to prove WHEN. Off by default. */

static u64 g_timingValues[TIMING_RING_CAPACITY];
static TimingStats g_timing;
static int g_timingEnabled;

void timing_note_write(void)
{
    static const u64 edges[TIMING_HISTOGRAM_BUCKETS - 1] = {
        500000ull, 1000000ull, 2000000ull, 4000000ull, 8000000ull, 16000000ull,
        20000000ull, 40000000ull, 80000000ull, 160000000ull, 320000000ull
    };
    u64 now;
    int bucket = 0;

    if (!g_timingEnabled) return;
    now = current_time_ns();
    if (g_timing.ignore) { g_timing.lastNs = now; return; }
    if (g_timing.lastNs) {
        u64 gap = now - g_timing.lastNs;
        if (gap > g_timing.longestNs) g_timing.longestNs = gap;
        if (gap > TIMING_LONG_GAP_NS) {
            g_timing.gapsOver20Ms++;
            /* Name the long gaps while they happen: on the car this says which
             * datagram the wire went quiet at. Capped so a pathological run
             * cannot flood the log. */
            if (g_timing.gapsOver20Ms <= TIMING_LONG_GAP_REPORT_LIMIT) {
                log_message("timing: idle %ld us before write %ld (gap %ld)",
                            (long)(gap / 1000), (long)g_timing.writes, (long)g_timing.gapsOver20Ms);
            }
        }
        while (bucket < TIMING_HISTOGRAM_BUCKETS - 1 && gap > edges[bucket]) bucket++;
        g_timing.histogram[bucket]++;
    }
    g_timing.lastNs = now;
    g_timing.writes++;

    ring_push(&g_timing.ring, now);
    {   /* peak density: datagrams inside ANY 25 ms window. The unit's own peak
         * is 61, measured on the same interface, so the numbers compare. */
        int density = ring_values_within(&g_timing.ring, now, TIMING_PEAK_WINDOW_NS);
        if ((u64)density > g_timing.peakPer25Ms) g_timing.peakPer25Ms = (u64)density;
    }
}

void timing_report(void)
{
    static const char *labels[TIMING_HISTOGRAM_BUCKETS] = {
        "<0.5ms", "<1ms", "<2ms", "<4ms", "<8ms", "<16ms",
        "<20ms", "<40ms", "<80ms", "<160ms", "<320ms", ">=320ms"
    };
    if (!g_timingEnabled) return;
    log_message("timing: writes=%ld  longest-gap=%ld us  gaps>20ms=%ld  peak-dg/25ms=%ld",
                (long)g_timing.writes, (long)(g_timing.longestNs / 1000),
                (long)g_timing.gapsOver20Ms, (long)g_timing.peakPer25Ms);
    log_message("timing: inter-datagram gap histogram (count per bucket)");
    for (int i = 0; i < TIMING_HISTOGRAM_BUCKETS; i++) {
        if (!g_timing.histogram[i]) continue;
        log_message("  timing:   %s %ld", labels[i], (long)g_timing.histogram[i]);
    }
}

void timing_reset(void)
{
    u64 *values = g_timing.ring.values;
    int capacity = g_timing.ring.capacity;
    memset(&g_timing, 0, sizeof g_timing);
    g_timing.ring.values = values;
    g_timing.ring.capacity = capacity;
    ring_reset(&g_timing.ring);
}

/* ------------------------------------------------------------ 7. wire format
 * Exact bytes for the container the ECU renders. See
 * docs/development/pathb-wire-format.md for the reference binary's symbols and
 * the captures that established each cadence. */

u32 mpeg_crc32(const u8 *data, usize_t length)
{
    u32 crc = 0xFFFFFFFFu;
    for (usize_t i = 0; i < length; i++) {
        crc ^= (u32)data[i] << 24;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
    }
    return crc;
}

void pack_transport_packet(u8 *out, int pid, int unitStart, int continuityCounter,
                           const u8 *payload, int payloadBytes, int wantPcr,
                           u64 pcrTicks, int randomAccess)
{
    int capacity = wantPcr ? PAYLOAD_BYTES_WITH_PCR : PAYLOAD_BYTES_PLAIN;
    int adaptationBytes;

    if (payloadBytes > capacity) payloadBytes = capacity;
    /* 183 has no legal encoding: the plain form needs 184 and the adaptation
     * form has 182 left. Left unhandled it wrote one byte past the packet and
     * silently dropped the last payload byte. */
    if (!wantPcr && !randomAccess && payloadBytes == PAYLOAD_BYTES_UNENCODABLE)
        payloadBytes = PAYLOAD_BYTES_WITH_RAI;

    memset(out, 0xFF, TRANSPORT_PACKET_BYTES);
    out[0] = TRANSPORT_PACKET_SYNC;
    out[1] = (u8)((unitStart ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    out[2] = (u8)(pid & 0xFF);

    adaptationBytes = wantPcr ? 7 : (randomAccess ? 1 : (PAYLOAD_BYTES_UNENCODABLE - payloadBytes));
    if (adaptationBytes < 0) adaptationBytes = 0;

    if (adaptationBytes == 0) {
        out[3] = (u8)(0x10 | (continuityCounter & 0x0F));
        if (payload && payloadBytes) memcpy(out + 4, payload, (usize_t)payloadBytes);
        return;
    }

    out[3] = (u8)(0x30 | (continuityCounter & 0x0F));
    out[4] = (u8)adaptationBytes;
    if (wantPcr) {
        u64 base = pcrTicks & 0x1FFFFFFFFull;
        u32 extension = 0;
        out[5] = (u8)((randomAccess ? ADAPTATION_FLAG_RANDOM_ACCESS : 0) | ADAPTATION_FLAG_PCR);
        out[6] = (u8)(base >> 25);
        out[7] = (u8)(base >> 17);
        out[8] = (u8)(base >> 9);
        out[9] = (u8)(base >> 1);
        out[10] = (u8)(((base & 1) << 7) | 0x7E | (extension >> 8));
        out[11] = (u8)(extension & 0xFF);
        if (payloadBytes > PAYLOAD_BYTES_WITH_PCR) payloadBytes = PAYLOAD_BYTES_WITH_PCR;
    } else if (randomAccess) {
        /* The indicator is the demuxer's "a presentable picture starts here".
         * Without it this ECU paints only the pictures that flush its decoder. */
        out[5] = ADAPTATION_FLAG_RANDOM_ACCESS;
        if (payloadBytes > PAYLOAD_BYTES_WITH_RAI) payloadBytes = PAYLOAD_BYTES_WITH_RAI;
    } else {
        out[5] = 0x00;
    }
    if (payload && payloadBytes) memcpy(out + 5 + adaptationBytes, payload, (usize_t)payloadBytes);
}

void pack_program_association(u8 *out, int transportStreamId, int pmtPid, int continuityCounter)
{
    u8 section[16];
    u8 payload[PAYLOAD_BYTES_PLAIN];
    int count = 0;
    u32 crc;

    section[count++] = 0x00;
    section[count++] = 0xB0; section[count++] = 0x0D;
    section[count++] = (u8)(transportStreamId >> 8); section[count++] = (u8)transportStreamId;
    section[count++] = 0xDB;
    section[count++] = 0x00; section[count++] = 0x00;
    section[count++] = 0x00; section[count++] = 0x01;
    section[count++] = (u8)(0xE0 | ((pmtPid >> 8) & 0x1F));
    section[count++] = (u8)(pmtPid & 0xFF);
    crc = mpeg_crc32(section, (usize_t)count);
    section[count++] = (u8)(crc >> 24); section[count++] = (u8)(crc >> 16);
    section[count++] = (u8)(crc >> 8);  section[count++] = (u8)crc;

    memset(payload, 0xFF, sizeof payload);
    payload[0] = 0x00;                                  /* pointer_field */
    memcpy(payload + 1, section, (usize_t)count);
    pack_transport_packet(out, 0, 1, continuityCounter, payload, PAYLOAD_BYTES_PLAIN, 0, 0, 0);
}

void pack_program_map(u8 *out, int pmtPid, int pcrPid, int elementaryPid, int continuityCounter)
{
    u8 section[32];
    u8 payload[PAYLOAD_BYTES_PLAIN];
    int count = 0;
    u32 crc;

    section[count++] = 0x02;
    section[count++] = 0xB0; section[count++] = 0x12;
    section[count++] = 0x00; section[count++] = 0x01;
    section[count++] = 0xC3;
    section[count++] = 0x00; section[count++] = 0x00;
    section[count++] = (u8)(0xE0 | ((pcrPid >> 8) & 0x1F));
    section[count++] = (u8)(pcrPid & 0xFF);
    section[count++] = 0xF0; section[count++] = 0x00;
    section[count++] = 0x1B;                            /* H.264/AVC */
    section[count++] = (u8)(0xE0 | ((elementaryPid >> 8) & 0x1F));
    section[count++] = (u8)(elementaryPid & 0xFF);
    section[count++] = 0xF0; section[count++] = 0x00;
    crc = mpeg_crc32(section, (usize_t)count);
    section[count++] = (u8)(crc >> 24); section[count++] = (u8)(crc >> 16);
    section[count++] = (u8)(crc >> 8);  section[count++] = (u8)crc;

    memset(payload, 0xFF, sizeof payload);
    payload[0] = 0x00;
    memcpy(payload + 1, section, (usize_t)count);
    pack_transport_packet(out, pmtPid, 1, continuityCounter, payload, PAYLOAD_BYTES_PLAIN, 0, 0, 0);
}

int write_pes_header(u8 *out, u64 presentationTicks)
{
    out[0] = 0x00; out[1] = 0x00; out[2] = 0x01; out[3] = 0xE0;
    out[4] = 0x00; out[5] = 0x00;
    out[6] = 0x80; out[7] = 0x80; out[8] = 0x05;
    out[9]  = (u8)(0x21 | (((presentationTicks >> 30) & 0x07) << 1));
    out[10] = (u8)((presentationTicks >> 22) & 0xFF);
    out[11] = (u8)(((presentationTicks >> 14) & 0xFE) | 0x01);
    out[12] = (u8)((presentationTicks >> 7) & 0xFF);
    out[13] = (u8)(((presentationTicks << 1) & 0xFE) | 0x01);
    return 14;
}

/* -------------------------------------------------------- 8. mux and release
 * The muxer assembles a COMPLETE access unit in one pass at one instant, so the
 * PSI and PCR decisions that depend on the wall clock are made exactly as they
 * always were. The emitter then releases the buffered datagrams one at a time,
 * on byte-proportional deadlines. Nothing sleeps inside the write path. */

void stream_write_datagram(Stream *stream, const u8 *datagram, int bytes)
{
    long written;
    if (stream->outFd < 0) return;
    written = write(stream->outFd, datagram, (usize_t)bytes);
    timing_note_write();
    if (written != (long)bytes) {
        stream->mux.writeFailures++;
        if (stream->mux.writeFailures <= 3 || (stream->mux.writeFailures & 0xFF) == 0)
            log_message("mux: WRITE FAILED (datagram lost) count=%ld", (long)stream->mux.writeFailures);
    }
}

/* Grow the three buffered-datagram arrays. The committed capacity only advances
 * when the last allocation succeeded, so a partial failure cannot leave the
 * capacity ahead of a short array. */
int emitter_reserve(Emitter *emitter)
{
    int newCapacity;
    u8 *bytes;
    u32 *lengths;
    u32 *cumulative;

    if (emitter->count < emitter->capacity) return 1;
    newCapacity = emitter->capacity ? emitter->capacity * 2 : 64;
    if (newCapacity > 2048) return 0;

    bytes = (u8 *)realloc(emitter->bytes, (usize_t)newCapacity * (usize_t)emitter->stride);
    if (!bytes) return 0;
    emitter->bytes = bytes;
    lengths = (u32 *)realloc(emitter->lengths, (usize_t)newCapacity * sizeof(u32));
    if (!lengths) return 0;
    emitter->lengths = lengths;
    cumulative = (u32 *)realloc(emitter->cumulative, (usize_t)newCapacity * sizeof(u32));
    if (!cumulative) return 0;
    emitter->cumulative = cumulative;
    emitter->capacity = newCapacity;
    return 1;
}

void emitter_buffer(Stream *stream, int bytes)
{
    Emitter *emitter = &stream->emitter;

    if (!emitter_reserve(emitter)) {
        /* Never lose a datagram to a memory shortage: send it now and stop
         * pretending to pace this unit. Counted, so a silent degradation cannot
         * masquerade as a paced stream. */
        stream_write_datagram(stream, stream->mux.datagram, bytes);
        emitter->slotMs = 0;
        emitter->statisticsOutOfMemory++;
        return;
    }
    memcpy(emitter->bytes + (usize_t)emitter->count * (usize_t)emitter->stride,
           stream->mux.datagram, (usize_t)bytes);
    emitter->lengths[emitter->count] = (u32)bytes;
    emitter->cumulative[emitter->count] =
        (u32)(emitter->payloadDone > 0xFFFFFFFFull ? 0xFFFFFFFFull : emitter->payloadDone);
    emitter->count++;
}

void mux_flush(Stream *stream)
{
    Mux *mux = &stream->mux;
    u32 timestamp;
    int total;

    if (mux->packetsInDatagram == 0) return;
    total = RTP_HEADER_BYTES + mux->packetsInDatagram * TRANSPORT_PACKET_BYTES;
    timestamp = mux->rtpTimestamp;
    mux->datagram[0] = 0x80;                            /* version 2 */
    mux->datagram[1] = 0x21;                            /* no marker, PT 33 */
    mux->datagram[2] = (u8)(mux->sequence >> 8);
    mux->datagram[3] = (u8)(mux->sequence & 0xFF);
    mux->datagram[4] = (u8)(timestamp >> 24);
    mux->datagram[5] = (u8)(timestamp >> 16);
    mux->datagram[6] = (u8)(timestamp >> 8);
    mux->datagram[7] = (u8)timestamp;
    mux->datagram[8]  = (u8)(mux->ssrc >> 24);
    mux->datagram[9]  = (u8)(mux->ssrc >> 16);
    mux->datagram[10] = (u8)(mux->ssrc >> 8);
    mux->datagram[11] = (u8)mux->ssrc;
    mux->sequence++;
    if (stream->emitter.slotMs) emitter_buffer(stream, total);
    else                        stream_write_datagram(stream, mux->datagram, total);
    mux->packetsInDatagram = 0;
}

void mux_push(Stream *stream, const u8 *packet)
{
    Mux *mux = &stream->mux;
    memcpy(mux->datagram + RTP_HEADER_BYTES + mux->packetsInDatagram * TRANSPORT_PACKET_BYTES,
           packet, TRANSPORT_PACKET_BYTES);
    if (++mux->packetsInDatagram >= mux->packetsPerDatagram) mux_flush(stream);
}

void mux_push_null(Stream *stream)
{
    u8 packet[TRANSPORT_PACKET_BYTES];
    packet[0] = TRANSPORT_PACKET_SYNC;
    packet[1] = 0x1F;                                   /* PID 0x1FFF */
    packet[2] = 0xFF;
    packet[3] = 0x10;                                   /* payload only */
    memset(packet + 4, 0xFF, TRANSPORT_PACKET_BYTES - 4);
    mux_push(stream, packet);
}

/* End every access unit on a datagram boundary. The unit's own sender never
 * straddles units (except at PSI insertions) and pads with null packets; without
 * this, a unit whose packet count was not a multiple of the datagram's left its
 * tail buffered until the next unit arrived. */
void mux_end_access_unit(Stream *stream)
{
    while (stream->mux.packetsInDatagram != 0) mux_push_null(stream);
}

void mux_access_unit(Stream *stream, const u8 *unit, int bytes, u64 presentationTicks)
{
    Mux *mux = &stream->mux;
    u8 packet[TRANSPORT_PACKET_BYTES];
    u8 pes[PAYLOAD_BYTES_PLAIN + 32];
    u64 now = current_time_ms();
    int wantPcr, headerBytes, room, first, offset;

    mux->rtpTimestamp = (u32)presentationTicks;          /* RFC 2250: media time */

    if (mux->programAssociationLastMs == 0 || now - mux->programAssociationLastMs >= PSI_REPEAT_INTERVAL_MS) {
        pack_program_association(packet, mux->transportStreamId, mux->pmtPid, mux->continuityPat++ & 0x0F);
        mux_push(stream, packet);
        pack_program_map(packet, mux->pmtPid, mux->pcrPid, mux->elementaryPid, mux->continuityPmt++ & 0x0F);
        mux_push(stream, packet);
        mux->programAssociationLastMs = now;
    }

    headerBytes = write_pes_header(pes, presentationTicks);
    wantPcr = (mux->programClockLastMs == 0 || now - mux->programClockLastMs >= PCR_REPEAT_INTERVAL_MS);
    room = (wantPcr ? PAYLOAD_BYTES_WITH_PCR : PAYLOAD_BYTES_WITH_RAI) - headerBytes;
    first = bytes < room ? bytes : room;
    memcpy(pes + headerBytes, unit, (usize_t)first);
    pack_transport_packet(packet, mux->elementaryPid, 1, mux->continuityEs++ & 0x0F,
                          pes, headerBytes + first, wantPcr, presentationTicks, 1);
    if (wantPcr) mux->programClockLastMs = now;

    /* Payload bytes are counted BEFORE the push that may close a datagram, so a
     * datagram records the unit bytes it actually carried and the emitter's
     * deadline is proportional to real progress rather than to a guess. PSI
     * packets contribute nothing (they are container, not payload). */
    stream->emitter.payloadDone += (u64)(unsigned)first;
    mux_push(stream, packet);

    offset = first;
    while (offset < bytes) {
        int chunk = bytes - offset;
        if (chunk > PAYLOAD_BYTES_PLAIN) chunk = PAYLOAD_BYTES_PLAIN;
        if (chunk == PAYLOAD_BYTES_UNENCODABLE) chunk = PAYLOAD_BYTES_WITH_RAI;
        pack_transport_packet(packet, mux->elementaryPid, 0, mux->continuityEs++ & 0x0F,
                              unit + offset, chunk, 0, 0, 0);
        stream->emitter.payloadDone += (u64)(unsigned)chunk;
        mux_push(stream, packet);
        offset += chunk;
    }
    mux_end_access_unit(stream);
}

/* ------------------------------------------------------- 9. release deadlines
 * deadline(i) = slotStart + slotMs * (payload bytes through datagram i / unit
 * size), at the MIDPOINT of the datagram's byte range. Using the end pushed
 * every unit's tail to the last microsecond of its slot and raced the next arm. */

u64 emitter_deadline_ns(const Emitter *emitter, int index)
{
    u64 total = emitter->payloadBytes ? emitter->payloadBytes : 1;
    u64 low = (index > 0) ? emitter->cumulative[index - 1] : 0;
    u64 high = emitter->cumulative[index];
    u64 middle;

    if (high > total) high = total;
    if (low > high) low = high;
    middle = low + (high - low) / 2;
    return emitter->slotStartNs + (emitter->slotMs * 1000000ull * middle) / total;
}

int emitter_release_due(Stream *stream, u64 nowNs)
{
    Emitter *emitter = &stream->emitter;
    int released = 0;

    if (emitter->cursor >= emitter->count) {
        emitter->count = emitter->cursor = 0;
        emitter->slotMs = 0;
        return 0;
    }
    /* DUE FIRST: a datagram whose deadline has arrived is normal traffic even if
     * that deadline is the last microsecond of the slot. */
    while (emitter->cursor < emitter->count && released < EMITTER_BURST_DATAGRAMS) {
        if (nowNs < emitter_deadline_ns(emitter, emitter->cursor)) break;
        stream_write_datagram(stream,
                              emitter->bytes + (usize_t)emitter->cursor * (usize_t)emitter->stride,
                              (int)emitter->lengths[emitter->cursor]);
        emitter->cursor++;
        released++;
    }
    /* OVERRUN EXTENDS THE SLOT, IT NEVER DUMPS. Collapsing the remainder to
     * immediate emission re-created exactly the clump that causes the artifacts:
     * a 192 KB frame went out as 169 datagrams inside one 25 ms window. */
    if (emitter->cursor < emitter->count && emitter->slotMs &&
        nowNs > emitter->slotStartNs + emitter->slotMs * 1000000ull) {
        u64 total, done, remaining, extra;
        if (!emitter->overrunCounted) { emitter->statisticsOverruns++; emitter->overrunCounted = 1; }
        total = emitter->payloadBytes ? emitter->payloadBytes : 1;
        done = (emitter->cursor > 0) ? emitter->cumulative[emitter->cursor - 1] : 0;
        remaining = (done < total) ? (total - done) : 0;
        extra = (remaining * emitter->slotMs) / total;
        if (!extra) extra = 1;
        emitter->slotStartNs = nowNs;                   /* same rate, more time */
        emitter->slotMs = extra;
    }
    if (emitter->cursor >= emitter->count) {
        emitter->count = emitter->cursor = 0;
        emitter->slotMs = 0;
        emitter->overrunCounted = 0;
        return 0;
    }
    return 1;
}

/* Push the remainder out now. Used at connection close and when a new unit is
 * armed while the previous one is still in flight (only when the release clock
 * has fallen behind). Unit order and boundaries are preserved. */
void emitter_release_all(Stream *stream, int counted)
{
    Emitter *emitter = &stream->emitter;
    if (emitter->cursor >= emitter->count) {
        emitter->count = emitter->cursor = 0;
        emitter->slotMs = 0;
        return;
    }
    if (counted) emitter->statisticsOverruns++;
    while (emitter->cursor < emitter->count) {
        stream_write_datagram(stream,
                              emitter->bytes + (usize_t)emitter->cursor * (usize_t)emitter->stride,
                              (int)emitter->lengths[emitter->cursor]);
        emitter->cursor++;
    }
    emitter->count = emitter->cursor = 0;
    emitter->slotMs = 0;
    emitter->overrunCounted = 0;
}

u64 emitter_poll_timeout_ns(Stream *stream)
{
    Emitter *emitter = &stream->emitter;
    u64 now, due;
    if (emitter->cursor >= emitter->count) return EMITTER_POLL_CAP_NS;
    now = current_time_ns();
    if (emitter->slotMs && now > emitter->slotStartNs + emitter->slotMs * 1000000ull) return 0;
    due = emitter_deadline_ns(emitter, emitter->cursor);
    if (due <= now) return 0;
    return (due - now) < EMITTER_POLL_CAP_NS ? (due - now) : EMITTER_POLL_CAP_NS;
}

/* ------------------------------------------------- 10. the access-unit walk
 * One pass over a unit answers everything: is it a parameter-set record, does it
 * carry VCL, is there an IDR, where does the last SPS start. The previous source
 * walked every unit three times, three different ways, to answer the same
 * questions. */

int nal_next(const u8 *data, usize_t length, usize_t *cursor, NalUnit *unit)
{
    usize_t index = *cursor;
    while (index + 3 <= length) {
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
            usize_t start = index;
            usize_t header = index + 3;
            while (start > 0 && data[start - 1] == 0) start--;
            if (header >= length) break;
            unit->startCode = start;
            unit->type = data[header] & 0x1F;
            unit->firstPayloadByte = (header + 1 < length) ? (int)data[header + 1] : -1;
            *cursor = header;
            return 1;
        }
        index++;
    }
    *cursor = length;
    return 0;
}

void access_unit_inspect(const u8 *data, usize_t length, AccessUnitInfo *info)
{
    usize_t cursor = 0;
    NalUnit unit;

    memset(info, 0, sizeof *info);
    info->lastSpsStart = length;
    while (nal_next(data, length, &cursor, &unit)) {
        switch (unit.type) {
        case 1: info->hasVcl = 1; break;
        case 5: info->hasVcl = 1; info->hasIdr = 1; break;
        case 7: info->hasSps = 1; info->lastSpsStart = unit.startCode; break;
        case 8: info->hasPps = 1; break;
        default: break;
        }
    }
}

/* ------------------------------------- 11. parameter-set folding (socket input)
 * The shim stamps the sequence header ahead of every unit as its own record. A
 * record-per-PES muxer would therefore emit two PES per frame, and every stream
 * this ECU decodes carries the parameter sets inside the same unit as the
 * slice. A VCL-free record is held and prepended to the next record that
 * carries VCL. See docs/development/pathb-pacing-and-timing.md. */

static int parameter_set_put(ParameterSet *set, const u8 *data, usize_t length)
{
    if (set->length + length > PARAMETER_SET_CAP_BYTES) {
        set->length = 0;
        set->hasSps = set->hasPps = 0;
    }
    if (set->length + length > set->capacity) {
        usize_t newCapacity = (set->length + length) * 2;
        u8 *bytes = (u8 *)realloc(set->bytes, newCapacity);
        if (!bytes) return -1;
        set->bytes = bytes;
        set->capacity = newCapacity;
    }
    memcpy(set->bytes + set->length, data, length);
    set->length += length;
    return 0;
}

int parameter_set_absorb(ParameterSet *set, const u8 *record, usize_t length,
                         const AccessUnitInfo *info, int *changed)
{
    if (info->hasVcl) return 0;                          /* the caller emits it */
    if (info->hasSps && info->lastSpsStart > 0 && info->lastSpsStart < length) {
        /* Keep exactly ONE sequence header: if the record carries several
         * SPS/PPS pairs the active one is the last. */
        record += info->lastSpsStart;
        length -= info->lastSpsStart;
    }
    /* Compare BEFORE the supersede block clears the buffer: doing it afterwards
     * makes every repeat look changed, which silently restores header-on-every-
     * frame. */
    *changed = !(set->length == length && length > 0 && memcmp(set->bytes, record, length) == 0);
    if (info->hasSps || info->hasPps) {
        if (set->length && ((info->hasSps && set->hasSps) || (info->hasPps && set->hasPps))) {
            set->length = 0;
            set->hasSps = set->hasPps = 0;
        }
    }
    if (parameter_set_put(set, record, length) < 0) { *changed = 0; return 1; }
    if (info->hasSps) set->hasSps = 1;
    if (info->hasPps) set->hasPps = 1;
    return 1;
}

void parameter_set_reset(ParameterSet *set)
{
    set->length = 0;
    set->hasSps = set->hasPps = 0;
}

/* ------------------------------------------------ 12. splitting (file input) */

int is_new_access_unit(int nalType, int haveSlice)
{
    if (nalType == 9) return 1;                          /* access unit delimiter */
    if (!haveSlice) return 0;
    if (nalType == 7 || nalType == 8 || nalType == 6) return 1;
    if (nalType == 1 || nalType == 5) return 1;
    return 0;
}

int split_access_units(const u8 *data, usize_t length, const u8 **units, int *lengths, int maximum)
{
    usize_t cursor = 0;
    usize_t unitStart = 0;
    NalUnit unit;
    int count = 0;
    int haveSlice = 0, started = 0;

    while (nal_next(data, length, &cursor, &unit)) {
        if (!started) {
            unitStart = unit.startCode;
            started = 1;
        } else if (is_new_access_unit(unit.type, haveSlice)) {
            usize_t size = unit.startCode - unitStart;
            if (size && count < maximum) {
                units[count] = data + unitStart;
                lengths[count] = (int)size;
                count++;
            }
            unitStart = unit.startCode;
            haveSlice = 0;
        }
        if ((unit.type == 1 || unit.type == 5) && unit.firstPayloadByte >= 0 &&
            (unit.firstPayloadByte & 0x80)) haveSlice = 1;
    }
    if (started && unitStart < length && count < maximum) {
        units[count] = data + unitStart;
        lengths[count] = (int)(length - unitStart);
        count++;
    }
    return count;
}

u8 *avcc_to_annexb(const u8 *data, usize_t length, usize_t *outLength)
{
    usize_t capacity = length + 1024, used = 0, index = 0;
    u8 *bytes = (u8 *)malloc(capacity);
    if (!bytes) return (u8 *)0;
    while (index + 4 <= length) {
        u32 nalLength = ((u32)data[index] << 24) | ((u32)data[index + 1] << 16) |
                        ((u32)data[index + 2] << 8) | data[index + 3];
        index += 4;
        if (nalLength == 0 || index + nalLength > length) break;
        if (used + 4 + nalLength > capacity) {
            u8 *grown;
            capacity = (used + 4 + nalLength) * 2;
            grown = (u8 *)realloc(bytes, capacity);
            if (!grown) { free(bytes); return (u8 *)0; }
            bytes = grown;
        }
        bytes[used++] = 0; bytes[used++] = 0; bytes[used++] = 0; bytes[used++] = 1;
        memcpy(bytes + used, data + index, nalLength);
        used += nalLength;
        index += nalLength;
    }
    *outLength = used;
    return bytes;
}

int read_all(int descriptor, u8 **buffer, usize_t *length)
{
    usize_t capacity = 1 << 20, used = 0;
    u8 *bytes = (u8 *)malloc(capacity);
    if (!bytes) return -1;
    for (;;) {
        long received;
        if (used + 65536 > capacity) {
            u8 *grown;
            capacity *= 2;
            grown = (u8 *)realloc(bytes, capacity);
            if (!grown) { free(bytes); return -1; }
            bytes = grown;
        }
        received = read(descriptor, bytes + used, 65536);
        if (received < 0) { free(bytes); return -1; }
        if (received == 0) break;
        used += (usize_t)received;
    }
    *buffer = bytes;
    *length = used;
    return 0;
}

/* -------------------------------------------------------- 13. the media clock
 * A steady clock, not a wall clock: timestamps advance by a bounded constant
 * step per unit, slewed against wall time, so a phone silence is never a PCR
 * discontinuity. See docs/development/pathb-pacing-and-timing.md. */

static u32 g_spreadPercent = DEFAULT_SPREAD_PERCENT;

void session_publish_access_unit(Session *session, const u8 *unit, usize_t length)
{
    Stream *stream = &session->stream;
    MediaClock *clock = &session->clock;
    u64 step, presentationTicks;

    {   /* emission-rhythm sample: called once per unit from every path, so it
         * sees what actually left. */
        u64 now = current_time_ms();
        if (session->cadence.lastEmissionMs && now >= session->cadence.lastEmissionMs &&
            now - session->cadence.lastEmissionMs <= 1000)
            ring_push(&session->cadence.emission, now - session->cadence.lastEmissionMs);
        session->cadence.lastEmissionMs = now;
    }
    g_supervisor.accessUnitsOut++;

    step = clock->stepTicks;
    if (step < MEDIA_STEP_FLOOR_TICKS) step = MEDIA_STEP_FLOOR_TICKS;
    if (step > MEDIA_STEP_CEILING_TICKS) step = MEDIA_STEP_CEILING_TICKS;

    /* SLEW, DON'T JUMP: correct the integrated error by at most a tenth per unit,
     * and re-anchor instead of racing when the error is a pause, not drift. */
    if (!clock->wallBaseMs) {
        clock->wallBaseMs = current_time_ms();
        clock->ptsBaseTicks = stream->mux.presentationTimeTicks;
    } else {
        u64 wall = current_time_ms() - clock->wallBaseMs;
        u64 media = (stream->mux.presentationTimeTicks - clock->ptsBaseTicks) / TICKS_PER_MILLISECOND;
        long error = (long)wall - (long)media;
        if (error > MEDIA_DRIFT_REBASE_MS || error < -MEDIA_DRIFT_REBASE_MS) {
            clock->wallBaseMs = current_time_ms();
            clock->ptsBaseTicks = stream->mux.presentationTimeTicks;
        } else if (error > MEDIA_DRIFT_DEADBAND_MS || error < -MEDIA_DRIFT_DEADBAND_MS) {
            long adjustment = (long)(step / MEDIA_SLEW_DIVISOR);
            long correction = (error > 0) ? adjustment : -adjustment;
            if (correction > 0 && (u64)correction > step - MEDIA_STEP_FLOOR_TICKS)
                correction = (long)(step - MEDIA_STEP_FLOOR_TICKS);
            if (correction < 0 && (u64)(-correction) > MEDIA_STEP_CEILING_TICKS - step)
                correction = -(long)(MEDIA_STEP_CEILING_TICKS - step);
            step = (u64)((long)step + correction);
        }
    }
    stream->mux.presentationTimeTicks += step;
    presentationTicks = stream->mux.presentationTimeTicks;

    /* One unit in flight, always: if the previous release is still running the
     * clock has fallen behind, so push its remainder out and count it. */
    if (!clock->slotMs) stream->emitter.statisticsSkipped++;
    if (stream->emitter.count) {
        emitter_release_due(stream, current_time_ns());
        if (stream->emitter.count) emitter_release_all(stream, 1);
    }
    stream->emitter.count = stream->emitter.cursor = 0;
    stream->emitter.overrunCounted = 0;
    stream->emitter.slotStartNs = current_time_ns();

    /* The spread budget is a fraction of the interval, so a unit finishes before
     * the next is due and there is a cushion for a scheduler hitch. */
    stream->emitter.slotMs = clock->slotMs ? (clock->slotMs * g_spreadPercent) / 100u : 0;
    if (clock->slotMs && stream->emitter.slotMs < 1) stream->emitter.slotMs = 1;

    /* Rate ceiling: never faster than the reference sender, whatever the clock
     * wants. This is what makes a late unit SPREAD instead of CLUMP. */
    {
        u64 floorMs = (u64)length / OUTPUT_RATE_CEILING_BYTES_PER_MS;
        if (floorMs < 1) floorMs = 1;
        if (stream->emitter.slotMs < floorMs) stream->emitter.slotMs = floorMs;
    }
    stream->emitter.payloadBytes = (u64)length;
    stream->emitter.payloadDone = 0;

    mux_access_unit(stream, unit, (int)length, presentationTicks);
    clock->frames++;
}

/* ----------------------------------------------------------- 14. the session
 * The queue is the buffer, the adopted rate is the clock, and the depth
 * controller holds depth by nudging the rate. A correction only fires when the
 * queue is more than 20% off target: near the target its noise was being
 * multiplied straight into the interval, and the interval is what the eye reads
 * as judder. */

static u64 g_cadenceArrivalValues[CADENCE_RING_CAPACITY];
static u64 g_cadenceEmissionValues[CADENCE_RING_CAPACITY];
static u64 g_latencyValues[LATENCY_RING_CAPACITY];

void session_initialise(Session *session, Options *options)
{
    memset(session, 0, sizeof *session);
    session->clock.stream = &session->stream;
    session->jitter.minimum = 2;
    session->jitter.maximum = 10;
    session->jitterMs = options->jitterMs;
    session->noFold = options->noFold;
    session->noIdrGate = options->noIdrGate;
    session->cadence.arrival.values = g_cadenceArrivalValues;
    session->cadence.arrival.capacity = CADENCE_RING_CAPACITY;
    session->cadence.emission.values = g_cadenceEmissionValues;
    session->cadence.emission.capacity = CADENCE_RING_CAPACITY;
    session->latency.samples.values = g_latencyValues;
    session->latency.samples.capacity = LATENCY_RING_CAPACITY;
}

static void queue_clear(Session *session)
{
    session->queue.head = 0;
    session->queue.count = 0;
}

static void queue_release(Session *session)
{
    for (int slot = 0; slot < QUEUE_CAPACITY; slot++) {
        free(session->queue.bytes[slot]);
        session->queue.bytes[slot] = 0;
        session->queue.capacity[slot] = 0;
        session->queue.length[slot] = 0;
    }
    queue_clear(session);
}

void session_reset(Session *session)
{
    queue_clear(session);
    parameter_set_reset(&session->parameters);
    session->stream.mux.programAssociationLastMs = 0;
    session->headerPending = 1;
    session->streaming = 0;
    session->nextEmitMs = 0;
    session->lastArrivalMs = 0;
    session->arrivalGaps = 0;
    session->rateWindowStartMs = 0;
    session->rateWindowCount = 0;
    session->cadenceReportMs = 0;

    session->jitter.rateMs = 0;
    session->jitter.target = 0;
    session->jitter.bandWindows = 0;
    session->jitter.started = 0;
    session->jitter.connectedAtMs = 0;
    session->jitter.starvedAtMs = 0;
    session->jitter.lastGrowMs = 0;
    session->jitter.lastShrinkMs = 0;
    session->jitter.starvations = 0;
    session->jitter.depthDrains = 0;

    ring_reset(&session->cadence.arrival);
    ring_reset(&session->cadence.emission);
    session->cadence.lastEmissionMs = 0;
    ring_reset(&session->latency.samples);
    session->latency.maximum = 0;
    session->latency.lastReportMs = 0;

    session->previousOverruns = 0;
    session->previousSkipped = 0;
    session->previousDeferred = 0;
    session->previousAccessUnitsIn = 0;
    session->previousAccessUnitsOut = 0;
}

/* The release clock. Releasing at the adopted rate holds whatever depth exists,
 * so depth is maintained by nudging the rate; the growth and shrink of the
 * target respond to evidence, not to a timer. */
void session_release_tick(Session *session)
{
    Stream *stream = &session->stream;
    Emitter *emitter = &stream->emitter;
    JitterBuffer *jitter = &session->jitter;
    u64 now = current_time_ms();
    u64 interval;

    /* ---- arrival rate meter: count arrivals over a window, never average gaps.
     * A mean of gaps is dominated by the many short ones and ignores the holes;
     * measured 65 ms while the stream sustained 74.6 ms. */
    if (session->rateWindowCount && session->rateWindowStartMs) {
        u64 window = now - session->rateWindowStartMs;
        if (window >= CLOCK_RATE_WINDOW_MS) {
            u64 measured = window / session->rateWindowCount;
            if (!jitter->rateMs) {
                jitter->rateMs = measured;
            } else {
                u64 low = jitter->rateMs - jitter->rateMs / CLOCK_HYSTERESIS_PERCENT;
                u64 high = jitter->rateMs + jitter->rateMs / CLOCK_HYSTERESIS_PERCENT;
                if (measured >= low && measured <= high) {
                    jitter->bandWindows = 0;
                } else if (++jitter->bandWindows >= CLOCK_BAND_WINDOWS) {
                    jitter->rateMs = (jitter->rateMs + measured) / 2;
                    jitter->bandWindows = 0;
                }
            }
            if (jitter->rateMs < MINIMUM_INTERVAL_MS) jitter->rateMs = MINIMUM_INTERVAL_MS;
            if (jitter->rateMs > MAXIMUM_INTERVAL_MS) jitter->rateMs = MAXIMUM_INTERVAL_MS;
            session->rateWindowStartMs = now;
            session->rateWindowCount = 0;
        }
    }

    /* Seed the target from the gap estimate only: the first units arrive before a
     * rate window has closed, and grow/shrink corrects it within seconds. */
    if (!jitter->target) {
        u64 estimate = session->clock.sensorTicks
                     ? session->clock.sensorTicks / TICKS_PER_MILLISECOND
                     : FALLBACK_INTERVAL_MS;
        if (estimate < MINIMUM_INTERVAL_MS) estimate = MINIMUM_INTERVAL_MS;
        if (estimate > MAXIMUM_INTERVAL_MS) estimate = MAXIMUM_INTERVAL_MS;
        jitter->target = (int)((u64)(session->jitterMs ? session->jitterMs : 250) / estimate);
    }
    if (jitter->target < 2) jitter->target = 2;
    if (jitter->target > jitter->maximum) jitter->target = jitter->maximum;

    interval = jitter->rateMs ? jitter->rateMs : FALLBACK_INTERVAL_MS;
    if (jitter->target > 0) {
        u64 target = (u64)jitter->target;
        u64 band = target * DEPTH_BAND_PERCENT / 100u;
        u64 highBand = target + band;
        u64 lowBand = target > band ? target - band : 1;
        if ((u64)session->queue.count > highBand) {
            u64 scaled = (jitter->rateMs ? jitter->rateMs : FALLBACK_INTERVAL_MS)
                       * target / (u64)session->queue.count;
            if (scaled < 2) scaled = 2;
            interval = scaled;
        } else if ((u64)session->queue.count < lowBand) {
            interval = interval + interval / DEPTH_RATE_ADJUST_DIVISOR + 1;
        }
    }
    if (session->queue.count > 0 &&
        (now - session->queue.arrivalMs[session->queue.head]) > LATENCY_CAP_MS)
        interval = 2;

    if (!session->streaming) { jitter->started = 0; jitter->connectedAtMs = 0; }
    if (session->streaming && !jitter->connectedAtMs) jitter->connectedAtMs = now;
    if (!session->nextEmitMs) session->nextEmitMs = now;

    /* PRE-ROLL: fill to the target before the first emit, bounded in wall time so
     * a slow-starting phone cannot hold the picture off the screen for ever. */
    if (session->jitterMs && session->queue.count > 0 && !jitter->started &&
        session->queue.count >= jitter->target) {
        jitter->started = 1;
        log_message("jit: pre-roll reached target %d AU(s) ~%ld ms, interval %ld ms",
                    jitter->target,
                    (long)((jitter->rateMs ? jitter->rateMs : FALLBACK_INTERVAL_MS) * (u64)jitter->target),
                    (long)(jitter->rateMs ? jitter->rateMs : FALLBACK_INTERVAL_MS));
    }
    if (!session->jitterMs) jitter->started = 1;

    if (now - session->latency.lastReportMs >= LATENCY_REPORT_INTERVAL_MS &&
        session->latency.samples.count) {
        u64 p50, p95, maximum;
        ring_percentiles(&session->latency.samples, &p50, &p95, &maximum);
        session->latency.lastReportMs = now;
        log_message("lat: ours p50=%ld p95=%ld max=%ld ms  qn=%d tgt=%d iv=%ld ms",
                    (long)p50, (long)p95, (long)maximum,
                    session->queue.count, jitter->target,
                    (long)(jitter->rateMs ? jitter->rateMs : FALLBACK_INTERVAL_MS));
    }

    /* ---- cadence window: what rhythm are we delivering, and what did it cost */
    if (now - session->cadenceReportMs >= CADENCE_REPORT_INTERVAL_MS) {
        u64 arrival50, arrival95, arrivalMax, emission50, emission95, emissionMax;
        u64 overrunDelta = emitter->statisticsOverruns - session->previousOverruns;
        u64 skipDelta = emitter->statisticsSkipped - session->previousSkipped;
        u64 deferDelta = emitter->statisticsDeferredArms - session->previousDeferred;
        u64 inDelta = g_supervisor.accessUnitsIn - session->previousAccessUnitsIn;
        u64 outDelta = g_supervisor.accessUnitsOut - session->previousAccessUnitsOut;

        ring_percentiles(&session->cadence.arrival, &arrival50, &arrival95, &arrivalMax);
        ring_percentiles(&session->cadence.emission, &emission50, &emission95, &emissionMax);
        log_message("cad: arr p50=%ld p95=%ld max=%ld | emi p50=%ld p95=%ld max=%ld"
                    " | au=%ld out=%ld seqgap=%ld lost=%ld ovr=%ld skip=%ld defer=%ld"
                    " | qn=%d tgt=%d iv=%ld ms",
                    (long)arrival50, (long)arrival95, (long)arrivalMax,
                    (long)emission50, (long)emission95, (long)emissionMax,
                    (long)inDelta, (long)outDelta,
                    (long)session->cadence.sequenceGaps, (long)session->cadence.lostFrames,
                    (long)overrunDelta, (long)skipDelta, (long)deferDelta,
                    session->queue.count, jitter->target,
                    (long)(jitter->rateMs ? jitter->rateMs : FALLBACK_INTERVAL_MS));
        session->previousOverruns = emitter->statisticsOverruns;
        session->previousSkipped = emitter->statisticsSkipped;
        session->previousDeferred = emitter->statisticsDeferredArms;
        session->previousAccessUnitsIn = g_supervisor.accessUnitsIn;
        session->previousAccessUnitsOut = g_supervisor.accessUnitsOut;
        session->cadenceReportMs = now;
        ring_reset(&session->cadence.arrival);
        ring_reset(&session->cadence.emission);
    }

    if (session->jitterMs && !jitter->started && jitter->connectedAtMs &&
        now - jitter->connectedAtMs >= (u64)session->jitterMs) {
        jitter->started = 1;
        log_message("jit: pre-roll TIMED OUT with %d of %d AU(s)",
                    session->queue.count, jitter->target);
    }

    if (session->streaming && jitter->started && session->queue.count > 0 &&
        now >= session->nextEmitMs) {
        if (emitter->count) {
            emitter_release_due(stream, current_time_ns());
            if (emitter->count) {
                emitter->statisticsDeferredArms++;
                session->nextEmitMs = now + (interval > 2 ? interval / 2 : 1);
            }
        }
        if (!emitter->count) {
            /* CATCH-UP MEANS "FASTER", NOT "UNSPACED": emitting a whole unit at
             * once because the queue is over target is what produced the clump. */
            session->clock.slotMs = !session->jitterMs ? 0
                : ((jitter->target && session->queue.count > jitter->target + 3)
                       ? (interval > 2 ? interval / 2 : interval)
                       : interval);
            {   /* Latency this unit spent in OUR buffer: the share of "it takes a
                 * second to react" that belongs to us. */
                u64 latency = now - session->queue.arrivalMs[session->queue.head];
                ring_push(&session->latency.samples, latency);
                if (latency > session->latency.maximum) session->latency.maximum = latency;
            }
            session_publish_access_unit(session,
                                        session->queue.bytes[session->queue.head],
                                        session->queue.length[session->queue.head]);
            session->clock.slotMs = 0;
            session->queue.head = (session->queue.head + 1) % QUEUE_CAPACITY;
            session->queue.count--;
            session->nextEmitMs = now + interval;
        }
    } else if (session->streaming && jitter->started && session->queue.count == 0 &&
               now >= session->nextEmitMs) {
        /* A hole the buffer could not cover is the only honest evidence that it
         * is too shallow. */
        jitter->starvations++;
        jitter->starvedAtMs = now;
        session->nextEmitMs = now + interval;
        if (now - jitter->lastGrowMs > JITTER_GROW_INTERVAL_MS && jitter->target < jitter->maximum) {
            jitter->target++;
            jitter->lastGrowMs = now;
            log_message("jit: grow -> %d AU(s) after a starved tick (total %ld)",
                        jitter->target, (long)jitter->starvations);
        }
    } else if (session->streaming && jitter->started && jitter->target > jitter->minimum &&
               now - jitter->starvedAtMs > JITTER_STARVE_SETTLE_MS &&
               now - jitter->lastShrinkMs > JITTER_SHRINK_INTERVAL_MS) {
        /* ...and give the latency back once the phone behaves. */
        jitter->target--;
        jitter->lastShrinkMs = now;
        log_message("jit: shrink -> %d AU(s) after 4 s without starvation", jitter->target);
    }

    /* DEPTH BOUND: drain ahead of the clock rather than let the queue walk up
     * towards the ceiling, which would be seconds of latency. Still spread. */
    if (session->queue.count > QUEUE_DRAIN_ABOVE) {
        session->clock.slotMs = !session->jitterMs ? 0 : (interval > 2 ? interval / 2 : interval);
        while (session->queue.count > QUEUE_DRAIN_ABOVE) {
            session_publish_access_unit(session,
                                        session->queue.bytes[session->queue.head],
                                        session->queue.length[session->queue.head]);
            session->queue.head = (session->queue.head + 1) % QUEUE_CAPACITY;
            session->queue.count--;
            jitter->depthDrains++;
        }
        session->clock.slotMs = 0;
    }
}

static long socket_readable_bytes(int descriptor)
{
    int available = 0;
    if (ioctl(descriptor, (unsigned long)FIONREAD, &available) < 0) return -1;
    return (long)available;
}

/* One record from the shim. Returns 0 when the connection is finished (EOF, a
 * short read, a malformed frame) and 1 to continue. */
int session_read_record(Session *session, int descriptor)
{
    Stream *stream = &session->stream;
    u8 header[MIB3_HELP_HDRLEN];
    u32 magic, length;
    u64 sequence;
    long received = 0;
    usize_t got = 0;
    AccessUnitInfo info;
    usize_t parameterBytes = 0;
    usize_t total;

    /* A stream socket may split the header across reads; a short read is not a
     * dead connection. */
    while (got < MIB3_HELP_HDRLEN) {
        received = read(descriptor, header + got, MIB3_HELP_HDRLEN - got);
        if (received > 0) { got += (usize_t)received; continue; }
        break;
    }
    if (got == 0 && received < 0) return 1;              /* spurious wakeup */
    if (got != MIB3_HELP_HDRLEN) {
        if (got) log_message("short header -- connection lost");
        return 0;
    }
    memcpy(&magic, header, 4);
    memcpy(&length, header + 4, 4);
    memcpy(&sequence, header + 8, 8);

    /* A jump of more than one is a picture the shim sent that never arrived here:
     * the only hop in the chain with no counter on the far side. */
    if (sequence) {
        if (session->cadence.lastSequence && sequence > session->cadence.lastSequence + 1) {
            session->cadence.sequenceGaps++;
            session->cadence.lostFrames += sequence - session->cadence.lastSequence - 1;
        }
        session->cadence.lastSequence = sequence;
    }
    if (magic != MIB3_HELP_MAGIC || length > MIB3_HELP_MAXLEN) {
        log_message("bad helper frame");
        return 0;
    }

    if ((usize_t)length > session->recordCapacity) {
        u8 *grown = (u8 *)realloc(session->record, (usize_t)length);
        if (!grown) { log_message("oom"); return 0; }
        session->record = grown;
        session->recordCapacity = (usize_t)length;
    }
    got = 0;
    while (got < (usize_t)length) {
        long chunk = read(descriptor, session->record + got, (usize_t)length - got);
        if (chunk <= 0) break;
        got += (usize_t)chunk;
    }
    if (got != (usize_t)length) {
        log_message("short payload -- connection lost");
        return 0;
    }

    /* Arrival accounting: the evidence the pacer acts on, and the rhythm nothing
     * used to measure. A one-second cap keeps a genuine pause out of the
     * distribution -- a silence is not a frame interval. */
    {
        u64 now = current_time_ms();
        if (session->lastArrivalMs && now - session->lastArrivalMs > ARRIVAL_GAP_THRESHOLD_MS)
            session->arrivalGaps++;
        if (session->lastArrivalMs) {
            u64 delta = now - session->lastArrivalMs;
            if (delta <= ARRIVAL_INTERVAL_MAX_MS)
                ring_push(&session->cadence.arrival, delta);
            /* Calibrate the PTS step from the phone's own cadence, but COMMIT it
             * at most once a second: committing per frame is the breathing clock
             * measured on the wire. */
            if (delta >= ARRIVAL_INTERVAL_MIN_MS) {
                u64 ticks = delta * TICKS_PER_MILLISECOND;
                session->clock.sensorTicks = session->clock.sensorTicks
                    ? (session->clock.sensorTicks * STEP_SENSOR_WEIGHT + ticks) / STEP_SENSOR_DIVISOR
                    : ticks;
            }
            if ((!session->clock.stepTicks || !session->clock.sensorTicks) &&
                session->clock.sensorTicks) {
                session->clock.stepTicks = session->clock.sensorTicks;
                session->clock.stepCommittedMs = now;
            } else if (session->clock.sensorTicks &&
                       now - session->clock.stepCommittedMs >= STEP_COMMIT_INTERVAL_MS) {
                session->clock.stepTicks = session->clock.sensorTicks;
                session->clock.stepCommittedMs = now;
            }
        }
        session->lastArrivalMs = now;
    }

    access_unit_inspect(session->record, (usize_t)length, &info);

    /* Fold a VCL-free record into the next frame. */
    if (!session->noFold) {
        int changed = 0;
        if (parameter_set_absorb(&session->parameters, session->record, (usize_t)length,
                                 &info, &changed)) {
            if (changed) session->headerPending = 1;
            return 1;
        }
    }

    /* Count every unit the shim hands us, bound or not: in monitor mode this is
     * how the supervisor learns a session exists at all. */
    g_supervisor.accessUnitsIn++;
    if (stream->outFd < 0) {
        /* Queueing while unbound would only build stale latency for a port we do
         * not own; drop it and keep the configuration. */
        supervisor_publish(0);
        return 1;
    }

    /* Arrival-rate sample: after the fold check, because parameter-set records
     * are not frames and counting them inflates the rate for exactly the streams
     * that carry their headers on every keyframe. */
    if (!session->rateWindowStartMs) session->rateWindowStartMs = session->lastArrivalMs;
    session->rateWindowCount++;

    /* START GATE: hold until parameters and an IDR are both in hand. The ECU's
     * map app does not retry within an acquisition it cannot decode. */
    if (!session->noIdrGate && !session->streaming) {
        if (!info.hasIdr) {
            session->held++;
            if ((session->held & 0x3F) == 1)
                log_message("gate: waiting for a decodable start (IDR); held %ld",
                            (long)session->held);
            return 1;
        }
        session->streaming = 1;
        supervisor_set_ready(1);
        supervisor_publish(1);
        log_message("gate: IDR in hand after %ld held AU(s) -- starting the stream",
                    (long)session->held);
    }

    /* Carry the sequence header only where the ECU has always seen it: on an IDR,
     * or when it is new. */
    if (!session->noFold && session->parameters.length &&
        (session->headerPending || info.hasIdr)) {
        parameterBytes = session->parameters.length;
        session->headerPending = 0;
    }
    total = parameterBytes + (usize_t)length;
    if (total > session->foldedCapacity) {
        u8 *grown = (u8 *)realloc(session->folded, total);
        if (!grown) { log_message("oom"); return 0; }
        session->folded = grown;
        session->foldedCapacity = total;
    }
    if (parameterBytes) memcpy(session->folded, session->parameters.bytes, parameterBytes);
    memcpy(session->folded + parameterBytes, session->record, (usize_t)length);

    if (session->queue.count == QUEUE_CAPACITY) {        /* full: bound latency */
        session_publish_access_unit(session,
                                    session->queue.bytes[session->queue.head],
                                    session->queue.length[session->queue.head]);
        session->queue.head = (session->queue.head + 1) % QUEUE_CAPACITY;
        session->queue.count--;
    }
    {
        int slot = (session->queue.head + session->queue.count) % QUEUE_CAPACITY;
        if (total > session->queue.capacity[slot]) {
            u8 *grown = (u8 *)realloc(session->queue.bytes[slot], total);
            if (!grown) { log_message("oom"); return 0; }
            session->queue.bytes[slot] = grown;
            session->queue.capacity[slot] = total;
        }
        memcpy(session->queue.bytes[slot], session->folded, total);
        session->queue.length[slot] = total;
        session->queue.arrivalMs[slot] = session->lastArrivalMs;
        session->queue.count++;
    }

    /* BACKLOG GUARD: pending bytes on the socket are the real backlog signal. A
     * burst is measured to knock the cluster into its placeholder, so draining
     * forward to the next IDR is better than flooding it with stale pictures. */
    if (!session->noIdrGate && session->queue.count > 3) {
        long pending = socket_readable_bytes(descriptor);
        if (pending > (long)MAX_PENDING_BYTES) {
            session->backlogAccessUnits += (u64)session->queue.count;
            queue_clear(session);
            session->streaming = 0;
            supervisor_set_ready(0);
            supervisor_publish(1);
            session->backlogDrains++;
            log_message("gate: backlog %ld bytes -- drained %ld AUs forward to the next IDR",
                        pending, (long)session->backlogAccessUnits);
        }
    }
    return 1;
}

void session_finish(Session *session, int descriptor)
{
    Stream *stream = &session->stream;
    int drained = 0;

    close(descriptor);
    /* THE DISCONNECT EVENT, published before the drain: the drain can push
     * already-accepted frames onto the wire, and an event that trails the last
     * byte it describes is a worse event. */
    supervisor_set_connection(0);

    /* DRAIN WHAT WE ALREADY ACCEPTED: closing with frames queued silently
     * discarded the phone's last pictures. Bounded by QUEUE_CAPACITY. */
    g_timing.ignore = 1;                                 /* teardown is not the stream */
    while (session->queue.count > 0) {
        session_publish_access_unit(session,
                                    session->queue.bytes[session->queue.head],
                                    session->queue.length[session->queue.head]);
        session->queue.head = (session->queue.head + 1) % QUEUE_CAPACITY;
        session->queue.count--;
        drained++;
    }
    emitter_release_all(stream, 0);
    mux_flush(stream);                                   /* no partial datagram */
    log_message("conn close frame=%ld", (long)session->clock.frames);
    if (drained) log_message("  pacer: drained-at-close=%d", drained);
    g_timing.ignore = 0;

    log_message("  arrival-gaps>250ms=%ld  depth-drains=%ld  spread-overruns=%ld"
                "  spread-skipped=%ld  deferred-arms=%ld",
                (long)session->arrivalGaps, (long)session->jitter.depthDrains,
                (long)stream->emitter.statisticsOverruns,
                (long)stream->emitter.statisticsSkipped,
                (long)stream->emitter.statisticsDeferredArms);
    if (stream->emitter.statisticsOutOfMemory)
        log_message("  PACING-FELL-BACK-TO-IMMEDIATE=%ld", (long)stream->emitter.statisticsOutOfMemory);
    timing_report();                                     /* this session's wire shape */
    timing_reset();

    if (session->held || session->backlogDrains) {
        log_message("  gate: held=%ld  backlog drains=%ld  backlog AUs discarded=%ld  streaming=%d",
                    (long)session->held, (long)session->backlogDrains,
                    (long)session->backlogAccessUnits, session->streaming);
    }
}

#if defined(MIB3_UNIT_AARCH64)
#define PPOLL_(descriptors, count, timeout) \
    ppoll((void *)(descriptors), (unsigned long)(count), (const void *)(timeout), 0)
#else
static int host_ppoll(PollDescriptor *descriptors, usize_t count, const UnitTime *timeout)
{
    int milliseconds = 0;
    if (timeout)
        milliseconds = (int)(timespec_seconds(*timeout) * 1000 +
                             (timespec_nanoseconds(*timeout) + 999999) / 1000000);
    return poll((void *)descriptors, count, milliseconds);
}
#define PPOLL_(descriptors, count, timeout) host_ppoll((descriptors), (count), (timeout))
#endif

void session_run(Session *session, int listener)
{
    Stream *stream = &session->stream;

    log_message("buffer: jitter=%d ms (adaptive: grows on starvation, shrinks when idle)"
                "  intra-frame release=ON (poll/ppoll, byte-proportional)", session->jitterMs);
    log_message("  spread=%lu%% of each unit's interval (the remainder is cushion so the"
                " next frame can start on time -- 100 = no cushion)",
                (unsigned long)g_spreadPercent);
    supervisor_publish(1);                               /* nothing connected yet */

    for (;;) {
        int connection = -1;

        /* NEVER BLOCK IN accept(): in monitor mode an 'unbind' must be honoured
         * even while no phone is connected. */
        {
            PollDescriptor pollDescriptor;
            UnitTime timeout;
            int ready;
            pollDescriptor.descriptor = listener;
            pollDescriptor.events = POLL_EVENT_READABLE;
            pollDescriptor.returnedEvents = 0;
            timespec_seconds(timeout) = 0;
            timespec_nanoseconds(timeout) = (long)ACCEPT_POLL_TIMEOUT_MS * 1000000L;
            ready = PPOLL_(&pollDescriptor, 1, &timeout);
            if (ready > 0 && (pollDescriptor.returnedEvents & POLL_EVENT_READABLE))
                connection = accept(listener, 0, 0);
        }
        {
            int action = control_poll();
            if (action == 1) output_bind(session, g_control.requestedPort);
            else if (action == 2) output_unbind(session);
        }
        supervisor_publish(0);
        if (connection < 0) continue;

        /* THE CONNECT EVENT, before a single unit is read. */
        supervisor_set_connection(1);
        session_reset(session);
        log_message("conn open  frame=%ld", (long)session->clock.frames);

        for (;;) {
            int readable;
            {
                PollDescriptor pollDescriptor;
                UnitTime timeout;
                u64 wait = emitter_poll_timeout_ns(stream);
                int ready;
                timespec_seconds(timeout) = (long)(wait / 1000000000ull);
                timespec_nanoseconds(timeout) = (long)(wait % 1000000000ull);
                pollDescriptor.descriptor = connection;
                pollDescriptor.events = POLL_EVENT_READABLE;
                pollDescriptor.returnedEvents = 0;
                ready = PPOLL_(&pollDescriptor, 1, &timeout);
                readable = (ready > 0 && (pollDescriptor.returnedEvents & POLL_EVENT_READABLE));
            }

            /* Published on EVERY tick: while the port was only published from the
             * unbound branch the supervisor saw no access unit for its whole
             * grace and flapped the cluster. */
            supervisor_publish(0);

            {   /* The supervisor's hand on the port: a bind starts from a clean
                 * queue and a fresh IDR, an unbind leaves no half-emitted frame. */
                int action = control_poll();
                if (action == 1) {
                    output_bind(session, g_control.requestedPort);
                    if (stream->outFd >= 0) {
                        stream->mux.programAssociationLastMs = 0;
                        session->streaming = 0;
                        session->headerPending = 1;
                        queue_clear(session);
                    }
                } else if (action == 2) {
                    output_unbind(session);
                    session->streaming = 0;
                    queue_clear(session);
                }
            }

            session_release_tick(session);
            emitter_release_due(stream, current_time_ns());

            if (readable && !session_read_record(session, connection)) break;
        }
        session_finish(session, connection);
    }
}

/* ------------------------------------------------------------ 15. file input */

static const u8 *g_fileUnits[FILE_ACCESS_UNIT_LIMIT];
static int g_fileUnitLengths[FILE_ACCESS_UNIT_LIMIT];

static void play_file(Session *session, Options *options, int unitCount)
{
    Stream *stream = &session->stream;
    u64 gapsSent = 0;

    for (u64 frame = 0;; frame++) {
        const u8 *unit = g_fileUnits[frame % (u64)unitCount];
        int bytes = g_fileUnitLengths[frame % (u64)unitCount];
        u64 presentationTicks;

        if (options->gapMs > 0) {
            presentationTicks = stream->mux.presentationTimeTicks
                ? stream->mux.presentationTimeTicks + (TICKS_PER_SECOND / (u64)(options->framesPerSecond > 0 ? options->framesPerSecond : DEFAULT_VIDEO_FPS))
                : TICKS_PER_SECOND;
        } else {
            presentationTicks = (current_time_ms() - stream->mux.startMs) * TICKS_PER_MILLISECOND;
            if (presentationTicks <= stream->mux.presentationTimeTicks)
                presentationTicks = stream->mux.presentationTimeTicks + 1;
        }
        stream->mux.presentationTimeTicks = presentationTicks;
        mux_access_unit(stream, unit, bytes, presentationTicks);

        if (options->realtime) {
            u64 wanted = stream->mux.startMs +
                         (frame + 1) * (1000000ull / (u64)(options->framesPerSecond > 0 ? options->framesPerSecond : DEFAULT_VIDEO_FPS)) / 1000ull;
            u64 now = current_time_ms();
            if (wanted > now) usleep((unsigned)((wanted - now) * 1000ull));
        }
        /* Simulate the phone's silence: the timeline keeps stepping while
         * nothing is written, which isolates "silence on the wire" from "PTS
         * jump". */
        if (options->gapMs > 0 && options->gapEvery > 0 &&
            ((frame + 1) % (u64)options->gapEvery) == 0) {
            gapsSent++;
            if ((gapsSent & 0x07) == 1)
                log_message("gap %ld after frame %ld (%d ms of silence)",
                            (long)gapsSent, (long)(frame + 1), options->gapMs);
            usleep((unsigned)options->gapMs * 1000u);
        }
        if (!options->loop && frame + 1 >= (u64)unitCount) break;
    }
}

/* ------------------------------------------------------------------ 16. probe
 * Join the group and dump whatever is already on it: prove the unit's own sender
 * is live and see its exact RTP fields, or capture its datagrams so ours can be
 * diffed against them. */

/* "address%interface" in place: the interface defaults to the VLAN the cluster
 * listens on. Shared by the output settings and the probe. */
static void split_destination(char *text, const char **interfaceName)
{
    *interfaceName = "eth0.5";
    for (char *cursor = text; *cursor; cursor++)
        if (*cursor == '%') { *cursor = 0; *interfaceName = cursor + 1; break; }
}

int probe_group(const char *destination, int port, const char *output, int count, int seconds, int videoOnly)
{
    char host[HOST_NAME_BYTES];
    const char *interfaceName = "eth0.5";
    usize_t destinationLength = strlen(destination);
    Ipv6Address address;
    Ipv6Address group;
    int descriptor, outputDescriptor = -1;
    u8 buffer[4096];
    long datagrams = 0;
    u64 firstSequence = 0, lastSequence = 0, ssrc = 0;
    int have = 0;
    u64 startedMs, skipped = 0;

    if (destinationLength >= sizeof host) destinationLength = sizeof host - 1;
    memcpy(host, destination, destinationLength);
    host[destinationLength] = 0;
    split_destination(host, &interfaceName);

    descriptor = socket(AF_INET6, SOCK_DGRAM, 0);
    if (descriptor < 0) { log_message("probe: socket failed"); return 1; }
    { int one = 1;
      setsockopt(descriptor, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
      setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }

    memset(&address, 0, sizeof address);
    ipv6_family(address) = AF_INET6;
    ipv6_port(address) = htons((u16)port);
    set_socket_length_ipv6(address);
    if (bind(descriptor, (const void *)&address, sizeof address) < 0) {
        log_message("probe: bind failed");
        close(descriptor);
        return 1;
    }

    memset(&group, 0, sizeof group);
    if (inet_pton(AF_INET6, host, &ipv6_address(group)) != 1) {
        log_message("probe: bad group");
        close(descriptor);
        return 1;
    }
    {   /* IPV6_JOIN_GROUP wants { address, interface index } */
        u8 request[24];
        unsigned index = if_nametoindex(interfaceName);
        memset(request, 0, sizeof request);
        memcpy(request, &ipv6_address(group), 16);
        memcpy(request + 16, &index, sizeof index);
        if (setsockopt(descriptor, IPPROTO_IPV6, IPV6_JOIN_GROUP, request, (unsigned)sizeof request) < 0) {
            log_message("probe: join failed");
            close(descriptor);
            return 1;
        }
    }
    log_message("probe: joined %s port %d", host, port);

    if (output && !strncmp(output, "file:", 5))
        outputDescriptor = open(output + 5, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    startedMs = current_time_ms();
    while (count < 0 || datagrams < count) {
        PollDescriptor pollDescriptor;
        long received;
        int ready;

        pollDescriptor.descriptor = descriptor;
        pollDescriptor.events = POLL_EVENT_READABLE;
        pollDescriptor.returnedEvents = 0;
        ready = poll((void *)&pollDescriptor, 1, 200);

        /* The wall-clock bound is checked EVERY iteration, not only on a poll
         * timeout: the idle beacon on this group is ~750 datagrams/s, so poll
         * never times out and a count-bounded probe would run for ever. */
        if (seconds > 0 && current_time_ms() - startedMs >= (u64)seconds * 1000ull) {
            log_message("probe: window elapsed (%d s)", seconds);
            break;
        }
        if (ready == 0) continue;
        if (ready < 0) break;

        received = read(descriptor, buffer, sizeof buffer);
        if (received <= 0) break;
        if (received < RTP_HEADER_BYTES + TRANSPORT_PACKET_BYTES) continue;
        if (videoOnly && (buffer[1] & 0x7F) != 33) { skipped++; continue; }
        {
            u64 sequence = ((u64)buffer[2] << 8) | buffer[3];
            u64 receivedSsrc = ((u64)buffer[8] << 24) | ((u64)buffer[9] << 16) |
                               ((u64)buffer[10] << 8) | buffer[11];
            if (!have) { firstSequence = sequence; ssrc = receivedSsrc; have = 1; }
            lastSequence = sequence;
            datagrams++;
        }
        if (outputDescriptor >= 0) write(outputDescriptor, buffer, (usize_t)received);
        if (datagrams == 1) {
            static const char digits[] = "0123456789abcdef";
            char hex[RTP_HEADER_BYTES * 2 + 1];
            int used = 0;
            for (int index = 0; index < RTP_HEADER_BYTES; index++) {
                hex[used++] = digits[buffer[index] >> 4];
                hex[used++] = digits[buffer[index] & 0x0F];
            }
            hex[used] = 0;
            log_message("probe: first datagram rtp=%s", hex);
        }
    }
    {
        u64 elapsed = current_time_ms() - startedMs;
        log_message("probe: datagrams=%ld", datagrams);
        log_message("probe: elapsed_ms=%ld", (long)elapsed);
        if (elapsed > 0) log_message("probe: datagrams per second=%ld",
                                     (long)(((u64)datagrams * 1000ull) / elapsed));
        /* A count-only bound is a trap: say so rather than let the count be read
         * as a rate. */
        if (seconds > 0 && elapsed + 50 < (u64)seconds * 1000ull)
            log_message("probe: COUNT CAP HIT before the window elapsed -- TRUNCATED;"
                        " do not read the count as a rate");
    }
    if (videoOnly) log_message("probe: skipped non-video=%ld", (long)skipped);
    log_message("probe: seq first=%ld", (long)firstSequence);
    log_message("probe: seq last=%ld", (long)lastSequence);
    log_message("probe: ssrc=%ld", (long)ssrc);
    if (outputDescriptor >= 0) close(outputDescriptor);
    close(descriptor);
    return 0;
}

/* ------------------------------------------------- 17. output socket and ctl
 * The port is taken only when told to, which is also the exact moment it becomes
 * free. See docs/development/pathb-supervisor-handover.md. */

int output_socket_open(OutputSettings *output)
{
    int descriptor = socket(AF_INET6, SOCK_DGRAM, 0);
    Ipv6Address source;
    Ipv6Address destination;

    if (descriptor < 0) { log_message("socket() failed"); return -1; }

    {   unsigned index = if_nametoindex(output->interfaceName);
        if (index) setsockopt(descriptor, IPPROTO_IPV6, IPV6_MULTICAST_IF, &index, sizeof index); }
    {   int hop = 1; setsockopt(descriptor, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hop, sizeof hop); }
    {   int loop = 1; setsockopt(descriptor, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &loop, sizeof loop); }
#if defined(UNIT_HAS_BINDTODEVICE)
    if (output->interfaceName && *output->interfaceName)
        setsockopt(descriptor, SOL_SOCKET, SO_BINDTODEVICE, output->interfaceName,
                   (unsigned)strlen(output->interfaceName));
#endif

    if (output->sourcePort > 0 || (output->sourceAddress && *output->sourceAddress)) {
        /* The receiver only accepts video from the port multi-screen-server uses,
         * so this bind IS the takeover. Deliberately no SO_REUSEADDR: if the unit
         * still holds the port we must fail loudly, not share it. */
        memset(&source, 0, sizeof source);
        ipv6_family(source) = AF_INET6;
        ipv6_port(source) = htons((u16)output->sourcePort);
        set_socket_length_ipv6(source);
        if (output->sourceAddress && *output->sourceAddress &&
            inet_pton(AF_INET6, output->sourceAddress, &ipv6_address(source)) != 1) {
            log_message("bad --src");
            close(descriptor);
            return -1;
        }
        if (bind(descriptor, (const void *)&source, sizeof source) < 0) {
            log_message("SPORT_BUSY: cannot bind source port %d"
                        "  (multi-screen-server still holds it -- park it first)",
                        output->sourcePort);
            close(descriptor);
            return -2;
        }
        if (output->sourcePort > 0) log_message("bound source port %d", output->sourcePort);
    }

    memset(&destination, 0, sizeof destination);
    ipv6_family(destination) = AF_INET6;
    ipv6_port(destination) = htons((u16)output->destinationPort);
    set_socket_length_ipv6(destination);
    if (inet_pton(AF_INET6, output->host, &ipv6_address(destination)) != 1) {
        log_message("bad --dst");
        close(descriptor);
        return -1;
    }
    if (connect(descriptor, (const void *)&destination, sizeof destination) < 0) {
        log_message("connect failed");
        close(descriptor);
        return -1;
    }
    log_message("-> %s port %d", output->host, output->destinationPort);
    return descriptor;
}

void output_bind(Session *session, int port)
{
    int descriptor;
    if (session->stream.outFd >= 0) return;
    if (port > 0) g_output.sourcePort = port;
    descriptor = output_socket_open(&g_output);
    if (descriptor < 0) { log_message("ctl: bind did not happen"); return; }
    session->stream.outFd = descriptor;
    session->stream.emitter.count = session->stream.emitter.cursor = 0;
    supervisor_set_bound(g_output.sourcePort);
    supervisor_set_ready(0);
    log_message("ctl: PORT OURS");
    supervisor_publish(1);
}

void output_unbind(Session *session)
{
    if (session->stream.outFd < 0) return;
    close(session->stream.outFd);
    session->stream.outFd = -1;
    session->stream.emitter.count = session->stream.emitter.cursor = 0;
    supervisor_set_bound(0);
    supervisor_set_ready(0);
    log_message("ctl: port released");
    supervisor_publish(1);
}

/* 0 = nothing new, 1 = bind, 2 = unbind, -1 = unrecognised (never acted on) */
int control_poll(void)
{
    char buffer[CONTROL_COMMAND_BYTES];
    int descriptor, action = 0;
    long received;
    int index;

    if (!g_control.path[0]) return 0;
    if (current_time_ms() - g_control.lastPollMs < CONTROL_POLL_INTERVAL_MS) return 0;
    g_control.lastPollMs = current_time_ms();

    descriptor = open(g_control.path, O_RDONLY);
    if (descriptor < 0) return 0;
    received = read(descriptor, buffer, sizeof buffer - 1);
    close(descriptor);
    if (received <= 0) return 0;
    buffer[received] = 0;
    for (index = 0; index < (int)received; index++)
        if (buffer[index] == '\n' || buffer[index] == '\r') buffer[index] = 0;
    if (!strcmp(buffer, g_control.lastCommand)) return 0;
    {   int copy = 0;
        while (copy < (int)sizeof g_control.lastCommand - 1 && buffer[copy]) {
            g_control.lastCommand[copy] = buffer[copy];
            copy++;
        }
        g_control.lastCommand[copy] = 0;
    }

    if (!strncmp(buffer, "bind", 4)) {
        /* The port travels WITH the command, so a supervisor that discovers a new
         * multi-screen-server port never has to restart this process. */
        int requested = atoi(buffer + 4);
        if (requested > 0 && requested < 65536) { g_control.requestedPort = requested; action = 1; }
        else { log_message("ctl: 'bind' without a valid port -- ignored"); action = -1; }
    } else if (!strncmp(buffer, "unbind", 6)) {
        action = 2;
    } else {
        log_message("ctl: unrecognised command -- ignored");
        action = -1;
    }
    return action;
}

/* -------------------------------------------------------------- 18. options */

static void options_set_defaults(Options *options)
{
    memset(options, 0, sizeof *options);
    options->input = "-";
    options->inputFormat = "annexb";
    options->destination = DEFAULT_DESTINATION;
    options->destinationPort = DEFAULT_PORT;
    options->framesPerSecond = DEFAULT_VIDEO_FPS;
    options->jitterMs = DEFAULT_JITTER_MS;
    options->spreadPercent = DEFAULT_SPREAD_PERCENT;
    options->packetsPerDatagram = DEFAULT_PACKETS_PER_DATAGRAM;
    options->ssrc = DEFAULT_SSRC;
    options->transportStreamId = DEFAULT_TRANSPORT_STREAM_ID;
    options->pmtPid = DEFAULT_PMT_PID;
    options->elementaryPid = DEFAULT_ELEMENTARY_PID;
    options->statePath = DEFAULT_STATE_PATH;
    options->controlPath = DEFAULT_CONTROL_PATH;
    options->probeCount = -1;
}

int parse_options(int argc, char **arguments, Options *options)
{
    options_set_defaults(options);

    for (int index = 1; index < argc; index++) {
        const char *argument = arguments[index];
        const char *value = (index + 1 < argc) ? arguments[index + 1] : 0;

        if (!strcmp(argument, "--in") && value) { options->input = value; index++; }
        else if (!strcmp(argument, "--in-fmt") && value) { options->inputFormat = value; index++; }
        else if (!strcmp(argument, "--dst") && value) { options->destination = value; index++; }
        else if (!strcmp(argument, "--src") && value) { options->sourceAddress = value; index++; }
        else if (!strcmp(argument, "--port") && value) { options->destinationPort = atoi(value); index++; }
        else if (!strcmp(argument, "--sport") && value) {
            if (!strcmp(value, "auto")) {
                Pe("--sport auto: discovery is pathb/msport.py (it reads /proc/<pid>/fd\n"
                   "  and /proc/net/udp6 and prints the port); pass --sport <n>.  Refusing to guess.\n");
                return 2;
            }
            options->sourcePort = atoi(value);
            index++;
        }
        else if (!strcmp(argument, "--out") && value) { options->output = value; index++; }
        else if (!strcmp(argument, "--conn-state") && value) { options->statePath = value; index++; }
        else if (!strcmp(argument, "--ctl") && value) { options->controlPath = value; index++; }
        else if (!strcmp(argument, "--monitor")) options->monitor = 1;
        else if (!strcmp(argument, "--pkts-per-dgram") && value) { options->packetsPerDatagram = atoi(value); index++; }
        else if (!strcmp(argument, "--ssrc") && value) { options->ssrc = atoi(value); index++; }
        else if (!strcmp(argument, "--tsid") && value) { options->transportStreamId = atoi(value); index++; }
        else if (!strcmp(argument, "--pmt-pid") && value) { options->pmtPid = atoi(value); index++; }
        else if (!strcmp(argument, "--es-pid") && value) { options->elementaryPid = atoi(value); index++; }
        else if (!strcmp(argument, "--fps") && value) { options->framesPerSecond = atoi(value); index++; }
        else if (!strcmp(argument, "--loop")) options->loop = 1;
        else if (!strcmp(argument, "--realtime")) options->realtime = 1;
        else if (!strcmp(argument, "--probe")) options->probe = 1;
        else if (!strcmp(argument, "--no-fold")) options->noFold = 1;      /* comparison only */
        else if (!strcmp(argument, "--no-idr-gate")) options->noIdrGate = 1;/* comparison only */
        else if (!strcmp(argument, "--probe-count") && value) { options->probeCount = atoi(value); index++; }
        else if (!strcmp(argument, "--probe-secs") && value) { options->probeSeconds = atoi(value); index++; }
        else if (!strcmp(argument, "--probe-video-only")) options->probeVideoOnly = 1;
        else if (!strcmp(argument, "--stats-timing")) options->statisticsTiming = 1;
        else if (!strcmp(argument, "--jit-ms") && value) { options->jitterMs = atoi(value); index++; }
        else if (!strcmp(argument, "--no-jit")) options->jitterMs = 0;
        /* Spread budget as a fraction of the release interval. 100 restores the
         * pre-2026-11-02 behaviour exactly (budget == interval, zero cushion). */
        else if (!strcmp(argument, "--spread-pct") && value) { options->spreadPercent = (u32)atoi(value); index++; }
        else if (!strcmp(argument, "--gap-ms") && value) { options->gapMs = atoi(value); index++; }
        else if (!strcmp(argument, "--gap-every") && value) { options->gapEvery = atoi(value); index++; }
        else if (!strcmp(argument, "--pace-ms") || !strcmp(argument, "--no-pace") ||
                 !strcmp(argument, "--ka-ms") || !strcmp(argument, "--no-ka")) {
            /* Removed features, refused loudly rather than silently ignored. */
            Pe("mib3clusterstream: --pace-ms, --no-pace, --ka-ms and --no-ka were removed.\n"
               "  Output pacing is intra-frame and always on; idle fill was measured and refuted.\n"
               "  Use --spread-pct (fraction of the interval to spread a unit over) or --jit-ms\n"
               "  (buffer depth).  See docs/development/pathb-pacing-and-timing.md\n");
            return 2;
        }
        else { Pe("unknown arg\n"); return 2; }
    }

    if (options->packetsPerDatagram < 1 ||
        options->packetsPerDatagram > MAX_PACKETS_PER_DATAGRAM) {
        Pe("bad --pkts-per-dgram\n");
        return 2;
    }
    /* 20% is the floor because below that the instantaneous rate approaches the
     * wire ceiling and a drain burst starts to look like the clump. */
    if (options->spreadPercent < 20) options->spreadPercent = 20;
    if (options->spreadPercent > 100) options->spreadPercent = 100;
    return 0;
}

void output_settings_from_options(OutputSettings *output, Options *options)
{
    usize_t length = strlen(options->destination);
    memset(output, 0, sizeof *output);
    if (length >= sizeof output->host) length = sizeof output->host - 1;
    memcpy(output->host, options->destination, length);
    output->host[length] = 0;
    split_destination(output->host, &output->interfaceName);
    output->destinationPort = options->destinationPort;
    output->sourcePort = options->sourcePort;
    output->sourceAddress = options->sourceAddress;
}

/* ------------------------------------------------ 19. entry point and bodies */

static void arm_diagnostics(Options *options)
{
    if (options->statePath && *options->statePath && strcmp(options->statePath, "off") != 0) {
        usize_t length = strlen(options->statePath);
        if (length < sizeof g_supervisor.path) {
            memcpy(g_supervisor.path, options->statePath, length);
            g_supervisor.path[length] = 0;
        } else {
            log_message("--conn-state: path too long, publisher disabled");
        }
    }
    if (options->controlPath && *options->controlPath) {
        usize_t length = strlen(options->controlPath);
        if (length < sizeof g_control.path) {
            memcpy(g_control.path, options->controlPath, length);
            g_control.path[length] = 0;
        } else {
            log_message("--ctl: path too long, control channel disabled");
        }
    }
}

static int open_listener(const char *path)
{
    UnixAddress address;
    int listener;
    usize_t length = strlen(path);
    usize_t limit = sizeof(unix_path(address));

    memset(&address, 0, sizeof address);
    unix_family(address) = AF_UNIX;
    if (length > limit - 1) length = limit - 1;
    memcpy(unix_path(address), path, length);
    unix_path(address)[length] = 0;

    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) { log_message("socket(unix) failed"); return -1; }
    unlink(unix_path(address));
    if (bind(listener, (const void *)&address, sizeof address) < 0) {
        log_message("bind(unix) failed");
        close(listener);
        return -1;
    }
    /* The shim runs as `carplay` (uid 986) and the socket is created by root: a
     * default umask leaves 0755 and connect() fails EACCES with the right SMACK
     * label but the wrong mode. */
    chmod(unix_path(address), 0777);
    listen(listener, 4);
    log_message("listening %s (0777, re-accepting)", unix_path(address));
    return listener;
}

int mib3_run(int argc, char **arguments)
{
    Options options;
    Session session;
    Stream *stream = &session.stream;
    int listener = -1;
    int unitCount = 0;
    u8 *blob = 0;
    usize_t blobLength = 0;
    int result;

    result = parse_options(argc, arguments, &options);
    if (result) return result;
    g_spreadPercent = options.spreadPercent;
    arm_diagnostics(&options);

    session_initialise(&session, &options);
    stream->mux.packetsPerDatagram = options.packetsPerDatagram;
    stream->mux.ssrc = (u32)options.ssrc;
    stream->mux.transportStreamId = options.transportStreamId;
    stream->mux.pmtPid = options.pmtPid;
    stream->mux.elementaryPid = options.elementaryPid;
    stream->mux.pcrPid = options.elementaryPid;
    stream->emitter.stride = RTP_HEADER_BYTES + options.packetsPerDatagram * TRANSPORT_PACKET_BYTES;

    g_timing.ring.values = g_timingValues;
    g_timing.ring.capacity = TIMING_RING_CAPACITY;
    g_timingEnabled = options.statisticsTiming;

    if (options.probe)
        return probe_group(options.destination, options.destinationPort, options.output,
                           options.probeCount, options.probeSeconds, options.probeVideoOnly);

    if (options.output && !strncmp(options.output, "file:", 5)) {
        stream->outFd = open(options.output + 5, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (stream->outFd < 0) { log_message("cannot open out"); return 1; }
    } else {
        output_settings_from_options(&g_output, &options);
        if (options.monitor) {
            /* The supervisor decides when the port is ours; until then this
             * process only watches the shim's stream. */
            stream->outFd = -1;
            log_message("monitor: AU socket only -- the port is taken on 'bind <port>'");
        } else {
            int descriptor = output_socket_open(&g_output);
            if (descriptor < 0) return descriptor == -2 ? 3 : 1;
            stream->outFd = descriptor;
        }
    }

    stream->mux.startMs = current_time_ms();

    if (!strncmp(options.input, "unix:", 5)) {
        listener = open_listener(options.input + 5);
        if (listener < 0) return 1;
        session_run(&session, listener);   /* never returns */
        close(listener);
    } else {
        int descriptor = (!strcmp(options.input, "-")) ? 0 : open(options.input, O_RDONLY);
        if (descriptor < 0) { log_message("cannot open in"); return 1; }
        if (read_all(descriptor, &blob, &blobLength) < 0) { log_message("read failed"); return 1; }
        if (descriptor) close(descriptor);
        if (!strcmp(options.inputFormat, "avcc")) {
            usize_t converted = 0;
            u8 *annexb = avcc_to_annexb(blob, blobLength, &converted);
            if (!annexb) { log_message("avcc convert failed"); return 1; }
            free(blob);
            blob = annexb;
            blobLength = converted;
        }
        unitCount = split_access_units(blob, blobLength, g_fileUnits, g_fileUnitLengths,
                                       FILE_ACCESS_UNIT_LIMIT);
        log_message("access units: %d", unitCount);
        if (unitCount == 0) return 1;
        play_file(&session, &options, unitCount);
    }

    emitter_release_all(stream, 0);
    mux_flush(stream);
    log_message("datagrams: %ld", (long)stream->mux.sequence);
    {   u64 elapsed = current_time_ms() - stream->mux.startMs;
        log_message("elapsed_ms: %ld", (long)elapsed);
        log_message("write-failures: %ld", (long)stream->mux.writeFailures);
        if (elapsed > 0)
            log_message("datagrams/sec: %ld", (long)(((u64)stream->mux.sequence * 1000ull) / elapsed));
    }
    timing_report();
    if (listener >= 0) close(listener);
    if (stream->outFd >= 0) close(stream->outFd);
    queue_release(&session);
    return 0;
}

#if defined(MIB3_UNIT_AARCH64)
/* Freestanding entry: the unit's loader has no crt for us. MUST be naked -- a
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
#else
int main(int argc, char **arguments) { return mib3_run(argc, arguments); }
#endif
