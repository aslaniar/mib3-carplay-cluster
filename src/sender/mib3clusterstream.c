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
 * and sends them to the same group and port.
 *
 * THE MODEL. A unit is muxed the moment it arrives and every datagram is given
 * a deadline inside that unit's slot. The pipe is therefore both the jitter
 * buffer and the intra-frame release: depth is how far ahead the schedule runs,
 * and a phase governor trims the output rate so it stays near the target. A
 * burst is drained by shortening the next slots, never by growing a queue, and
 * a release-time floor stops a stalled reader from turning into a clump.
 *
 *   OUT  --dst <ipv6>[%iface]  (default ff14::2:4%eth0.5)   --src <ipv6>
 *        --port <n> (default 42800)   --sport <n>   --out file:<path>
 *   IN   --in <file> | --in unix:<path>   --in-fmt annexb|avcc
 *   DIAG --trace <path>   (one tab-separated event per arrival/schedule/emission;
 *                          the offline replay in tests/replay reads it)
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
 * file (format_line straight into a buffer). The toolkit supplies the write; a
 * line is assembled first so a multi-field line is ONE write, not fifteen. */

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

/* The offline replay's event log (--trace). One tab-separated line per event,
 * written as it happens; a datagram capture carries no timestamps, so this is
 * the only place the harness can see WHEN a unit was scheduled and released.
 * Disabled by default; when off it is one predictable branch. */
void trace_message(Runtime *runtime, const char *format, ...)
{
    char line[LOG_LINE_BYTES];
    va_list arguments;
    usize_t used;

    if (runtime->traceFd < 0) return;
    va_start(arguments, format);
    used = format_arguments(line, sizeof line, format, arguments);
    va_end(arguments);
    append_character(line, sizeof line, &used, '\n');
    (void)write(runtime->traceFd, line, used);
}

void trace_close(Runtime *runtime)
{
    if (runtime->traceFd >= 0) {
        close(runtime->traceFd);
        runtime->traceFd = -1;
    }
}

/* ------------------------------------------------------------------ 3. clocks
 * Two clocks on purpose: the schedule, the media clock and every report use
 * milliseconds; the pipe uses nanoseconds because a datagram deadline is a few
 * milliseconds out and a millisecond quantisation would be a large share of the
 * spacing. */

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

/* ------------------------------------------------------------------- 4. rings */

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

/* Diagnostic buffers: the rings point into these, so they outlive any Runtime. */
static u64 g_cadenceArrivalValues[CADENCE_RING_CAPACITY];
static u64 g_cadenceEmissionValues[CADENCE_RING_CAPACITY];
static u64 g_latencyValues[LATENCY_RING_CAPACITY];
static u64 g_timingValues[TIMING_RING_CAPACITY];

/* -------------------------------------------------- 5. supervisor's state line
 * One line, rewritten by rename so a reader never sees a blank. Counters rather
 * than a flag because on wireless CarPlay the phone's socket outlives the
 * session: what a session is, is pictures arriving. See
 * docs/development/pathb-supervisor-handover.md. */

void supervisor_publish(Runtime *runtime, int force)
{
    Supervisor *supervisor = &runtime->supervisor;
    char line[STATE_LINE_BYTES];
    char temporary[STATE_PATH_BYTES + 8];
    long written;
    usize_t length;
    int descriptor;

    if (!supervisor->path[0]) return;
    if (!force && current_time_ms() - supervisor->lastPublishMs < SUPERVISOR_PUBLISH_INTERVAL_MS) return;

    supervisor->sequence++;
    length = format_line(line, sizeof line,
                         "conn=%d ready=%d bound=%d au=%ld out=%ld ms=%ld seq=%lu\n",
                         supervisor->connection, supervisor->ready, supervisor->bound,
                         (long)supervisor->accessUnitsIn, (long)supervisor->accessUnitsOut,
                         (long)current_time_ms(), (unsigned long)supervisor->sequence);

    format_line(temporary, sizeof temporary, "%s.tmp", supervisor->path);
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor >= 0) {
        written = write(descriptor, line, length);
        close(descriptor);
        if (written == (long)length) rename(temporary, supervisor->path);
        else unlink(temporary);
    }
    supervisor->lastPublishMs = current_time_ms();
}

void supervisor_set_connection(Runtime *runtime, int connected)
{
    runtime->supervisor.connection = connected ? 1 : 0;
    if (!connected) runtime->supervisor.ready = 0;
    supervisor_publish(runtime, 1);
}

void supervisor_set_ready(Runtime *runtime, int ready)
{
    runtime->supervisor.ready = ready ? 1 : 0;
}

void supervisor_set_bound(Runtime *runtime, int port)
{
    runtime->supervisor.bound = port;
}

/* ------------------------------------------------------- 6. emission timing
 * --stats-timing records the monotonic timestamp of every write and reports the
 * distribution at exit. A datagram capture carries no timestamps, so this is the
 * only way to prove WHEN the wire was busy. Off by default. */

void timing_note_write(Runtime *runtime)
{
    static const u64 edges[TIMING_HISTOGRAM_BUCKETS - 1] = {
        500000ull, 1000000ull, 2000000ull, 4000000ull, 8000000ull, 16000000ull,
        20000000ull, 40000000ull, 80000000ull, 160000000ull, 320000000ull
    };
    TimingStats *timing = &runtime->timing;
    u64 now;
    int bucket = 0;

    if (!runtime->timingEnabled) return;
    now = current_time_ns();
    if (timing->ignore) { timing->lastNs = now; return; }
    if (timing->lastNs) {
        u64 gap = now - timing->lastNs;
        if (gap > timing->longestNs) timing->longestNs = gap;
        if (gap > TIMING_LONG_GAP_NS) {
            timing->gapsOver20Ms++;
            /* Name the long gaps while they happen: on the car this says which
             * datagram the wire went quiet at. Capped so a pathological run
             * cannot flood the log. */
            if (timing->gapsOver20Ms <= TIMING_LONG_GAP_REPORT_LIMIT) {
                log_message("timing: idle %ld us before write %ld (gap %ld)",
                            (long)(gap / 1000), (long)timing->writes, (long)timing->gapsOver20Ms);
            }
        }
        while (bucket < TIMING_HISTOGRAM_BUCKETS - 1 && gap > edges[bucket]) bucket++;
        timing->histogram[bucket]++;
    }
    timing->lastNs = now;
    timing->writes++;

    ring_push(&timing->ring, now);
    {   /* peak density: datagrams inside ANY 25 ms window. The unit's own peak is
         * 61 on the same interface, so the numbers compare. */
        int density = ring_values_within(&timing->ring, now, TIMING_PEAK_WINDOW_NS);
        if ((u64)density > timing->peakPer25Ms) timing->peakPer25Ms = (u64)density;
    }
}

void timing_report(Runtime *runtime)
{
    static const char *labels[TIMING_HISTOGRAM_BUCKETS] = {
        "<0.5ms", "<1ms", "<2ms", "<4ms", "<8ms", "<16ms",
        "<20ms", "<40ms", "<80ms", "<160ms", "<320ms", ">=320ms"
    };
    TimingStats *timing = &runtime->timing;

    if (!runtime->timingEnabled) return;
    log_message("timing: writes=%ld  longest-gap=%ld us  gaps>20ms=%ld  peak-dg/25ms=%ld",
                (long)timing->writes, (long)(timing->longestNs / 1000),
                (long)timing->gapsOver20Ms, (long)timing->peakPer25Ms);
    log_message("timing: inter-datagram gap histogram (count per bucket)");
    for (int i = 0; i < TIMING_HISTOGRAM_BUCKETS; i++) {
        if (!timing->histogram[i]) continue;
        log_message("  timing:   %s %ld", labels[i], (long)timing->histogram[i]);
    }
}

