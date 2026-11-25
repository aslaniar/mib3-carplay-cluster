/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ============================================================================
 * mib3clusterstream.h -- every type, constant and prototype the programme needs.
 *
 * No #include, no libc types and no variable definitions: the programme is built
 * -nostdlib for the unit, so this header has to survive without a C library in
 * scope. The one include-allowed header is unit_compat.h, included before this.
 *
 * The shape of the programme is three nested objects and one root:
 *
 *   Runtime   everything that lives for the process: options, the output
 *             socket, the supervisor state line, the control channel, and the
 *             diagnostics. One instance exists (see mib3_run).
 *   Session   everything that lives for one phone connection: the media clock,
 *             the parameter-set holdover, the interval estimate and the buffers.
 *   Stream    the wire: the muxer and the pipe.
 * ==========================================================================*/
#ifndef MIB3_CLUSTERSTREAM_H
#define MIB3_CLUSTERSTREAM_H

/* ---------------------------------------------------------------- defaults */
#define DEFAULT_DESTINATION          "ff14::2:4%eth0.5"
#define DEFAULT_PORT                 42800
#define DEFAULT_SSRC                 100000u
#define DEFAULT_TRANSPORT_STREAM_ID  100
#define DEFAULT_PMT_PID              1000
#define DEFAULT_ELEMENTARY_PID       17
#define DEFAULT_PACKETS_PER_DATAGRAM 6
#define DEFAULT_STATE_PATH           "/dev/shm/pathb_conn.state"
#define DEFAULT_CONTROL_PATH         "/dev/shm/pathb_sender.ctl"
#define DEFAULT_JITTER_MS            450
#define DEFAULT_SPREAD_PERCENT       60
#define DEFAULT_VIDEO_FPS            30

/* ------------------------------------------------------------- wire format
 * See docs/development/pathb-wire-format.md for the measurements behind the
 * cadences and the three payload capacities. */
#define TRANSPORT_PACKET_BYTES        188
#define RTP_HEADER_BYTES              12
#define MAX_PACKETS_PER_DATAGRAM      32
#define DATAGRAM_CAPACITY_BYTES       (RTP_HEADER_BYTES + MAX_PACKETS_PER_DATAGRAM * TRANSPORT_PACKET_BYTES)
#define PAYLOAD_BYTES_PLAIN           184   /* no adaptation field: must be full */
#define PAYLOAD_BYTES_WITH_RAI        182   /* one-byte adaptation field */
#define PAYLOAD_BYTES_WITH_PCR        176   /* seven-byte adaptation field */
#define PAYLOAD_BYTES_UNENCODABLE     183   /* neither form can carry this */
#define ADAPTATION_FLAG_RANDOM_ACCESS 0x40
#define ADAPTATION_FLAG_PCR           0x10
#define TRANSPORT_PACKET_SYNC         0x47
#define NULL_PACKET_PID               0x1FFF
#define PSI_REPEAT_INTERVAL_MS        500
#define PCR_REPEAT_INTERVAL_MS        90

/* ---------------------------------------------------------------- the pipe
 * See docs/development/pathb-pacing-and-timing.md. A frame is muxed on arrival
 * and each datagram is given a deadline inside that frame's slot; the pipe is
 * both the jitter buffer and the intra-frame release. */
#define PIPE_BURST_DATAGRAMS                8
#define PIPE_POLL_CAP_NS                    2000000ull
#define PIPE_CAPACITY                       2048
#define OUTPUT_RATE_CEILING_BYTES_PER_MS    2000u
#define LATENCY_CAP_MS                      400u

/* ---- the rate loop --------------------------------------------------------
 * Depth is the integral of (source rate - output rate): the phase error of a
 * phase-locked loop whose oscillator is the output rate. One PI filter on that
 * single error sets the rate -- P damps, I removes the steady-state bias. The
 * arrival-count estimate is only the feedforward term it starts from. */
#define PACE_PERIOD_MS                 100
#define PACE_GAIN_PROPORTIONAL         0.024   /* fps per ms of depth error        */
#define PACE_GAIN_INTEGRAL             0.0072  /* fps per (ms of error * second)   */
#define PACE_INTEGRAL_LIMIT            1500.0  /* ms * s                           */
#define PACE_RATE_FLOOR                0.5     /* x the adopted base rate          */
#define PACE_RATE_CEILING              2.0     /* x the adopted base rate          */

/* ---- the offline replay's controller selector -----------------------------
 * The desk harness builds the same source several times with a different
 * PATHB_PACE_VARIANT so a controller can be falsified before it sees a car:
 *   0  no rate feedback (the first pipe build)
 *   1  clamped proportional term (the build that could not refill)
 *   2  PI on depth (the build that limit-cycled)
 *   3  the production controller: nominal rate from the count estimate, a
 *      filtered phase trim, a hard depth budget that drains a burst by rate,
 *      and a freeze so an empty pipe never causes a slow-down.
 * Only 3 is shipped; 0/1/2 exist to keep the harness honest. */
#ifndef PATHB_PACE_VARIANT
#define PATHB_PACE_VARIANT             3
#endif
#define PACE_DEPTH_DEADBAND_PERCENT    2       /* no trim inside +/-2%           */
/* The budget is a LATENCY CAP, not an allowance. The two knobs are deliberately
 * independent: the target is how much future the trim defends (the bank a hole
 * draws on -- raised to 450 because the capture's holes run 500-590 ms), the
 * budget is how much future the schedule may ever hold (the old proven sender
 * capped effective wait near 400-470 ms and that envelope is kept here). A
 * burst is drained by the budget's shorter slots; a hole is covered by the
 * target's bank. */
#define PACE_DEPTH_BUDGET_MS           50      /* budget = target + this         */
/* The phase filter is ASYMMETRIC, and the asymmetry is the point. This source
 * runs clump-then-silence: a burst lifts depth, then a 400-590 ms hole follows
 * it. A symmetric filter lags high after the burst, the trim turns positive,
 * and the wire pumps the buffer out INTO the hole that is about to starve it
 * -- measured on the desk as a 550 ms gap with 27 ms of buffer left. So:
 *   LOW side  (depth below target): fall fast, so a refill is ordered at once;
 *   HIGH side (depth above target): rise slowly, so a burst is left to the
 *              budget, which drains it by rate, and the trim never speeds the
 *              wire up into an oncoming silence. */
#define PACE_DEPTH_TAU_LOW_MS          400
#define PACE_DEPTH_TAU_HIGH_MS         2500
#define PACE_TRIM_LIMIT                0.45    /* x the nominal rate             */
#define PACE_TRIM_GAIN                 0.5     /* per unit of relative error     */
/* A DEEP drain gets a deeper trim. The car-proven sender's own emission cadence
 * ran at ~139 ms median -- the cluster demonstrably tolerates a slow wire -- so
 * after a hole the refill may slow to ~0.3x nominal and close the vulnerable
 * window (the seconds after a drain where another hole would find the bank
 * half-full) instead of crawling back at -45%. */
#define PACE_TRIM_DEEP_PERCENT         40      /* phase under this share of bank  */
#define PACE_TRIM_DEEP_LIMIT           0.70
/* Deceleration limit per control tick. Refill NEEDS the slow direction: the
 * trim's full -45% must be reachable in ~3 ticks, not ~8, or the buffer does
 * not rebuild between one hole and the next. The phase filter is the real
 * oscillation guard; this is the belt behind it. */
#define PACE_RATE_SLEW_PERCENT         15
#define PACE_ESTIMATE_JUMP_PERCENT     40      /* adopt a big change at once     */
/* Until the first count window closes the source rate is unknown. Bias the
 * opening pace SLOW: too slow costs depth the budget drains at the wire floor,
 * too fast starves the wire -- and the phone's opening burst is exactly the
 * fast case the estimate misreads first. */
#define PACE_STARTUP_INTERVAL_MS       50
/* The phase governor: when the filtered depth sits outside 70-130% of target
 * for two seconds, the base rate itself is wrong (the count estimate is only a
 * starting point). One small step per two seconds, never while the pipe is
 * empty. Bounded, so it cannot wind up the way the integral did. */
#define PACE_GOVERNOR_LOW_PERCENT      70
#define PACE_GOVERNOR_HIGH_PERCENT     130
#define PACE_GOVERNOR_TICKS            20
#define PACE_GOVERNOR_DOWN             0.85
#define PACE_GOVERNOR_UP               1.10
/* A STARVED phase (under a fifth of target for two seconds) is not a bias, it
 * is a rate the estimate has not admitted at all. Step harder. */
#define PACE_GOVERNOR_STARVE_PERCENT   20
#define PACE_GOVERNOR_DOWN_HARD        0.75
/* THE BANK RATCHET -- the one organ the old queue sender had that the pipe
 * dropped, restored. The car-proven build never let a wire gap exceed 278 ms
 * across 580 s of driving on a source whose silences ran to 670 ms, because its
 * queue target GREW on every starved tick and shrank when calm. A fixed bank
 * cannot do that: a stacked drought (silence -> thin clump -> silence) always
 * finds it empty. The starvation signal is the SOURCE going quiet past the
 * cadence (holes are >= 380 ms, the cadence gap is <= ~210 ms): detecting the
 * pipe emptying instead fires ~300 ms too late and only on the deepest holes,
 * which is why the trigger reads the arrival clock, not the pipe. The bank
 * steps once per hole and relaxes after sustained calm, so latency rides the
 * source: 450 ms when it is clean, up to the cap while it is being silenced. */