void timing_reset(Runtime *runtime)
{
    Ring ring = runtime->timing.ring;
    memset(&runtime->timing, 0, sizeof runtime->timing);
    runtime->timing.ring = ring;
    ring_reset(&runtime->timing.ring);
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
 * always were. The pipe then holds each datagram until its deadline. */

void stream_write_datagram(Runtime *runtime, const u8 *datagram, int bytes)
{
    Stream *stream = &runtime->stream;
    long written;

    if (stream->outFd < 0) return;
    written = write(stream->outFd, datagram, (usize_t)bytes);
    timing_note_write(runtime);
    if (written != (long)bytes) {
        stream->mux.writeFailures++;
        if (stream->mux.writeFailures <= 3 || (stream->mux.writeFailures & 0xFF) == 0)
            log_message("mux: WRITE FAILED (datagram lost) count=%ld", (long)stream->mux.writeFailures);
    }
}

/* Drop the datagrams already emitted and move the pending ones to the front, so
 * the arrays are reused without a ring index. The pending set is normally a frame
 * or two, so the move is trivial and `count` keeps its obvious meaning. */
static void pipe_compact(Pipe *pipe)
{
    int pending;
    if (pipe->cursor == 0) return;
    pending = pipe->count - pipe->cursor;
    if (pending > 0) {
        memmove(pipe->bytes, pipe->bytes + (usize_t)pipe->cursor * (usize_t)pipe->stride,
                (usize_t)pending * (usize_t)pipe->stride);
        memmove(pipe->datagrams, pipe->datagrams + pipe->cursor,
                (usize_t)pending * sizeof(Datagram));
    }
    pipe->count = pending;
    pipe->cursor = 0;
}

int pipe_reserve(Pipe *pipe, int wanted)
{
    int newCapacity;
    u8 *bytes;
    Datagram *datagrams;

    if (pipe->count + wanted <= pipe->capacity) return 1;
    newCapacity = pipe->capacity ? pipe->capacity : 64;
    while (newCapacity < pipe->count + wanted) {
        if (newCapacity >= PIPE_CAPACITY) return 0;
        newCapacity *= 2;
    }
    if (newCapacity > PIPE_CAPACITY) newCapacity = PIPE_CAPACITY;

    bytes = (u8 *)realloc(pipe->bytes, (usize_t)newCapacity * (usize_t)pipe->stride);
    if (!bytes) return 0;
    pipe->bytes = bytes;
    datagrams = (Datagram *)realloc(pipe->datagrams, (usize_t)newCapacity * sizeof(Datagram));
    if (!datagrams) return 0;
    pipe->datagrams = datagrams;
    pipe->capacity = newCapacity;
    return 1;
}

/* Deadline of the datagram that just closed: the midpoint of its byte range
 * inside the frame's slot, proportional to payload bytes actually delivered. */
static u64 pipe_datagram_deadline(Pipe *pipe)
{
    u64 total = pipe->unitPayloadTotal ? pipe->unitPayloadTotal : 1;
    u64 low = pipe->unitPayloadFlushed;
    u64 high = pipe->unitPayloadDone;
    u64 middle;

    if (high > total) high = total;
    if (low > high) low = high;
    middle = low + (high - low) / 2;
    pipe->unitPayloadFlushed = pipe->unitPayloadDone;
    return pipe->unitStartNs + (pipe->unitLengthNs * middle) / total;
}

void pipe_append_datagram(Runtime *runtime, int bytes)
{
    Pipe *pipe = &runtime->stream.pipe;
    int slot = pipe->count;

    memcpy(pipe->bytes + (usize_t)slot * (usize_t)pipe->stride,
           runtime->stream.mux.datagram, (usize_t)bytes);
    pipe->datagrams[slot].length = (u32)bytes;
    pipe->datagrams[slot].deadlineNs = pipe_datagram_deadline(pipe);
    pipe->datagrams[slot].arrivalNs = pipe->unitArrivalNs;
    pipe->count++;
}

void mux_flush(Runtime *runtime)
{
    Mux *mux = &runtime->stream.mux;
    Pipe *pipe = &runtime->stream.pipe;
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
    if (pipe->unitPaced) pipe_append_datagram(runtime, total);
    else                 stream_write_datagram(runtime, mux->datagram, total);
    mux->packetsInDatagram = 0;
}

void mux_push(Runtime *runtime, const u8 *packet)
{
    Mux *mux = &runtime->stream.mux;
    memcpy(mux->datagram + RTP_HEADER_BYTES + mux->packetsInDatagram * TRANSPORT_PACKET_BYTES,
           packet, TRANSPORT_PACKET_BYTES);
    if (++mux->packetsInDatagram >= mux->packetsPerDatagram) mux_flush(runtime);
}

void mux_push_null(Runtime *runtime)
{
    u8 packet[TRANSPORT_PACKET_BYTES];
    packet[0] = TRANSPORT_PACKET_SYNC;
    packet[1] = 0x1F;                                   /* PID 0x1FFF */
    packet[2] = 0xFF;
    packet[3] = 0x10;                                   /* payload only */
    memset(packet + 4, 0xFF, TRANSPORT_PACKET_BYTES - 4);
    mux_push(runtime, packet);
}

/* End every access unit on a datagram boundary. The unit's own sender never
 * straddles units (except at PSI insertions) and pads with null packets; without
 * this, a unit whose packet count was not a multiple of the datagram's left its
 * tail buffered until the next unit arrived. */
void mux_end_access_unit(Runtime *runtime)
{
    while (runtime->stream.mux.packetsInDatagram != 0) mux_push_null(runtime);
}

void mux_access_unit(Runtime *runtime, const u8 *unit, int bytes, u64 presentationTicks)
{
    Mux *mux = &runtime->stream.mux;
    Pipe *pipe = &runtime->stream.pipe;
    u8 packet[TRANSPORT_PACKET_BYTES];
    u8 pes[PAYLOAD_BYTES_PLAIN + 32];
    u64 now = current_time_ms();
    int wantPcr, headerBytes, room, first, offset;

    /* RFC 2250 section 2: for a transport stream the RTP timestamp is a
     * transmission-time clock tied to the PCR, not the presentation time. Here
     * the PCR, the PTS and the start of the unit's slot are one instant, so the
     * PTS is that clock. Keeping that identity is a decision, not a coincidence:
     * see docs/development/pathb-wire-format.md. */
    mux->rtpTimestamp = (u32)presentationTicks;

    if (mux->programAssociationLastMs == 0 || now - mux->programAssociationLastMs >= PSI_REPEAT_INTERVAL_MS) {
        pack_program_association(packet, mux->transportStreamId, mux->pmtPid, mux->continuityPat++ & 0x0F);
        mux_push(runtime, packet);
        pack_program_map(packet, mux->pmtPid, mux->pcrPid, mux->elementaryPid, mux->continuityPmt++ & 0x0F);
        mux_push(runtime, packet);
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
     * datagram records the unit bytes it actually carried and its deadline is
     * proportional to real progress. PSI packets contribute nothing. */
    pipe->unitPayloadDone += (u64)(unsigned)first;
    mux_push(runtime, packet);

    offset = first;
    while (offset < bytes) {
        int chunk = bytes - offset;
        if (chunk > PAYLOAD_BYTES_PLAIN) chunk = PAYLOAD_BYTES_PLAIN;
        if (chunk == PAYLOAD_BYTES_UNENCODABLE) chunk = PAYLOAD_BYTES_WITH_RAI;
        pack_transport_packet(packet, mux->elementaryPid, 0, mux->continuityEs++ & 0x0F,
                              unit + offset, chunk, 0, 0, 0);
        pipe->unitPayloadDone += (u64)(unsigned)chunk;
        mux_push(runtime, packet);
        offset += chunk;
    }
    mux_end_access_unit(runtime);
}

/* --------------------------------------------------------- 9. the pipe's clock
 * The pipe IS the schedule: every datagram carries the instant it may go out, and
 * the poll timeout is the next of those instants. Release is bounded per call so
 * the reader is never held off, and nothing sleeps inside the write path. */

int pipe_release_due(Runtime *runtime, u64 nowNs)
{
    Pipe *pipe = &runtime->stream.pipe;
    int released = 0;

    while (pipe->cursor < pipe->count && released < PIPE_BURST_DATAGRAMS) {
        Datagram *datagram = &pipe->datagrams[pipe->cursor];
        nowNs = current_time_ns();
        if (nowNs < datagram->deadlineNs) break;
#if PATHB_PACE_VARIANT >= 3
        /* THE WIRE-RATE FLOOR. A reader stall (a large access unit arriving) can
         * leave a batch of deadlines overdue; writing them back to back is the
         * clump the receiver's ring drops on. The floor is the same 2000 B/ms
         * ceiling the slots use, applied at the point of emission, so a stalled
         * reader costs latency and never density. */
        {
            u64 minimal = (u64)datagram->length * 1000000ull / OUTPUT_RATE_CEILING_BYTES_PER_MS;
            if (pipe->lastEmissionNs && nowNs < pipe->lastEmissionNs + minimal) break;
        }
#endif
        stream_write_datagram(runtime,
                              pipe->bytes + (usize_t)pipe->cursor * (usize_t)pipe->stride,
                              (int)datagram->length);
#if PATHB_PACE_VARIANT >= 3
        pipe->lastEmissionNs = nowNs;
#endif
        {
            u64 depth = pipe->nextSlotStartNs > nowNs ? pipe->nextSlotStartNs - nowNs : 0;
            trace_message(runtime, "E\t%lu\t%u\t%lu\t%lu\t%lu\t%lu\t%lu",
                          nowNs, (unsigned)datagram->length, datagram->deadlineNs,
                          datagram->arrivalNs,
                          nowNs > datagram->arrivalNs ? (nowNs - datagram->arrivalNs) / 1000000ull : 0,
                          depth / 1000000ull,
                          runtime->session.paceIntervalMs);
        }
        if (!pipe->latencyRecorded) {
            /* How long this picture waited in OUR buffer: the share of "it takes
             * a second to react" that belongs to us. One sample per unit. */
            u64 ageMs = nowNs > datagram->arrivalNs ? (nowNs - datagram->arrivalNs) / 1000000ull : 0;
            ring_push(&runtime->session.latency.samples, ageMs);
            if (ageMs > runtime->session.latency.maximum) runtime->session.latency.maximum = ageMs;
            pipe->latencyRecorded = 1;
        }
        pipe->cursor++;
        released++;
    }
    if (pipe->cursor >= pipe->count) {
        pipe->count = pipe->cursor = 0;
        return 0;
    }
    return 1;
}

/* Push the remainder out now. Used at connection close: closing with datagrams
 * still scheduled silently discarded the phone's last pictures. */
void pipe_release_all(Runtime *runtime)
{
    Pipe *pipe = &runtime->stream.pipe;
    while (pipe->cursor < pipe->count) {
        Datagram *datagram = &pipe->datagrams[pipe->cursor];
        stream_write_datagram(runtime,
                              pipe->bytes + (usize_t)pipe->cursor * (usize_t)pipe->stride,
                              (int)datagram->length);
        pipe->cursor++;
    }
    pipe->count = pipe->cursor = 0;
}

/* How long the loop may block: the next datagram's deadline, capped at the
 * loop's historical 2 ms cadence so EOF detection keeps its timing. */
u64 pipe_poll_timeout_ns(Runtime *runtime)
{
    Pipe *pipe = &runtime->stream.pipe;
    u64 now, due;

    if (pipe->cursor >= pipe->count) return PIPE_POLL_CAP_NS;
    due = pipe->datagrams[pipe->cursor].deadlineNs;
#if PATHB_PACE_VARIANT >= 3
    /* The release-time floor is also a wake-up time; without this the loop
     * spins on ppoll(0) until the floor clears. */
    if (pipe->lastEmissionNs) {
        u64 minimal = (u64)pipe->datagrams[pipe->cursor].length
                    * 1000000ull / OUTPUT_RATE_CEILING_BYTES_PER_MS;
        if (pipe->lastEmissionNs + minimal > due) due = pipe->lastEmissionNs + minimal;
    }
#endif
    now = current_time_ns();
    if (due <= now) return 0;
    return (due - now) < PIPE_POLL_CAP_NS ? (due - now) : PIPE_POLL_CAP_NS;
}

/* How far ahead the schedule runs: the standing depth of the buffer, in ns. */
u64 pipe_pending_ns(Runtime *runtime)
{
    Pipe *pipe = &runtime->stream.pipe;
    if (!pipe->nextSlotStartNs) return 0;
    {   u64 now = current_time_ns();
        return pipe->nextSlotStartNs > now ? pipe->nextSlotStartNs - now : 0; }
}

/* ------------------------------------------------- 10. the access-unit walk
 * One pass over a unit answers everything: is it a parameter-set record, does it
 * carry VCL, is there an IDR, where does the last SPS start. */

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
 * record-per-PES muxer would emit two PES per frame, and every stream this ECU
 * decodes carries the parameter sets inside the same unit as the slice. A
 * VCL-free record is held and prepended to the next record that carries VCL. */

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
 * A steady clock, not a wall clock: the timeline advances by a bounded constant
 * step per unit, slewed against wall time, so a phone silence is never a PCR
 * discontinuity. Kept independent of the slot schedule: see
 * docs/development/pathb-pacing-and-timing.md. */

static void clock_advance(Runtime *runtime, u64 *presentationTicks)
{
    MediaClock *clock = &runtime->session.clock;
    Mux *mux = &runtime->stream.mux;
    u64 step = clock->stepTicks;

    if (step < MEDIA_STEP_FLOOR_TICKS) step = MEDIA_STEP_FLOOR_TICKS;
    if (step > MEDIA_STEP_CEILING_TICKS) step = MEDIA_STEP_CEILING_TICKS;

    /* SLEW, DON'T JUMP: correct the integrated error by at most a tenth per unit,
     * and re-anchor instead of racing when the error is a pause, not drift. */
    if (!clock->wallBaseMs) {
        clock->wallBaseMs = current_time_ms();
        clock->ptsBaseTicks = mux->presentationTimeTicks;
    } else {
        u64 wall = current_time_ms() - clock->wallBaseMs;
        u64 media = (mux->presentationTimeTicks - clock->ptsBaseTicks) / TICKS_PER_MILLISECOND;
        long error = (long)wall - (long)media;
        if (error > MEDIA_DRIFT_REBASE_MS || error < -MEDIA_DRIFT_REBASE_MS) {
            clock->wallBaseMs = current_time_ms();
            clock->ptsBaseTicks = mux->presentationTimeTicks;
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
    mux->presentationTimeTicks += step;
    *presentationTicks = mux->presentationTimeTicks;
}

/* ----------------------------------------------------------- 14. the session */

void runtime_initialise(Runtime *runtime, Options *options)
{
    memset(runtime, 0, sizeof *runtime);
    runtime->options = *options;
    runtime->spreadPercent = options->spreadPercent;
    runtime->timingEnabled = options->statisticsTiming;
    runtime->traceFd = -1;

    if (options->tracePath && *options->tracePath) {
        runtime->traceFd = open(options->tracePath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (runtime->traceFd < 0) log_message("--trace: cannot open %s", options->tracePath);
    }

    runtime->session.jitterMs = options->jitterMs;
    runtime->session.noFold = options->noFold;
    runtime->session.noIdrGate = options->noIdrGate;

    runtime->session.cadence.arrival.values = g_cadenceArrivalValues;
    runtime->session.cadence.arrival.capacity = CADENCE_RING_CAPACITY;
    runtime->session.cadence.emission.values = g_cadenceEmissionValues;
    runtime->session.cadence.emission.capacity = CADENCE_RING_CAPACITY;
    runtime->session.latency.samples.values = g_latencyValues;
    runtime->session.latency.samples.capacity = LATENCY_RING_CAPACITY;
    runtime->timing.ring.values = g_timingValues;
    runtime->timing.ring.capacity = TIMING_RING_CAPACITY;

    if (options->statePath && *options->statePath && strcmp(options->statePath, "off") != 0) {
        usize_t length = strlen(options->statePath);
        if (length < sizeof runtime->supervisor.path) {
            memcpy(runtime->supervisor.path, options->statePath, length);
            runtime->supervisor.path[length] = 0;
        } else {
            log_message("--conn-state: path too long, publisher disabled");
        }
    }
    if (options->controlPath && *options->controlPath) {
        usize_t length = strlen(options->controlPath);
        if (length < sizeof runtime->control.path) {
            memcpy(runtime->control.path, options->controlPath, length);
            runtime->control.path[length] = 0;
        } else {
            log_message("--ctl: path too long, control channel disabled");
        }
    }
}

void session_reset(Runtime *runtime)
{
    Session *session = &runtime->session;
    Pipe *pipe = &runtime->stream.pipe;

    parameter_set_reset(&session->parameters);
    runtime->stream.mux.programAssociationLastMs = 0;
    pipe->nextSlotStartNs = 0;
    pipe->lastEmissionNs = 0;
    pipe->count = pipe->cursor = 0;
    session->headerPending = 1;
    session->streaming = 0;
    session->lastArrivalMs = 0;
    session->arrivalGaps = 0;
    session->rateWindowStartMs = 0;
    session->rateWindowCount = 0;
    session->estimateMs = 0;
    session->estimateBandWindows = 0;
    session->paceIntervalMs = FALLBACK_INTERVAL_MS;
    session->paceBaseRate = 1000.0 / (double)FALLBACK_INTERVAL_MS;
    session->paceIntegral = 0.0;
    session->paceControlMs = 0;
    session->paceTrim = 0.0;
    session->paceDepthAvg = 0.0;
    session->paceArrivalRate = 0.0;
    session->paceArrivalCount = 0;
    session->paceBankMs = 0;
    session->paceLastStarveMs = 0;
    session->paceHoleCounted = 0;
    session->paceLowTicks = 0;
    session->paceHighTicks = 0;
    session->cadenceReportMs = 0;

    ring_reset(&session->cadence.arrival);
    ring_reset(&session->cadence.emission);
    session->cadence.lastEmissionMs = 0;
    ring_reset(&session->latency.samples);
    session->latency.maximum = 0;
    session->latency.lastReportMs = 0;

    session->previousLate = 0;
    session->previousCatchUps = 0;
    session->previousDropped = 0;
    session->previousAccessUnitsIn = 0;
    session->previousAccessUnitsOut = 0;
}

/* Mux one unit and give it a slot.
 *
 * The schedule is the rate, not a depth: in advance, assign it the next free
 * slot; if the schedule has fallen behind, its slot starts now and the late
 * frames catch up by themselves. The only depth control is the bound: when the
 * schedule runs further ahead than the latency allowance, all pending deadlines
 * are pulled forward together, which preserves their spacing exactly (no clump)
 * while shortening the buffer. */
void session_emit_access_unit(Runtime *runtime, const u8 *unit, usize_t length)
{
    Stream *stream = &runtime->stream;
    Pipe *pipe = &stream->pipe;
    Session *session = &runtime->session;
    u64 nowNs, nowMs, presentationTicks, intervalNs, slotNs, startNs, advanceNs;

    nowNs = current_time_ns();
    nowMs = nowNs / 1000000ull;

    {   /* emission-rhythm sample: this is the instant the unit started leaving. */
        if (session->cadence.lastEmissionMs && nowMs >= session->cadence.lastEmissionMs &&
            nowMs - session->cadence.lastEmissionMs <= 1000)
            ring_push(&session->cadence.emission, nowMs - session->cadence.lastEmissionMs);
        session->cadence.lastEmissionMs = nowMs;
    }
    runtime->supervisor.accessUnitsOut++;

    clock_advance(runtime, &presentationTicks);

    /* Make room for the whole frame BEFORE muxing it, so a full pipe drops a
     * frame rather than leaving half of one on the wire. */
    pipe_compact(pipe);
    if (!pipe_reserve(pipe, (int)(length / PAYLOAD_BYTES_PLAIN) + 4)) {
        pipe->statisticsDropped++;
        trace_message(runtime, "C\t%lu\tdrop\t%lu", nowMs, (u64)length);
        return;
    }

    /* The interval is the loop's output. The estimate is only its seed. */
    intervalNs = session->paceIntervalMs * 1000000ull;
#if PATHB_PACE_VARIANT >= 3
    /* THE DEPTH BUDGET. A unit may not schedule the stream further ahead than
     * the budget. If it would, this unit gets a shorter slot: the burst is
     * drained by rate, and the schedule never grows into a queue. The budget
     * rides the ratcheted bank, so it is a latency cap that loosens only while
     * the source is being silenced. */
    {
        u64 bankMs = session->paceBankMs ? session->paceBankMs : (u64)session->jitterMs;
        u64 depthNs = pipe->nextSlotStartNs > nowNs ? pipe->nextSlotStartNs - nowNs : 0;
        u64 budgetNs = (bankMs + PACE_DEPTH_BUDGET_MS) * 1000000ull;
        if (depthNs >= budgetNs)
            intervalNs = (u64)MINIMUM_INTERVAL_MS * 1000000ull;
        else if (depthNs + intervalNs > budgetNs)
            intervalNs = budgetNs - depthNs;
    }
#endif
    if (!session->jitterMs) {
        /* --no-jit: the unspaced control. Every datagram goes out at once. */
        pipe->unitPaced = 0;
        pipe->statisticsUnpaced++;
        startNs = nowNs;
        slotNs = 0;
        pipe->nextSlotStartNs = 0;
    } else {
        pipe->unitPaced = 1;
        slotNs = intervalNs * runtime->spreadPercent / 100u;
        {   /* Rate ceiling: a frame may never leave faster than the reference
             * sender, however urgently the schedule wants it gone. */
            u64 ceilingNs = (u64)length * 1000000ull / OUTPUT_RATE_CEILING_BYTES_PER_MS;
            if (ceilingNs > slotNs) slotNs = ceilingNs;
        }
        if (!slotNs) slotNs = intervalNs;

        if (!pipe->nextSlotStartNs) {
            /* First unit after the gate opened: start one buffer depth in, so the
             * phone's first hole does not starve the wire immediately. */
            startNs = nowNs + (u64)session->jitterMs * 1000000ull;
        } else {
#if PATHB_PACE_VARIANT >= 3
            /* The budget has already bounded the schedule; the shift is only a
             * last-resort safety net, so it sits above the budget. */
            u64 bankMs = session->paceBankMs ? session->paceBankMs : (u64)session->jitterMs;
            u64 highWaterMs = bankMs + PACE_DEPTH_BUDGET_MS + 50u;
#else
            u64 highWaterMs = session->jitterMs > (int)LATENCY_CAP_MS
                            ? (u64)session->jitterMs : (u64)LATENCY_CAP_MS;
#endif
            u64 highWaterNs = highWaterMs * 1000000ull;
            u64 lag = pipe->nextSlotStartNs > nowNs ? pipe->nextSlotStartNs - nowNs : 0;

            if (lag > highWaterNs) {
                u64 shift = lag - highWaterNs;
                for (int index = pipe->cursor; index < pipe->count; index++) {
                    u64 deadline = pipe->datagrams[index].deadlineNs;
                    pipe->datagrams[index].deadlineNs =
                        deadline > shift + nowNs ? deadline - shift : nowNs;
                }
                pipe->nextSlotStartNs = pipe->nextSlotStartNs > shift
                                      ? pipe->nextSlotStartNs - shift : nowNs;
                pipe->statisticsCatchUps++;
                trace_message(runtime, "C\t%lu\tcatchup\t%lu\t%lu",
                              nowMs, shift / 1000000ull, lag / 1000000ull);
            }
            if (pipe->nextSlotStartNs <= nowNs) pipe->statisticsLate++;
            startNs = pipe->nextSlotStartNs > nowNs ? pipe->nextSlotStartNs : nowNs;
        }
        advanceNs = intervalNs > slotNs ? intervalNs : slotNs;
        pipe->nextSlotStartNs = startNs + advanceNs;
    }

    pipe->unitStartNs = startNs;
    pipe->unitLengthNs = slotNs;
    pipe->unitArrivalNs = nowNs;
    pipe->unitPayloadTotal = (u64)length;
    pipe->unitPayloadDone = 0;
    pipe->unitPayloadFlushed = 0;
    pipe->latencyRecorded = 0;

    trace_message(runtime, "U\t%lu\t%lu\t%lu\t%lu\t%lu",
                  nowNs, (u64)length, startNs, slotNs, session->paceIntervalMs);
    mux_access_unit(runtime, unit, (int)length, presentationTicks);
    session->clock.frames++;
}

/* The interval estimate and the reports. The interval is measured by COUNTING
 * arrivals over a window, never by averaging gaps: a mean is dominated by the
 * many short ones and reads 15% too fast. It moves only when it must -- inside
 * +/-10% it is left alone, outside it the new value has to hold for two windows
 * and is then blended, so the interval the eye reads as judder cannot jump. */
/* The rate loop. Depth is the integral of (source rate - output rate): it is the
 * phase error of a phase-locked loop whose oscillator is the output rate. One PI
 * filter on that single error sets the rate -- P supplies the damping, I removes
 * the steady-state bias. The arrival-count estimate is only the feedforward term
 * the loop starts from; it does not have to be accurate, only close.
 *
 *   error     = target - depth                    (ms, positive = too shallow)
 *   rate      = baseRate - kp*error - ki*integral (frames per second)
 *   integral += error * dt                        (ms * s)
 *
 * The integrator is frozen while the pipe is empty. A pause is not a rate error,
 * and winding up on silence makes the first frame after it far too slow. */
static void pace_control(Runtime *runtime, u64 now)
{
    Session *session = &runtime->session;
    double error, rate, dt;

    if (!session->jitterMs) return;                  /* --no-jit: unpaced */
    if (!session->paceControlMs) { session->paceControlMs = now; return; }
    if (now - session->paceControlMs < PACE_PERIOD_MS) return;

    dt = (double)(now - session->paceControlMs) / 1000.0;
    session->paceControlMs = now;

    /* Arrival feedforward: how fast the source is actually delivering right now.
     * Counted per tick, never averaged from gaps: a gap mean ignores the holes
     * and reads the burst, not the stream. */
    {
        double instant = session->paceArrivalCount / (dt > 0.01 ? dt : 0.01);
        session->paceArrivalRate = session->paceArrivalRate
            ? session->paceArrivalRate * 0.75 + instant * 0.25
            : instant;
        session->paceArrivalCount = 0;
    }

    error = (double)session->jitterMs - (double)(pipe_pending_ns(runtime) / 1000000ull);
    (void)error;

#if PATHB_PACE_VARIANT == 0
    /* No rate feedback: the estimate is the only authority. */
    rate = session->paceBaseRate;

#elif PATHB_PACE_VARIANT == 1
    /* The clamped proportional term. The clamp bounds the correction at 15%, so
     * a biased estimate leaves a standing depth error the loop cannot remove. */
    rate = session->paceBaseRate - PACE_GAIN_PROPORTIONAL * error;
    if (rate < session->paceBaseRate * 0.85) rate = session->paceBaseRate * 0.85;
    if (rate > session->paceBaseRate * 1.15) rate = session->paceBaseRate * 1.15;

#elif PATHB_PACE_VARIANT == 2
    /* The PI-on-depth build. Integral action against a signal that is zero
     * during every phone silence winds up while the pipe is empty and unwinds
     * into the next burst. */
    if (runtime->stream.pipe.count > 0) {
        session->paceIntegral += error * dt;
        if (session->paceIntegral >  PACE_INTEGRAL_LIMIT) session->paceIntegral =  PACE_INTEGRAL_LIMIT;
        if (session->paceIntegral < -PACE_INTEGRAL_LIMIT) session->paceIntegral = -PACE_INTEGRAL_LIMIT;
    }
    rate = session->paceBaseRate
         - PACE_GAIN_PROPORTIONAL * error
         - PACE_GAIN_INTEGRAL * session->paceIntegral;

#else
    /* THE PRODUCTION CONTROLLER.
     *
     * Depth is a phase, so it is corrected by a small rate trim -- but the rate
     * itself comes from how often units actually arrive, never from the phase.
     * Three rules follow, and they are the whole design:
     *
     *  1. The phase signal is FILTERED (2 s). Correcting the burst cycle itself
     *     is what makes the adopted interval swing; the interval is the judder
     *     the eye sees, so the loop must not chase a clump.
     *  2. While the pipe is empty the rate is nominal and the trim is frozen.
     *     Emptiness is not a rate error: nothing has arrived to schedule, and
     *     slowing down here is what turned the next burst into an overshoot.
     *  3. The depth budget (enforced when a unit is scheduled) is what drains a
     *     burst: a unit that would push the schedule past the budget gets a
     *     shorter slot, not a longer queue.
     *
     * The slow integrator the car rejected is replaced by the phase governor
     * above: small steps, only on a sustained phase error, never while the pipe
     * is empty. It trims the estimate's bias without winding up on a pause. */
    {
        int empty = runtime->stream.pipe.count == 0;
        double nominal;
        double depth_ms = (double)(pipe_pending_ns(runtime) / 1000000ull);
        double rate, relative;

        /* THE BANK RATCHET (see the header block). The trigger is the SOURCE
         * going quiet past the cadence -- the arrival clock, not the pipe:
         * detecting the pipe emptying fires a bank-length into the hole and
         * only catches the deepest ones. One step per hole, relax on calm. */
        if (!session->paceBankMs) session->paceBankMs = (u64)session->jitterMs;
        {
            u64 quiet = session->lastArrivalMs ? now - session->lastArrivalMs : 0;
            if (quiet >= PACE_HOLE_MS) {
                if (!session->paceHoleCounted) {
                    session->paceHoleCounted = 1;
                    session->paceLastStarveMs = now;
                    if (session->paceBankMs < PACE_BANK_MAX_MS) {
                        session->paceBankMs += PACE_BANK_STEP_MS;
                        trace_message(runtime, "C\t%lu\tbank\t%lu\t%lu", now,
                                      session->paceBankMs, quiet);
                    }
                }
            } else {
                session->paceHoleCounted = 0;
                if (session->paceBankMs > (u64)session->jitterMs
                    && session->paceLastStarveMs
                    && now - session->paceLastStarveMs >= PACE_CALM_MS) {
                    session->paceBankMs -= PACE_RELAX_MS;
                    if (session->paceBankMs < (u64)session->jitterMs)
                        session->paceBankMs = (u64)session->jitterMs;
                    session->paceLastStarveMs = now;   /* one relax per calm period */
                    trace_message(runtime, "C\t%lu\trelax\t%lu\t%lu", now,
                                  session->paceBankMs, quiet);
                }
            }
        }

        /* THE PHASE GOVERNOR. The count estimate is only a starting point: it
         * can be biased, and after a rate change it is stale. The filtered phase
         * cannot be biased -- if depth has sat below 70% of the bank for two
         * seconds the base rate is too fast, whatever the count says. One small
         * step per two seconds, skipped while the pipe is empty, bounded in
         * authority: this replaces the integrator that wound up on the car. */
#if PATHB_PACE_VARIANT >= 3
        if (!empty) {
            double low = (double)session->paceBankMs * PACE_GOVERNOR_LOW_PERCENT / 100.0;
            double high = (double)session->paceBankMs * PACE_GOVERNOR_HIGH_PERCENT / 100.0;
            if (session->paceDepthAvg > 0.0 && session->paceDepthAvg < low)
                session->paceLowTicks++;
            else
                session->paceLowTicks = 0;
            if (session->paceDepthAvg > high)
                session->paceHighTicks++;
            else
                session->paceHighTicks = 0;
        }
        if (session->paceLowTicks >= PACE_GOVERNOR_TICKS) {
            /* A starved phase steps harder: under a fifth of the bank for two
             * seconds means the count estimate has not admitted the source's
             * real rate at all, and every 100 ms spent too fast is wire
             * starvation the ECU sees. */
            double step = (session->paceDepthAvg
                           < (double)session->paceBankMs * PACE_GOVERNOR_STARVE_PERCENT / 100.0)
                          ? PACE_GOVERNOR_DOWN_HARD : PACE_GOVERNOR_DOWN;
            session->paceLowTicks = 0;
            session->paceBaseRate *= step;
            if (session->paceBaseRate < 1.0) session->paceBaseRate = 1.0;
            session->estimateMs = (u64)(1000.0 / session->paceBaseRate + 0.5);
            if (session->estimateMs < MINIMUM_INTERVAL_MS) session->estimateMs = MINIMUM_INTERVAL_MS;
            if (session->estimateMs > MAXIMUM_INTERVAL_MS) session->estimateMs = MAXIMUM_INTERVAL_MS;
            session->rateWindowStartMs = now;
            session->rateWindowCount = 0;
            trace_message(runtime, "C\t%lu\tslow\t%lu\t%lu", now,
                          (u64)(session->paceBaseRate * 1000.0),
                          (u64)(session->paceDepthAvg + 0.5));
        } else if (session->paceHighTicks >= PACE_GOVERNOR_TICKS) {
            session->paceHighTicks = 0;
            session->paceBaseRate *= PACE_GOVERNOR_UP;
            if (session->paceBaseRate > 100.0) session->paceBaseRate = 100.0;
            session->estimateMs = (u64)(1000.0 / session->paceBaseRate + 0.5);
            if (session->estimateMs < MINIMUM_INTERVAL_MS) session->estimateMs = MINIMUM_INTERVAL_MS;
            if (session->estimateMs > MAXIMUM_INTERVAL_MS) session->estimateMs = MAXIMUM_INTERVAL_MS;
            session->rateWindowStartMs = now;
            session->rateWindowCount = 0;
            trace_message(runtime, "C\t%lu\tfast\t%lu\t%lu", now,
                          (u64)(session->paceBaseRate * 1000.0),
                          (u64)(session->paceDepthAvg + 0.5));
        }
#endif

        nominal = session->paceBaseRate;

        if (!empty) {
            /* Asymmetric phase filter: fall fast (order the refill at once),
             * rise slowly (never speed up into the silence that follows a
             * burst -- that is the measured 550 ms starvation mechanism). */
            double bank = (double)session->paceBankMs;
            double tau = depth_ms < bank
                       ? (double)PACE_DEPTH_TAU_LOW_MS
                       : (double)PACE_DEPTH_TAU_HIGH_MS;
            double alpha = dt * 1000.0 / tau;
            if (alpha > 1.0) alpha = 1.0;
            if (session->paceDepthAvg <= 0.0) session->paceDepthAvg = bank;
            session->paceDepthAvg = session->paceDepthAvg * (1.0 - alpha) + depth_ms * alpha;
        }

        relative = (session->paceDepthAvg - (double)session->paceBankMs)
                 / (double)session->paceBankMs;
        if (relative >  PACE_DEPTH_DEADBAND_PERCENT / 100.0)
            relative -= PACE_DEPTH_DEADBAND_PERCENT / 100.0;
        else if (relative < -PACE_DEPTH_DEADBAND_PERCENT / 100.0)
            relative += PACE_DEPTH_DEADBAND_PERCENT / 100.0;
        else
            relative = 0.0;

        session->paceTrim = empty ? 0.0
            : PACE_TRIM_GAIN * relative;
        {
            double limit = PACE_TRIM_LIMIT;
            if (session->paceDepthAvg
                < (double)session->paceBankMs * PACE_TRIM_DEEP_PERCENT / 100.0)
                limit = PACE_TRIM_DEEP_LIMIT;
            if (session->paceTrim >  limit) session->paceTrim =  limit;
            if (session->paceTrim < -limit) session->paceTrim = -limit;
        }

        rate = nominal * (1.0 + session->paceTrim);
        if (rate < nominal * 0.30) rate = nominal * 0.30;   /* refill floor    */
        if (rate > nominal * 1.60) rate = nominal * 1.60;   /* trim ceiling     */
        {
            /* Speed up at once (a burst must be met); slow down at a bounded
             * pace, fast enough that a refill completes between two holes. */
            u64 wanted = (u64)(1000.0 / rate + 0.5);
            u64 slowest;
            if (wanted < MINIMUM_INTERVAL_MS) wanted = MINIMUM_INTERVAL_MS;
            if (wanted > MAXIMUM_INTERVAL_MS) wanted = MAXIMUM_INTERVAL_MS;
            /* Until the first count window has closed (or the governor has
             * taught us something), the source rate is unknown: bias the
             * opening pace SLOW. Too slow costs depth, which the budget drains
             * at the wire floor; too fast starves the wire, which nothing
             * downstream can repair. */
            if (!session->estimateMs && wanted < PACE_STARTUP_INTERVAL_MS)
                wanted = PACE_STARTUP_INTERVAL_MS;
            slowest = session->paceIntervalMs
                    + session->paceIntervalMs * PACE_RATE_SLEW_PERCENT / 100u + 1;
            if (wanted > slowest) wanted = slowest;
            session->paceIntervalMs = wanted;
            trace_message(runtime, "P\t%lu\t%lu\t%lu\t%lu\t%lu\t%d\t%lu\t%lu\t%lu\t%lu",
                          now,
                          pipe_pending_ns(runtime) / 1000000ull,
                          session->paceIntervalMs,
                          session->estimateMs ? session->estimateMs : FALLBACK_INTERVAL_MS,
                          (u64)(session->paceBaseRate * 1000.0 + 0.5),
                          runtime->stream.pipe.count,
                          (u64)(session->paceArrivalRate * 1000.0 + 0.5),
                          (u64)((session->paceTrim + 1.0) * 1000.0 + 0.5),
                          (u64)(session->paceDepthAvg + 0.5),
                          session->paceBankMs);
        }
        return;
    }
#endif

    if (rate < 1000.0 / (double)MAXIMUM_INTERVAL_MS) rate = 1000.0 / (double)MAXIMUM_INTERVAL_MS;
    if (rate > 1000.0 / (double)MINIMUM_INTERVAL_MS) rate = 1000.0 / (double)MINIMUM_INTERVAL_MS;

    {
        u64 wanted = (u64)(1000.0 / rate + 0.5);
        if (wanted < MINIMUM_INTERVAL_MS) wanted = MINIMUM_INTERVAL_MS;
        if (wanted > MAXIMUM_INTERVAL_MS) wanted = MAXIMUM_INTERVAL_MS;
#if PATHB_PACE_VARIANT >= 3
        /* Accelerate immediately (a burst must be met), decelerate gently (a
         * rate that can jump back and forth is the limit cycle). */
        {
            u64 previous = session->paceIntervalMs;
            u64 slowest = previous + previous * PACE_RATE_SLEW_PERCENT / 100u + 1;
            if (wanted > slowest) wanted = slowest;
        }
#endif
        session->paceIntervalMs = wanted;
    }

    trace_message(runtime, "P\t%lu\t%lu\t%lu\t%lu\t%lu\t%d\t%lu\t%lu",
                  now,
                  pipe_pending_ns(runtime) / 1000000ull,
                  session->paceIntervalMs,
                  session->estimateMs ? session->estimateMs : FALLBACK_INTERVAL_MS,
                  (u64)(session->paceBaseRate * 1000.0 + 0.5),
                  runtime->stream.pipe.count,
                  (u64)(session->paceArrivalRate * 1000.0 + 0.5),
                  (u64)((session->paceTrim + 1.0) * 1000.0 + 0.5));
}

/* The estimate and the reports. The estimate is measured by COUNTING arrivals
 * over a window, never by averaging gaps: a mean is dominated by the many short
 * ones. It is the loop's feedforward term, so it is smoothed but not trusted. */
void session_maintain(Runtime *runtime)
{
    Session *session = &runtime->session;
    u64 now = current_time_ms();

    if (session->rateWindowCount && session->rateWindowStartMs) {
        u64 window = now - session->rateWindowStartMs;
        u64 windowMs = CLOCK_RATE_WINDOW_MS;
#if PATHB_PACE_VARIANT >= 3
        /* A hole every ~2 s makes a 4 s count window alternate between one hole
         * and two; that is a rate estimate that swings by 25% with nothing
         * changing. Eight seconds averages four holes, and the jump path below
         * still adopts a genuine step immediately. */
        windowMs = 8000;
#endif
        if (window >= windowMs) {
            u64 measured = window / session->rateWindowCount;
            if (!session->estimateMs) {
#if PATHB_PACE_VARIANT >= 3
                /* The first window is the least trustworthy one: at session
                 * open it averages the phone's ramp with its steady rate. Bias
                 * it slow for the same reason the opening pace is clamped --
                 * excess depth is drained by the budget, starvation is not
                 * repairable. */
                if (measured < PACE_STARTUP_INTERVAL_MS) measured = PACE_STARTUP_INTERVAL_MS;
#endif
                session->estimateMs = measured;
            } else {
                u64 low = session->estimateMs - session->estimateMs / CLOCK_HYSTERESIS_PERCENT;
                u64 high = session->estimateMs + session->estimateMs / CLOCK_HYSTERESIS_PERCENT;
                if (measured >= low && measured <= high) {
                    session->estimateBandWindows = 0;
                } else if (++session->estimateBandWindows >= CLOCK_BAND_WINDOWS) {
#if PATHB_PACE_VARIANT >= 3
                    /* A genuine rate change is adopted, not blended: half of a
                     * 3x step is still half wrong, and the loop then spends
                     * seconds chasing it. Anything smaller still has to hold
                     * for two windows. */
                    session->estimateMs = measured;
#else
                    session->estimateMs = (session->estimateMs + measured) / 2;
#endif
                    session->estimateBandWindows = 0;
                } else {
#if PATHB_PACE_VARIANT >= 3
                    /* The one-window jump path is for SLOW-DOWNS only. A window
                     * that says "faster" may just be straddling a burst (the
                     * opening 57 fps ramp averaged into the steady rate is
                     * exactly that), and pacing too fast drains the wire --
                     * the one error nothing downstream can repair. A speed-up
                     * waits for the two-window path; if it is real, the cost is
                     * depth, which the budget drains at the wire floor. */
                    u64 difference = measured > session->estimateMs
                                   ? measured - session->estimateMs
                                   : session->estimateMs - measured;
                    if (measured > session->estimateMs
                        && difference * 100u > session->estimateMs * PACE_ESTIMATE_JUMP_PERCENT) {
                        session->estimateMs = measured;
                        session->estimateBandWindows = 0;
                    }
#endif
                }
            }
            if (session->estimateMs < MINIMUM_INTERVAL_MS) session->estimateMs = MINIMUM_INTERVAL_MS;
            if (session->estimateMs > MAXIMUM_INTERVAL_MS) session->estimateMs = MAXIMUM_INTERVAL_MS;
            session->paceBaseRate = 1000.0 / (double)session->estimateMs;
            session->rateWindowStartMs = now;
            session->rateWindowCount = 0;
        }
    }

    pace_control(runtime, now);

    if (now - session->latency.lastReportMs >= LATENCY_REPORT_INTERVAL_MS &&
        session->latency.samples.count) {
        u64 p50, p95, maximum;
        ring_percentiles(&session->latency.samples, &p50, &p95, &maximum);
        session->latency.lastReportMs = now;
        log_message("lat: ours p50=%ld p95=%ld max=%ld ms  depth=%ld ms  interval=%ld ms",
                    (long)p50, (long)p95, (long)session->latency.maximum,
                    (long)(pipe_pending_ns(runtime) / 1000000ull),
                    (long)session->paceIntervalMs);
    }

    if (now - session->cadenceReportMs >= CADENCE_REPORT_INTERVAL_MS) {
        u64 arrival50, arrival95, arrivalMax, emission50, emission95, emissionMax;
        Pipe *pipe = &runtime->stream.pipe;
        u64 lateDelta = pipe->statisticsLate - session->previousLate;
        u64 catchUpDelta = pipe->statisticsCatchUps - session->previousCatchUps;
        u64 dropDelta = pipe->statisticsDropped - session->previousDropped;
        u64 inDelta = runtime->supervisor.accessUnitsIn - session->previousAccessUnitsIn;
        u64 outDelta = runtime->supervisor.accessUnitsOut - session->previousAccessUnitsOut;

        ring_percentiles(&session->cadence.arrival, &arrival50, &arrival95, &arrivalMax);
        ring_percentiles(&session->cadence.emission, &emission50, &emission95, &emissionMax);
        log_message("cad: arr p50=%ld p95=%ld max=%ld | emi p50=%ld p95=%ld max=%ld"
                    " | au=%ld out=%ld seqgap=%ld lost=%ld late=%ld catchup=%ld drop=%ld"
                    " | depth=%ld ms  interval=%ld ms  base=%ld ms",
                    (long)arrival50, (long)arrival95, (long)arrivalMax,
                    (long)emission50, (long)emission95, (long)emissionMax,
                    (long)inDelta, (long)outDelta,
                    (long)session->cadence.sequenceGaps, (long)session->cadence.lostFrames,
                    (long)lateDelta, (long)catchUpDelta, (long)dropDelta,
                    (long)(pipe_pending_ns(runtime) / 1000000ull),
                    (long)session->paceIntervalMs,
                    (long)(session->estimateMs ? session->estimateMs : FALLBACK_INTERVAL_MS));
        session->previousLate = pipe->statisticsLate;
        session->previousCatchUps = pipe->statisticsCatchUps;
        session->previousDropped = pipe->statisticsDropped;
        session->previousAccessUnitsIn = runtime->supervisor.accessUnitsIn;
        session->previousAccessUnitsOut = runtime->supervisor.accessUnitsOut;
        session->cadenceReportMs = now;
        ring_reset(&session->cadence.arrival);
        ring_reset(&session->cadence.emission);
    }
}

/* One record from the shim. Returns 0 when the connection is finished (EOF, a
 * short read, a malformed frame) and 1 to continue. */
int session_read_record(Runtime *runtime, int descriptor)
{
    Stream *stream = &runtime->stream;
    Session *session = &runtime->session;
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

    /* Arrival accounting: the evidence the interval estimate acts on, and the
     * rhythm nothing used to measure. A one-second cap keeps a genuine pause out
     * of the distribution -- a silence is not a frame interval. */
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
    trace_message(runtime, "A\t%lu\t%lu\t%lu\t%d\t%d\t%d",
                  current_time_ns(), sequence, (u64)length,
                  info.hasIdr, info.hasVcl, info.hasSps);

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
    runtime->supervisor.accessUnitsIn++;
    if (stream->outFd < 0) {
        /* Queueing while unbound would only build stale latency for a port we do
         * not own; drop it and keep the configuration. */
        supervisor_publish(runtime, 0);
        return 1;
    }

    /* Arrival-rate sample: after the fold check, because parameter-set records
     * are not frames and counting them inflates the rate for exactly the streams
     * that carry their headers on every keyframe. */
    if (!session->rateWindowStartMs) session->rateWindowStartMs = session->lastArrivalMs;
    session->rateWindowCount++;
    session->paceArrivalCount++;

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
        supervisor_set_ready(runtime, 1);
        supervisor_publish(runtime, 1);
        trace_message(runtime, "G\t%lu\t%lu", current_time_ns(), session->held);
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

    session_emit_access_unit(runtime, session->folded, total);
    return 1;
}

void session_finish(Runtime *runtime, int descriptor)
{
    Stream *stream = &runtime->stream;
    Pipe *pipe = &stream->pipe;
    Session *session = &runtime->session;
    int drained;

    close(descriptor);
    /* THE DISCONNECT EVENT, published before the drain: the drain can push
     * already-accepted frames onto the wire, and an event that trails the last
     * byte it describes is a worse event. */
    supervisor_set_connection(runtime, 0);

    /* DRAIN WHAT IS ALREADY SCHEDULED: closing with datagrams still pending
     * silently discarded the phone's last pictures. */
    runtime->timing.ignore = 1;                          /* teardown is not the stream */
    drained = pipe->count - pipe->cursor;
    pipe_release_all(runtime);
    mux_flush(runtime);                                  /* no partial datagram */
    log_message("conn close frame=%ld", (long)session->clock.frames);
    log_message("  pacer: drained-at-close=%d", drained);
    runtime->timing.ignore = 0;

    log_message("  arrival-gaps>250ms=%ld  late-frames=%ld  catch-ups=%ld  dropped-frames=%ld",
                (long)session->arrivalGaps, (long)pipe->statisticsLate,
                (long)pipe->statisticsCatchUps, (long)pipe->statisticsDropped);
    if (pipe->statisticsUnpaced)
        log_message("  UNPACED frames=%ld", (long)pipe->statisticsUnpaced);
    timing_report(runtime);                              /* this session's wire shape */
    timing_reset(runtime);

    if (session->held) {
        log_message("  gate: held=%ld  streaming=%d",
                    (long)session->held, session->streaming);
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

void session_run(Runtime *runtime, int listener)
{
    Stream *stream = &runtime->stream;
    Session *session = &runtime->session;

    log_message("pipe: start depth=%d ms  intra-frame release=ON (poll/ppoll,"
                " byte-proportional, one schedule)", session->jitterMs);
    log_message("  spread=%lu%% of each unit's interval (the remainder is cushion so the"
                " next frame can start on time -- 100 = no cushion)",
                (unsigned long)runtime->spreadPercent);
    supervisor_publish(runtime, 1);                      /* nothing connected yet */

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
            int action = control_poll(runtime);
            if (action == 1) output_bind(runtime, runtime->control.requestedPort);
            else if (action == 2) output_unbind(runtime);
        }
        supervisor_publish(runtime, 0);
        if (connection < 0) continue;

        /* THE CONNECT EVENT, before a single unit is read. */
        supervisor_set_connection(runtime, 1);
        session_reset(runtime);
        log_message("conn open  frame=%ld", (long)session->clock.frames);

        for (;;) {
            int readable;
            {
                PollDescriptor pollDescriptor;
                UnitTime timeout;
                u64 wait = pipe_poll_timeout_ns(runtime);
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
            supervisor_publish(runtime, 0);

            {   /* The supervisor's hand on the port: a bind starts from a clean
                 * schedule and a fresh IDR, an unbind leaves nothing in flight. */
                int action = control_poll(runtime);
                if (action == 1) {
                    output_bind(runtime, runtime->control.requestedPort);
                    if (stream->outFd >= 0) {
                        stream->mux.programAssociationLastMs = 0;
                        session->streaming = 0;
                        session->headerPending = 1;
                        stream->pipe.nextSlotStartNs = 0;
                        stream->pipe.lastEmissionNs = 0;
                        stream->pipe.count = stream->pipe.cursor = 0;
                    }
                } else if (action == 2) {
                    output_unbind(runtime);
                    session->streaming = 0;
                    stream->pipe.nextSlotStartNs = 0;
                    stream->pipe.lastEmissionNs = 0;
                    stream->pipe.count = stream->pipe.cursor = 0;
                }
            }

            session_maintain(runtime);

            /* Read before releasing, so a unit that arrives now can start leaving
             * in this same iteration. */
            if (readable && !session_read_record(runtime, connection)) break;
            pipe_release_due(runtime, current_time_ns());
        }
        session_finish(runtime, connection);
    }
}

/* ------------------------------------------------------------ 15. file input */

static const u8 *g_fileUnits[FILE_ACCESS_UNIT_LIMIT];
static int g_fileUnitLengths[FILE_ACCESS_UNIT_LIMIT];

static void play_file(Runtime *runtime, Options *options, int unitCount)
{
    Stream *stream = &runtime->stream;

    stream->pipe.unitPaced = 0;                          /* file mode is unpaced */
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
        mux_access_unit(runtime, unit, bytes, presentationTicks);

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
            if (((frame + 1) / (u64)options->gapEvery & 0x07) == 1)
                log_message("gap after frame %ld (%d ms of silence)",
                            (long)(frame + 1), options->gapMs);
            usleep((unsigned)options->gapMs * 1000u);
        }
        if (!options->loop && frame + 1 >= (u64)unitCount) break;
    }
}

/* ------------------------------------------------------------------ 16. probe */

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

/* ------------------------------------------------- 17. output socket and ctl */

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

void output_bind(Runtime *runtime, int port)
{
    Stream *stream = &runtime->stream;
    int descriptor;

    if (stream->outFd >= 0) return;
    if (port > 0) runtime->output.sourcePort = port;
    descriptor = output_socket_open(&runtime->output);
    if (descriptor < 0) { log_message("ctl: bind did not happen"); return; }
    stream->outFd = descriptor;
    stream->pipe.count = stream->pipe.cursor = 0;
    supervisor_set_bound(runtime, runtime->output.sourcePort);
    supervisor_set_ready(runtime, 0);
    log_message("ctl: PORT OURS");
    supervisor_publish(runtime, 1);
}

void output_unbind(Runtime *runtime)
{
    Stream *stream = &runtime->stream;

    if (stream->outFd < 0) return;
    close(stream->outFd);
    stream->outFd = -1;
    stream->pipe.count = stream->pipe.cursor = 0;
    stream->pipe.nextSlotStartNs = 0;
    stream->pipe.lastEmissionNs = 0;
    supervisor_set_bound(runtime, 0);
    supervisor_set_ready(runtime, 0);
    log_message("ctl: port released");
    supervisor_publish(runtime, 1);
}

/* 0 = nothing new, 1 = bind, 2 = unbind, -1 = unrecognised (never acted on) */
int control_poll(Runtime *runtime)
{
    ControlChannel *control = &runtime->control;
    char buffer[CONTROL_COMMAND_BYTES];
    int descriptor, action = 0;
    long received;
    int index;

    if (!control->path[0]) return 0;
    if (current_time_ms() - control->lastPollMs < CONTROL_POLL_INTERVAL_MS) return 0;
    control->lastPollMs = current_time_ms();

    descriptor = open(control->path, O_RDONLY);
    if (descriptor < 0) return 0;
    received = read(descriptor, buffer, sizeof buffer - 1);
    close(descriptor);
    if (received <= 0) return 0;
    buffer[received] = 0;
    for (index = 0; index < (int)received; index++)
        if (buffer[index] == '\n' || buffer[index] == '\r') buffer[index] = 0;
    if (!strcmp(buffer, control->lastCommand)) return 0;
    {   int copy = 0;
        while (copy < (int)sizeof control->lastCommand - 1 && buffer[copy]) {
            control->lastCommand[copy] = buffer[copy];
            copy++;
        }
        control->lastCommand[copy] = 0;
    }

    if (!strncmp(buffer, "bind", 4)) {
        /* The port travels WITH the command, so a supervisor that discovers a new
         * multi-screen-server port never has to restart this process. */
        int requested = atoi(buffer + 4);
        if (requested > 0 && requested < 65536) { control->requestedPort = requested; action = 1; }
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
        else if (!strcmp(argument, "--trace") && value) { options->tracePath = value; index++; }
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
               "  (start depth).  See docs/development/pathb-pacing-and-timing.md\n");
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
    static Runtime runtime;
    Options options;
    Stream *stream = &runtime.stream;
    int listener = -1;
    int unitCount = 0;
    u8 *blob = 0;
    usize_t blobLength = 0;
    int result;

    result = parse_options(argc, arguments, &options);
    if (result) return result;

    runtime_initialise(&runtime, &options);
    stream->mux.packetsPerDatagram = options.packetsPerDatagram;
    stream->mux.ssrc = (u32)options.ssrc;
    stream->mux.transportStreamId = options.transportStreamId;
    stream->mux.pmtPid = options.pmtPid;
    stream->mux.elementaryPid = options.elementaryPid;
    stream->mux.pcrPid = options.elementaryPid;
    stream->pipe.stride = RTP_HEADER_BYTES + options.packetsPerDatagram * TRANSPORT_PACKET_BYTES;

    if (options.probe)
        return probe_group(options.destination, options.destinationPort, options.output,
                           options.probeCount, options.probeSeconds, options.probeVideoOnly);

    if (options.output && !strncmp(options.output, "file:", 5)) {
        stream->outFd = open(options.output + 5, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (stream->outFd < 0) { log_message("cannot open out"); return 1; }
    } else {
        output_settings_from_options(&runtime.output, &options);
        if (options.monitor) {
            /* The supervisor decides when the port is ours; until then this
             * process only watches the shim's stream. */
            stream->outFd = -1;
            log_message("monitor: AU socket only -- the port is taken on 'bind <port>'");
        } else {
            int descriptor = output_socket_open(&runtime.output);
            if (descriptor < 0) return descriptor == -2 ? 3 : 1;
            stream->outFd = descriptor;
        }
    }

    stream->mux.startMs = current_time_ms();

    if (!strncmp(options.input, "unix:", 5)) {
        listener = open_listener(options.input + 5);
        if (listener < 0) return 1;
        session_run(&runtime, listener);   /* never returns */
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
        play_file(&runtime, &options, unitCount);
    }

    pipe_release_all(&runtime);
    mux_flush(&runtime);
    log_message("datagrams: %ld", (long)stream->mux.sequence);
    {   u64 elapsed = current_time_ms() - stream->mux.startMs;
        log_message("elapsed_ms: %ld", (long)elapsed);
        log_message("write-failures: %ld", (long)stream->mux.writeFailures);
        if (elapsed > 0)
            log_message("datagrams/sec: %ld", (long)(((u64)stream->mux.sequence * 1000ull) / elapsed));
    }
    timing_report(&runtime);
    if (listener >= 0) close(listener);
    trace_close(&runtime);
    if (stream->outFd >= 0) close(stream->outFd);
    free(stream->pipe.bytes);
    free(stream->pipe.datagrams);
    free(runtime.session.record);
    free(runtime.session.folded);
    free(runtime.session.parameters.bytes);
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