#define PACE_BANK_STEP_MS              50
#define PACE_BANK_MAX_MS               600
#define PACE_HOLE_MS                   300
#define PACE_CALM_MS                   10000
#define PACE_RELAX_MS                  50

/* ------------------------------------------------------------- media clock */
#define TICKS_PER_SECOND          90000u
#define TICKS_PER_MILLISECOND     90u
#define MEDIA_STEP_FLOOR_TICKS    1800u     /* 20 ms: a burst cannot run us back */
#define MEDIA_STEP_CEILING_TICKS  90000u    /* 1 fps: a sanity bound, not a rate */
#define MEDIA_DRIFT_REBASE_MS     2000
#define MEDIA_DRIFT_DEADBAND_MS   50
#define MEDIA_SLEW_DIVISOR        10

/* ------------------------------------------------------- the frame interval */
#define CLOCK_RATE_WINDOW_MS        4000
#define CLOCK_HYSTERESIS_PERCENT    10
#define CLOCK_BAND_WINDOWS          2
#define MINIMUM_INTERVAL_MS         10
#define MAXIMUM_INTERVAL_MS         125
#define FALLBACK_INTERVAL_MS        40

/* ----------------------------------------------------------------- session */
#define PARAMETER_SET_CAP_BYTES     (256u * 1024u)
#define FILE_ACCESS_UNIT_LIMIT      (1u << 14)
#define ARRIVAL_GAP_THRESHOLD_MS    250
#define ARRIVAL_INTERVAL_MIN_MS     5
#define ARRIVAL_INTERVAL_MAX_MS     1000
#define STEP_SENSOR_WEIGHT          7
#define STEP_SENSOR_DIVISOR         8
#define STEP_COMMIT_INTERVAL_MS     1000

/* ------------------------------------------------------------- diagnostics */
#define SUPERVISOR_PUBLISH_INTERVAL_MS 200
#define CONTROL_POLL_INTERVAL_MS       100
#define ACCEPT_POLL_TIMEOUT_MS         200
#define LATENCY_REPORT_INTERVAL_MS     2000
#define CADENCE_REPORT_INTERVAL_MS     2000
#define CADENCE_RING_CAPACITY          128
#define LATENCY_RING_CAPACITY          64
#define TIMING_RING_CAPACITY           512
#define TIMING_HISTOGRAM_BUCKETS       12
#define TIMING_PEAK_WINDOW_NS          25000000ull
#define TIMING_LONG_GAP_NS             20000000ull
#define TIMING_LONG_GAP_REPORT_LIMIT   32
#define LOG_LINE_BYTES                 256
#define STATE_LINE_BYTES               160
#define STATE_PATH_BYTES               112
#define CONTROL_PATH_BYTES             112
#define CONTROL_COMMAND_BYTES          48
#define HOST_NAME_BYTES                128
#define UNIX_PATH_BYTES                108

/* -------------------------------------------------------------------- types */

/* A number ring with one push and one percentile routine. Four capacities are in
 * use, so capacity is a field rather than a compile-time constant. */
typedef struct {
    u64 *values;
    int  capacity, index, count;
} Ring;

/* One NAL unit found by the single start-code walk. `startCode` is backed up over
 * the leading zeroes of a four-byte start code, which is the offset the splitter
 * cuts at. */
typedef struct {
    usize_t startCode;
    int     type;
    int     firstPayloadByte;   /* -1 when the NAL is only a header byte */
} NalUnit;

/* Everything one access unit tells us, from one pass over it. */
typedef struct {
    int     hasVcl, hasSps, hasPps, hasIdr;
    usize_t lastSpsStart;
} AccessUnitInfo;

typedef struct {
    u8      *bytes;
    usize_t  length, capacity;
    int      hasSps, hasPps;
} ParameterSet;

/* ---- wire ---- */
typedef struct {
    u8  datagram[DATAGRAM_CAPACITY_BYTES];
    int packetsInDatagram;
    int packetsPerDatagram;
    int continuityPat, continuityPmt, continuityEs;
    u16 sequence;
    u32 ssrc;
    u32 rtpTimestamp;               /* 90 kHz transmission clock of this unit */
    u64 programAssociationLastMs;
    u64 programClockLastMs;
    u64 presentationTimeTicks;
    u64 startMs;
    int transportStreamId, pmtPid, elementaryPid, pcrPid;
    u64 writeFailures;
} Mux;

/* One datagram waiting in the pipe. Its deadline is the instant it may go out;
 * its arrival is when the frame it belongs to arrived. */
typedef struct {
    u32 length;
    u64 deadlineNs;
    u64 arrivalNs;
} Datagram;

typedef struct {
    u8       *bytes;               /* datagram payloads, stride `stride` */
    Datagram *datagrams;
    int       capacity, count, cursor, stride;

    /* the slot of the frame currently being muxed */
    u64 unitStartNs, unitLengthNs, unitArrivalNs;
    u64 unitPayloadTotal, unitPayloadDone, unitPayloadFlushed;
    int unitPaced, latencyRecorded;

    u64 nextSlotStartNs;           /* when the next frame's slot begins (0 = unarmed) */
    u64 lastEmissionNs;            /* the release-time wire-rate floor's anchor */

    u64 statisticsLate;            /* frames whose slot had already begun */
    u64 statisticsCatchUps;        /* high-water shifts of the schedule */
    u64 statisticsDropped;         /* frames refused because the pipe was full */
    u64 statisticsUnpaced;         /* frames written without pacing */
} Pipe;

typedef struct {
    Mux  mux;
    Pipe pipe;
    int  outFd;
} Stream;

/* ---- clocks ---- */
typedef struct {
    u64 frames;
    u64 stepTicks;              /* committed step per access unit */
    u64 sensorTicks;            /* arrival-interval EWMA (ms*90) */
    u64 stepCommittedMs;
    u64 wallBaseMs, ptsBaseTicks;
} MediaClock;

typedef struct {
    Ring arrival, emission;
    u64  lastEmissionMs, lastSequence, sequenceGaps, lostFrames;
} Cadence;

typedef struct {
    Ring samples;
    u64  maximum, lastReportMs;
} Latency;

/* ---- supervisor and control ---- */
typedef struct {
    char path[STATE_PATH_BYTES];
    u64  lastPublishMs, sequence;
    int  connection, ready, bound;
    u64  accessUnitsIn, accessUnitsOut;
} Supervisor;

typedef struct {
    char path[CONTROL_PATH_BYTES];
    char lastCommand[CONTROL_COMMAND_BYTES];
    u64  lastPollMs;
    int  requestedPort;
} ControlChannel;

typedef struct {
    char        host[HOST_NAME_BYTES];
    const char *interfaceName;
    int         destinationPort, sourcePort;
    const char *sourceAddress;
} OutputSettings;

typedef struct {
    Ring ring;
    u64  writes, lastNs, longestNs, gapsOver20Ms;
    u64  histogram[TIMING_HISTOGRAM_BUCKETS];
    u64  peakPer25Ms;
    int  ignore;
} TimingStats;

/* ---- one phone connection ---- */
typedef struct {
    MediaClock   clock;
    Cadence      cadence;
    Latency      latency;
    ParameterSet parameters;
    int jitterMs, noFold, noIdrGate;
    int streaming, headerPending;
    u64 held;
    u64 lastArrivalMs, arrivalGaps;
    u64 estimateMs;                /* the arrival-count estimate (feedforward) */
    int estimateBandWindows;
    u64 paceIntervalMs;            /* the loop's interval -- the authority */
    double paceBaseRate;           /* the estimate as a rate, frames per second */
    double paceIntegral;           /* ms * s */
    u64 paceControlMs;
    u64 pacePreviousTickMs;
    double paceTrim;               /* the new loop's bounded phase trim        */
    double paceDepthAvg;           /* filtered depth: the phase, not the burst */
    double paceArrivalRate;        /* per-tick count, EWMA: the burst signal    */
    u64 paceArrivalCount;
    u64 paceBankMs;                /* the ratcheted target the trim defends     */
    u64 paceLastStarveMs;
    int paceHoleCounted;
    u64 paceLowTicks, paceHighTicks;
    u64 rateWindowStartMs, rateWindowCount, cadenceReportMs;
    u64 previousLate, previousCatchUps, previousDropped;
    u64 previousAccessUnitsIn, previousAccessUnitsOut;
    u8 *record, *folded;
    usize_t recordCapacity, foldedCapacity;
} Session;

typedef struct {
    const char *input, *inputFormat, *destination, *sourceAddress;
    const char *output, *statePath, *controlPath, *tracePath;
    int destinationPort, sourcePort, framesPerSecond;
    int loop, realtime, monitor, noFold, noIdrGate, statisticsTiming;
    int probe, probeCount, probeSeconds, probeVideoOnly;
    int jitterMs, gapMs, gapEvery;
    int packetsPerDatagram, ssrc, transportStreamId, pmtPid, elementaryPid;
    u32 spreadPercent;
} Options;

/* ---- the process ---- */
typedef struct {
    Options        options;
    Stream         stream;
    Session        session;
    OutputSettings output;
    ControlChannel control;
    Supervisor     supervisor;
    TimingStats    timing;
    u32            spreadPercent;
    int            timingEnabled;
    int            traceFd;          /* --trace: the offline harness's event log */
} Runtime;

/* --------------------------------------------------------------- prototypes */

int mib3_run(int argc, char **arguments);

/* logging: one formatter, two sinks (stderr and the state file) */
usize_t format_line(char *out, usize_t capacity, const char *format, ...);
void    log_message(const char *format, ...);
void    trace_message(Runtime *runtime, const char *format, ...);
void    trace_close(Runtime *runtime);

/* rings */
void ring_reset(Ring *ring);
void ring_push(Ring *ring, u64 value);
void ring_percentiles(const Ring *ring, u64 *p50, u64 *p95, u64 *maximum);
int  ring_values_within(const Ring *ring, u64 newest, u64 window);

/* the process */
void runtime_initialise(Runtime *runtime, Options *options);

/* supervisor state */
void supervisor_publish(Runtime *runtime, int force);
void supervisor_set_connection(Runtime *runtime, int connected);
void supervisor_set_ready(Runtime *runtime, int ready);
void supervisor_set_bound(Runtime *runtime, int port);

/* timing diagnostics */
void timing_note_write(Runtime *runtime);
void timing_report(Runtime *runtime);
void timing_reset(Runtime *runtime);

/* wire format */
u32  mpeg_crc32(const u8 *data, usize_t length);
void pack_transport_packet(u8 *out, int pid, int unitStart, int continuityCounter,
                           const u8 *payload, int payloadBytes, int wantPcr,
                           u64 pcrTicks, int randomAccess);
void pack_program_association(u8 *out, int transportStreamId, int pmtPid, int continuityCounter);
void pack_program_map(u8 *out, int pmtPid, int pcrPid, int elementaryPid, int continuityCounter);
int  write_pes_header(u8 *out, u64 presentationTicks);

/* mux and pipe */
void stream_write_datagram(Runtime *runtime, const u8 *datagram, int bytes);
void mux_flush(Runtime *runtime);
void mux_push(Runtime *runtime, const u8 *packet);
void mux_push_null(Runtime *runtime);
void mux_end_access_unit(Runtime *runtime);
void mux_access_unit(Runtime *runtime, const u8 *unit, int bytes, u64 presentationTicks);
int  pipe_reserve(Pipe *pipe, int wanted);
void pipe_append_datagram(Runtime *runtime, int bytes);
int  pipe_release_due(Runtime *runtime, u64 nowNs);
void pipe_release_all(Runtime *runtime);
u64  pipe_poll_timeout_ns(Runtime *runtime);
u64  pipe_pending_ns(Runtime *runtime);

/* input parsing */
int  nal_next(const u8 *data, usize_t length, usize_t *cursor, NalUnit *unit);
void access_unit_inspect(const u8 *data, usize_t length, AccessUnitInfo *info);
int  parameter_set_absorb(ParameterSet *set, const u8 *record, usize_t length,
                          const AccessUnitInfo *info, int *changed);
void parameter_set_reset(ParameterSet *set);
int  is_new_access_unit(int nalType, int haveSlice);
int  split_access_units(const u8 *data, usize_t length, const u8 **units, int *lengths, int maximum);
u8  *avcc_to_annexb(const u8 *data, usize_t length, usize_t *outLength);
int  read_all(int descriptor, u8 **buffer, usize_t *length);

/* session */
void session_reset(Runtime *runtime);
void session_emit_access_unit(Runtime *runtime, const u8 *unit, usize_t length);
void session_maintain(Runtime *runtime);
int  session_read_record(Runtime *runtime, int descriptor);
void session_finish(Runtime *runtime, int descriptor);
void session_run(Runtime *runtime, int listener);

/* output and control */
int  output_socket_open(OutputSettings *output);
void output_bind(Runtime *runtime, int port);
void output_unbind(Runtime *runtime);
int  control_poll(Runtime *runtime);

/* inputs and options */
int  probe_group(const char *destination, int port, const char *output, int count, int seconds, int videoOnly);
int  parse_options(int argc, char **arguments, Options *options);
void output_settings_from_options(OutputSettings *output, Options *options);

#endif /* MIB3_CLUSTERSTREAM_H */
