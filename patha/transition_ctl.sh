#!/bin/sh
# ============================================================================
# transition_ctl.sh -- ONE lifecycle controller for WHAT THE CLUSTER SHOWS.
#
#   *** LIVE-ONLY. RAM-only. Touches no service (carplay / multi-display /
#       rsi-display-management / displaymgr).  Starts and stops only OUR OWN
#       processes, and does RSI element work through deliver_ctl.sh.  A reboot
#       is the full undo. ***
#
# THE BUG THIS EXISTS FOR (plain language): when the phone leaves, the cluster
# keeps showing the last CarPlay frame -- a picture of a road that no longer
# exists -- and it does that FOREVER.  Three things conspire:
#   (1) the cluster ECU RETAINS the last frame when the feed stops (documented);
#   (2) the presenter (mib3egl --rgba-sock) re-commits its LAST texture every
#       frame, by design, so the stale frame is not even a frozen stream -- it is
#       a live stream of a stale picture;
#   (3) nothing in the chain notices that the phone's session ended: the shim
#       never closes the AU socket, so the decoder never sees EOF, so it keeps
#       running, so the presenter keeps presenting.
# The same is true, less visibly, in the other direction: between "CarPlay's
# cluster stream started" and "the first decodable frame", a premature switch
# would park the car's map and show an empty/garbage surface instead.
#
# THE STATE MACHINE (see LANE_CLUSTER_TRANSITION_2026-10-18.md):
#
#        (phone's cluster stream starts; no decodable frame yet)
#   NAV ───────────────────────────────────────────────► PENDING
#    ▲   cluster shows the CAR'S OWN MAP (nav ivi 33)     │  do nothing to MS-S:
#    │                                                    │  keep showing nav
#    │                        frames >= NFRAMES           │
#    │                                    ▼               ▼
#    │                                 CARPLAY  (MS-S shares our 3000)
#    │                                    │   frames stall < HOLD  -> hold (last frame stays)
#    │                                    │   stream closed / stall > HOLD
#    │                                    ▼
#    │                                SHUTDOWN   (bounded, in this order:)
#    │                                    │   1. stop the decoder   (also: a new
#    │                                    │      decoder per session is an invariant)
#    │                                    │   2. FEED A NEUTRAL FRAME -> the stale
#    │                                    │      map is gone in ~1 s, unconditionally
#    │                                    │   3. hand the cluster back to the car's
#    │                                    │      nav (oracle-gated; may fail)
#    └────────────────────────────────────┴──► NAV      (nav restored)
#                                            └──► FALLBACK (neutral frame stays;
#                                                 bounded retries, then honest give-up)
#
# WHY NEUTRAL FIRST AND NAV SECOND: handing the cluster back is the PLEASANT
# outcome but it depends on multi-screen-server, which we do not control (its
# 1 Hz sampling, its (ip,port) dedup and its holder status can all refuse
# silently).  The neutral frame depends only on our own presenter, so it is what
# makes the lie stop deterministically.  Both are attempted, in that order, and
# both verdicts are logged.
#
# USAGE (on the unit):
#   nohup sh transition_ctl.sh run [minutes] &   # hands-free: the operator never waits
#   sh transition_ctl.sh once                    # one tick (diagnosis; prints the decision)
#   sh transition_ctl.sh status                  # state + timeline tail
#   sh transition_ctl.sh to-nav                  # force the car's map back (idempotent)
#   sh transition_ctl.sh to-neutral              # force the neutral frame
#   sh transition_ctl.sh to-carplay              # force the switch now (operator/test)
#   sh transition_ctl.sh log [n]                 # the transition timeline
#   sh transition_ctl.sh stop                    # stop the controller (bring_up.sh down)
#
# EVERY TRANSITION IS LOGGED WITH A MONOTONIC TIMESTAMP (uptime ms) AND UTC, so a
# parallel investigation (session drops) can correlate against the shim's log.
#
# RAM-only. Every mutating step also leaves a breadcrumb on the STICK if one is
# mounted (a fault in this area can reset the unit and wipe /dev/shm).
# ============================================================================
set -u

# --- inputs / knobs (all overridable: the desk bench drives this file) ------
SHIMLOG="${MIB3_TR_SHIMLOG:-/tmp/mib3shim.log}"
DECLOG="${MIB3_TR_DECLOG:-/tmp/dec.log}"
EGLLOG="${MIB3_TR_EGLLOG:-/tmp/egl.log}"
AUSOCK="${MIB3_TR_AUSOCK:-/dev/shm/mib3cluster.sock}"
RGBASOCK="${MIB3_TR_RGBA_SOCK:-/dev/shm/mib3rgba.sock}"
NEUTRAL="${MIB3_TR_NEUTRAL:-/dev/shm/neutral.rgba}"
W="${MIB3_TR_W:-1280}"
H="${MIB3_TR_H:-480}"
POLL="${MIB3_TR_POLL:-0.2}"            # seconds per tick (sub-second when supported)
# The "even if nothing changed, evaluate at least this often" ceiling.  It USED to
# be 0.5 s, because a quiet evaluation cost two whole-file greps and running that
# 5x a second was unaffordable.  Phase 2a made the reads bounded (see shim_counts),
# so the ceiling is now the tick itself -- and it has to be: the stall detector
# samples at this cadence, so with a 0.5 s ceiling a 1.2 s hold could not fire
# before ~1.7 s however fast the sleep is.  Measured: with 0.5 s the hold fired at
# 2.4 s from the last frame; with POLL it fires at 1.2-1.4 s.
TICKMAX="${MIB3_TR_TICKMAX:-$POLL}"    # full evaluation at least this often, even if quiet
HOLD="${MIB3_TR_HOLD:-1.2}"            # a frame stall shorter than this is a blip, not an end
HOLDMAX="${MIB3_TR_HOLDMAX:-6}"        # ceiling for the flap backoff (see hold_for)
FLAPWIN="${MIB3_TR_FLAPWIN:-30}"       # a new session within this of an end = a flap
PENDMAX="${MIB3_TR_PENDMAX:-45}"       # the keyframe gate is ~3 s worst case; 45 s is "not delivering"
# A VIDEO PAUSE IS NOT AN END (2026-10-24).  The phone stops sending for a second
# or two at a scene change and we used to tear the whole session down for it --
# then the re-arm's keyframe request produced one frame, which re-triggered the
# switch, which produced another pause: a 25-35 s cycle that showed a black frame
# on the cluster every lap.  Now a stall below PAUSE_MAX is a PAUSE: we ask for a
# keyframe (by reconnecting the helper, which makes the shim send forceKeyFrame),
# keep the session, the RSI and the decoder's role untouched, and only hand the
# cluster back if the silence outlives PAUSE_MAX.
PAUSE_MAX="${MIB3_TR_PAUSE_MAX:-10}"   # seconds of silence before we really end it.
#    Measured 2026-10-24 with 25 s: the cluster froze on the last map for 8 s (the
#    presenter's idle-blank), then sat GREY for ~17 s before the nav came back --
#    ~20 s of nothing.  The longest REAL pause measured is 1.5-2.3 s (a scene
#    change), so 10 s is 4x the worst case and still quick to hand back.
NFRAMES="${MIB3_TR_NFRAMES:-1}"        # decoded frames needed to call the decode path live
NFRAMES_EVENT="${MIB3_TR_NFRAMES_EVENT:-1}"  # 1 = also gate on the decoder's own
                                       # first `FRAME ` line (see dec_first_frame);
                                       # 0 = the counter only (pre-2026-10-21)
MAXATT="${MIB3_TR_MAXATT:-3}"          # switch attempts before we stop trying
RETRY="${MIB3_TR_RETRY:-5}"            # seconds between switch attempts
RESTORE_T="${MIB3_TR_RESTORE_T:-10}"   # oracle gate for the nav hand-back (MS-S samples 1 Hz)
FEED_T="${MIB3_TR_FEED_T:-8}"          # wall guard on ONE neutral-feed attempt
FEED_TRIES="${MIB3_TR_FEED_TRIES:-2}"  # a stall here must not become a slow shutdown
FEEDCOUNT="${MIB3_TR_FEEDCOUNT:-3}"    # 1-3 frames is enough: the presenter keeps the last one
FBTRIES="${MIB3_TR_FBTRIES:-6}"        # bounded nav-restore retries from FALLBACK
FBPERIOD="${MIB3_TR_FBPERIOD:-60}"     # after giving up, how rarely to try again (s)
EXIT_RESTORE="${MIB3_TR_EXIT_RESTORE:-0}"

# --- our own binaries (paths only; nothing here is required to exist) -------
KIT="${MIB3_TR_KIT:-/tmp/kit}"
DEC="${MIB3_TR_DEC:-$KIT/v4l2_seq}"
EGL="${MIB3_TR_EGL:-$KIT/mib3egl}"
RGBAFEED="${MIB3_TR_RGBAFEED:-$KIT/rgba_feed}"
IVIID="${MIB3_TR_IVIID:-3000}"

# --- the delivery controller (its `handover` verb is the ONLY switch) ------
case "$0" in */*) HEREDIR="${0%/*}" ;; *) HEREDIR="." ;; esac
CTL="${MIB3_TR_CTL:-}"
if [ -z "$CTL" ]; then
  for c in "$HEREDIR/deliver_ctl.sh" /tmp/kit/deliver_ctl.sh /dev/shm/deliver_ctl.sh; do
    [ -f "$c" ] && { CTL="$c"; break; }
  done
fi
[ -n "$CTL" ] || CTL="$HEREDIR/deliver_ctl.sh"

# --- where our state and timeline live -------------------------------------
stick_path() {
  [ -n "${MIB3_DLV_STICK:-}" ] && { printf '%s' "$MIB3_DLV_STICK"; return 0; }
  for d in /tmp/USB/PORT1 /tmp/USB/PORT2 /tmp/USB/PORT3 /tmp/USB/PORT4; do
    [ -d "$d/PART1" ] && { printf '%s/PART1' "$d"; return 0; }
  done
  return 1
}
if RUN="$(stick_path)"; then :; else RUN="${MIB3_TR_RAM:-/dev/shm}"; fi
mkdir -p "$RUN" 2>/dev/null
TLOG="$RUN/transition.log"        # the timeline (append-only)
TSTATE="$RUN/transition.state"    # one line: STATE hold_start frame_last session_up t_enter attempts
TBC="$RUN/transition.bc"          # breadcrumbs (write+sync) for a post-mortem
TLOCK="$RUN/transition.lock"

# ============================================================================
# time + logging
# ============================================================================
# monotonic ms.  /proc/uptime is on the unit and costs nothing; the desk bench
# has no /proc, so fall back to epoch seconds (ordering is what the bench asserts).
#
# *** BASE 10, ALWAYS (2026-10-21, PLAN_SWITCH_LATENCY V3). ***  `/proc/uptime`
# prints e.g. `2289.08`, and `08` / `09` are NOT valid octal: the shell aborts
# the arithmetic ("value too great for base") and now_ms returns the EMPTY
# STRING.  ~20 % of seconds, and the field log carries 10 empty timings from it.
# `10#` is a bashism busybox ash does not have, so the portable cure is to strip
# the leading zeros -- the value is then a plain decimal literal everywhere.
# MIB3_TR_UPTIME exists so the desk bench can assert the 08/09 cases.
MS_LAST=0
now_ms() {
  _uf="${MIB3_TR_UPTIME:-/proc/uptime}"
  if [ -r "$_uf" ]; then
    _u="$(cut -d' ' -f1 "$_uf" 2>/dev/null)"
    _s="${_u%%.*}"; case "$_s" in ''|*[!0-9]*) _s=0 ;; esac
    case "$_u" in *.*) _f="${_u#*.}" ;; *) _f="" ;; esac
    _f="${_f}000"; _f="${_f%"${_f#???}"}"      # exactly the first three digits
    _f="${_f#0}"; _f="${_f#0}"; _f="${_f#0}"   # 08/09 -> 8/9, 000 -> empty
    printf '%s' "$(( _s * 1000 + ${_f:-0} ))"
  else
    # No /proc (the desk).  `date +%s%N` is nanoseconds where the platform has
    # it; a platform whose date prints a literal "N" fails the digit test and
    # gets whole seconds.  This matters only on the desk, but it matters a lot
    # there: with a second-granular clock every sub-second claim this bench
    # makes (a 50 ms poll, a 1.2 s hold) is unmeasurable, which is exactly how
    # the 1 s tick survived a green suite.  On the unit /proc/uptime is always
    # readable, so this branch never runs on the car.
    _n="$(date +%s%N 2>/dev/null)"
    case "$_n" in
      ''|*[!0-9]*) printf '%s' "$(( $(date +%s) * 1000 ))" ;;
      *) if [ ${#_n} -ge 13 ]; then printf '%s' "$(( _n / 1000000 ))"
         else printf '%s' "$(( _n * 1000 ))"; fi ;;
    esac
  fi
}

utc() { date -u +%H:%M:%S 2>/dev/null || echo "--:--:--"; }

# --- the sub-second wait (PHASE 2a, 2026-10-21) -----------------------------
# THE DEFECT: on the unit `sleep` is busybox without FANCY_SLEEP, so `sleep 0.2`
# is not "a fifth of a second" -- the applet REJECTS the argument -- and the old
# code's answer was to pay a whole second instead.  Every tick, every 50 ms reap
# poll and every 50 ms neutral poll therefore cost 1000 ms, and the field log says
# so in one word: `nap=INTEGER-ONLY`.  That one second is most of the detect and
# stall latency the driver felt, and it is why a "5 Hz" controller was really 1 Hz.
# THE FIX: `busybox usleep` for fractional values (deliver_ctl.sh has used it
# since 2026-10-20: measured accurate on this unit, 110 ms for a 100 ms request),
# `sleep` for whole seconds, and NOTHING AT ALL for 0 (the old code sent 0 down
# the fractional branch and paid a full second for a no-op).
# WHY NOT THE OLD PROBE: it chose the backend by TIMING six `sleep 0.25` calls in
# whole seconds.  On the unit each of those returned instantly (rejected
# argument), so the verdict was "integer-only" and the controller then quietly
# disabled every sub-second wait for the rest of the run.  The probe was measuring
# a symptom and using it as a capability answer.  The backend is now chosen by a
# CAPABILITY test (does the applet accept the argument: rc 0) and the timing probe
# survives only to PRINT what the platform does -- which is the one thing a field
# log should never have to guess.
# Knob: MIB3_TR_NAP=auto|usleep|sleep (sleep = today's behaviour, on purpose).
NAP_MODE="${MIB3_TR_NAP:-auto}"
NAP_B=""                          # resolved backend: usleep | frac | sleep
NAP_F=""                          # the timing probe's verdict (diagnostics only)
nap_probe() {
  [ -n "$NAP_F" ] && return 0
  # 6 x 0.25 s = 1.5 s nominal, judged in WHOLE SECONDS: 1.5 s always crosses a
  # boundary, so the verdict cannot be a coin flip (4 x 0.25 judged in ms did not
  # work on a platform whose "ms" clock is really whole seconds).  REPORT ONLY.
  _a="$(date +%s)"; _i=0
  while [ "$_i" -lt 6 ]; do sleep 0.25 2>/dev/null; _i=$((_i+1)); done
  _b="$(date +%s)"
  if [ "$_b" -gt "$_a" ]; then NAP_F=f; else NAP_F=i; fi
  return 0
}
nap_backend() {
  [ -n "$NAP_B" ] && return 0
  case "$NAP_MODE" in
    usleep) NAP_B=usleep ;;
    sleep)  NAP_B=sleep ;;
    *)  # CAPABILITY, not timing: the applet either accepts the argument or not.
        if busybox usleep 1000 >/dev/null 2>&1; then NAP_B=usleep
        elif sleep 0.05 >/dev/null 2>&1;   then NAP_B=frac
        else NAP_B=sleep; fi ;;
  esac
  return 0
}
# "0.05" -> 50000 microseconds, WITHOUT a fork: this runs on every tick, and an
# `awk` here would be a process per 200 ms for arithmetic the shell can do.
us_of() {
  _s="${1:-0}"
  case "$_s" in *.*) _i="${_s%%.*}"; _f="${_s#*.}" ;; *) _i="$_s"; _f="" ;; esac
  case "$_i" in ''|*[!0-9]*) _i=0 ;; esac
  _f="${_f}000000"; _f="${_f%"${_f#??????}"}"       # exactly the first six digits
  _f="${_f#"${_f%%[!0]*}"}"                          # 000500 -> 500, 000000 -> ""
  printf '%s' "$(( _i * 1000000 + ${_f:-0} ))"
}
nap() {
  _sec="${1:-0}"
  case "$_sec" in ''|0|0.0|0.00|0.000|0.0000|0.00000|0.000000) return 0 ;; esac
  nap_backend
  case "$_sec" in
    *.*)
      case "$NAP_B" in
        usleep)
          _us="$(us_of "$_sec")"
          [ "$_us" -gt 0 ] && busybox usleep "$_us" 2>/dev/null && return 0
          # a usleep that refuses at runtime must DEGRADE to the old whole-second
          # floor, never spin: a busy loop here would be a much worse failure
          # than a slow one (it would also hide itself -- no line, no sleep).
          sleep 1 ;;
        frac) sleep "$_sec" 2>/dev/null || sleep 1 ;;
        *)    sleep 1 ;;                       # today's behaviour, by knob
      esac ;;
    *) sleep "$_sec" 2>/dev/null ;;
  esac
  return 0
}
# what the RUN banner reports: the BACKEND (what actually runs) and the probe's
# measurement (what the platform actually does).  Two facts, never one guess.
nap_mode() {
  nap_backend; nap_probe
  printf '%s/%s' "$NAP_B" "$([ "$NAP_F" = f ] && echo fractional || echo INTEGER-ONLY)"
}

# cheap change detector: the logs only ever grow, so their SIZE is a free "did
# anything happen" signal.  Without it a 200 ms tick would grep three log files
# five times a second for nothing.
FS_MODE=""
fsize() {
  [ -z "$FS_MODE" ] && {
    if stat -c %s "$1" >/dev/null 2>&1; then FS_MODE=1
    elif stat -f %z "$1" >/dev/null 2>&1; then FS_MODE=2
    else FS_MODE=3; fi; }
  case "$FS_MODE" in
    1) stat -c %s "$1" 2>/dev/null ;;
    2) stat -f %z "$1" 2>/dev/null ;;
    *) if [ -f "$1" ]; then wc -c < "$1" 2>/dev/null | tr -d ' '; else echo 0; fi ;;
  esac
}
logsig() { printf '%s:%s' "$(fsize "$SHIMLOG")" "$(fsize "$DECLOG")"; }

SEQ=0
# NB: the counter must be bumped in THIS shell.  `$(nextseq)` would run in a
# command substitution's subshell, where the increment is lost -- every line then
# carries seq=1 (measured; it made the sequence column useless for correlation).
seq_inc() { SEQ=$((SEQ + 1)); }

# log <STATE> <event> [k=v ...]  -- one line: monotonic ms, UTC, global seq
TSYNC="${MIB3_TR_TSYNC:-0}"    # 1 = fsync every timeline line (the old behaviour)
tlog() {
  _st="$1"; shift
  seq_inc
  printf '%s %s %s %s %s\n' "$(now_ms)" "$(utc)" "$SEQ" "$_st" "$*" >> "$TLOG" 2>/dev/null
  # The timeline is INFORMATIONAL; the breadcrumb file (bc) carries the mutations
  # and is always synced.  A sync per line cost ~100 ms on the desk and would cost
  # a visible slice of a 3-second budget on the unit -- measured, not assumed.
  [ "$TSYNC" = "1" ] && sync 2>/dev/null
  printf '[%s] %-9s %s\n' "$(utc)" "$_st" "$*"
}
# a mutating step's breadcrumb survives a reset; /dev/shm does not
bc() { printf '%s %s %s\n' "$(now_ms)" "$(utc)" "$*" >> "$TBC" 2>/dev/null; sync 2>/dev/null; }
say() { printf '[%s] %s\n' "$(utc)" "$*"; }

# ============================================================================
# inputs
# ============================================================================
have() { command -v "$1" >/dev/null 2>&1; }
pids_of() { if have pidof; then pidof "$1" 2>/dev/null; elif have pgrep; then pgrep -x "$1" 2>/dev/null; fi; }
alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }

# count occurrences of a fixed string in a file (busybox-safe: no grep -c on a
# process substitution, no arrays).  NOT used by the tick any more -- shim_counts
# is incremental now (see below) because a whole-file grep per tick was the
# single most expensive thing a 5 Hz tick did.  Kept for ad-hoc/one-shot use.
count_in() { # count_in <file> <fixed string>
  [ -f "$1" ] || { echo 0; return 0; }
  _c="$(grep -acF "$2" "$1" 2>/dev/null)"   # grep -c ALWAYS prints a number
  [ -n "$_c" ] || _c=0
  echo "$_c"
}

# THE SESSION TRIGGER (requirement 1).  The shim logs exactly two lines around
# the phone's cluster stream:
#     cluster/thread: stream connected      <- the phone dialled our port
#     cluster/thread: stream closed         <- hdr/body read returned 0 or err
#     cluster/thread: exit
# Sessions are counted, not "grepped for the last line": the stream is UP iff
# there has been a `stream connected` that is not yet followed by a
# `stream closed`.  Counts survive an unchanged log and a re-opened one.
#
# *** COUNTED INCREMENTALLY (PHASE 2a, 2026-10-21). ***  $SHIMLOG is append-only
# and grows for the whole drive, and this ran on EVERY tick: two whole-file scans
# a second at the old 1 Hz, ten at Phase 2's 5 Hz.  Phase 1 measured that shape on
# dec_frames -- 80 ms per whole-file scan of a 6.6 MB log -- and the shim's log is
# the bigger of the two, so at 5 Hz the controller would have spent most of a
# loaded unit's CPU counting lines it had already counted.  The `logsig` gate in
# do_run cannot help: it skips QUIET ticks, and a growing log is exactly when the
# old code was most expensive.
# So the counts are CARRIED BETWEEN TICKS and only the bytes appended since the
# last call are scanned.  An unchanged log costs one stat() and no read at all.
# The FIRST scan of a log this process has never counted (a fresh controller, or
# one whose log just shrank) is bounded to the last MIB3_TR_SHIM_TAIL bytes for the
# same reason: at boot the log can already be tens of MB, and busybox awk over tens
# of MB is seconds.  The counts are therefore WINDOW-relative -- which is all they
# have ever been used as: `session_up` is CC > SC, and the last transition PAIR is
# always inside 256 KB (~2500 lines, tens of minutes of this log); the NAV edge
# base is rebased from CC on every entry to NAV; and `shim_rotated` compares
# against a base initialised to 0.  A window that starts mid-line can only LOSE the
# oldest line's match, never invent one, and the awk program drops that line
# outright (sf=1) so the count is never half a line.
# The offset is a LINE BOUNDARY: the shim writes each line with ONE write(2) and a
# reader on Linux never observes a partially written line (read takes the inode
# lock shared, write takes it exclusive), and the offset is re-read AFTER the
# scan, so a line appended during the scan is counted next time and never twice.
# A SHRINK means the shim was re-injected with a fresh log: the counts start over
# (that is the condition `shim_rotated` reports).
# SHIM_BYTES is how many bytes the last call actually read -- the number the bench
# asserts on, and the honest answer to "is this still bounded?".
SHIM_OFF=0; SHIM_CC=0; SHIM_SC=0; SHIM_BYTES=0; SHIM_MID=0; TAILC_OK=""
SHIM_TAIL="${MIB3_TR_SHIM_TAIL:-262144}"   # bytes of history a FRESH count reads
# `tail -c +N` is POSIX but not universal (a busybox built without it, or a pipe).
# Probe it ONCE on a 6-byte file rather than trusting it: the failure mode is a
# silently frozen counter, which would look like a dead session.
tailc_ok() {
  [ -n "$TAILC_OK" ] && return 0
  printf 'ab\ncd\n' > "$RUN/.tailprobe" 2>/dev/null
  if [ "$(tail -c +4 "$RUN/.tailprobe" 2>/dev/null)" = "cd" ]; then TAILC_OK=1; else TAILC_OK=0; fi
  rm -f "$RUN/.tailprobe" 2>/dev/null
  return 0
}
# one pass, both counts, no giant shell string: awk prints "<connected> <closed>".
# sf=1 means "the window starts mid-line": drop that line rather than count half of
# it (a suffix of a line can never contain the whole pattern, so this can only ever
# lose the oldest match -- and dropping it is what keeps the answer exact).
SHIM_COUNT_AWK='
  NR == 1 && sf == 1 { next }
  /cluster\/thread: stream connected/ { a++ }
  /cluster\/thread: stream closed/    { b++ }
  END { print a + 0, b + 0 }
'
shim_counts() {
  tailc_ok
  _sz="$(fsize "$SHIMLOG")"
  case "$_sz" in ''|*[!0-9]*) _sz=0 ;; esac
  if [ "$_sz" -lt "${SHIM_OFF:-0}" ]; then SHIM_OFF=0; SHIM_CC=0; SHIM_SC=0; fi
  SHIM_MID=0
  if [ "${SHIM_OFF:-0}" = "0" ] && [ "$_sz" -gt "$SHIM_TAIL" ]; then
    SHIM_OFF=$(( _sz - SHIM_TAIL )); SHIM_MID=1
  fi
  if [ "$_sz" -gt "${SHIM_OFF:-0}" ]; then
    SHIM_BYTES=$(( _sz - SHIM_OFF ))
    if [ "$TAILC_OK" = "1" ]; then
      _cnt="$(tail -c +$(( SHIM_OFF + 1 )) "$SHIMLOG" 2>/dev/null | awk -v sf="$SHIM_MID" "$SHIM_COUNT_AWK")"
    else
      # no usable `tail -c +N`: count the whole file.  Correct, just not bounded --
      # and the RUN banner says so, so a slow unit is explained instead of blamed.
      _cnt="$(awk -v sf=0 "$SHIM_COUNT_AWK" "$SHIMLOG" 2>/dev/null)"
      SHIM_OFF=0; SHIM_CC=0; SHIM_SC=0
      SHIM_BYTES="$_sz"
    fi
    case "$_cnt" in
      *' '*) SHIM_CC=$(( SHIM_CC + ${_cnt%% *} )); SHIM_SC=$(( SHIM_SC + ${_cnt##* } )) ;;
      *)     : ;;                                   # awk said nothing: leave as is
    esac
    # after the scan: a line appended while we read is counted next time, never twice
    _sz2="$(fsize "$SHIMLOG")"
    case "$_sz2" in ''|*[!0-9]*) _sz2="$_sz" ;; esac
    [ "$_sz2" -ge "$SHIM_OFF" ] && SHIM_OFF="$_sz2"
  else
    SHIM_BYTES=0
  fi
  CC="$SHIM_CC"; SC="$SHIM_SC"
}
session_up()  { [ "$CC" -gt "$SC" ]; }
# a shrink means the shim was re-injected (a fresh log): treat as a new world
shim_rotated() { [ "${CC:-0}" -lt "${LAST_CC:-0}" ] || [ "${SC:-0}" -lt "${LAST_SC:-0}" ]; }

# the decoder's REAL frame counter.  NOTE THE TRAP: `FRAME n luma=` lines are
# printed only for #1-8 and every 50th, so counting log lines is wrong.  The
# STATUS line's `frames=` is the counter.
#
# *** IT MUST BE THE COUNTER OF THE DECODER THAT IS RUNNING NOW (2026-10-21,
#     PLAN_SWITCH_LATENCY V1). ***  $DECLOG is APPEND-ONLY and a decoder is
#     started per session with its counters at zero, so "the last `frames=` in
#     the file" is the PREVIOUS process's total for the first ~2 s of every new
#     session (a STATUS line is printed only every 200 loop iterations).  The
#     PENDING gate captures PEND_F_BASE in exactly that window, then needs
#     `base + NFRAMES` -- i.e. 678 frames of a process that only reaches ~675 in
#     the whole 45 s PENDMAX.  RESULT: the second navigation session in a boot
#     can NEVER switch; it times out to NAV.  Reproduced on the desk with the
#     unmodified function: session 1 fine, session 2 blocked.
#     The fix is SHELL-ONLY (no binary, no pin moves): every decoder process
#     announces itself with `v4l2_seq: file=...` as its first line, so the
#     counter is the last `frames=` AFTER the last such banner -- and 0 if that
#     decoder has not printed a STATUS yet.
#     BOUNDED: only the last MIB3_TR_DEC_TAIL bytes are read (the log grows all
#     drive, and this runs on every tick of a 5 Hz controller).  64 KB is ~a
#     minute of the decoder's own log -- it prints a STATUS every 200 loop
#     iterations, i.e. every ~0.4 s when idle and ~13 s at 15 fps -- so the last
#     counter is always inside it, and a NEW decoder's banner only has to be
#     inside it for the few seconds after its start, when it is the last line of
#     the file.  If the window holds NEITHER a banner nor a counter (never seen in
#     practice), the same query is repeated once over the whole file: a decoder
#     that has outlived the window must not read as 0, because a false 0 looks
#     exactly like a stalled session and would tear a healthy stream down.
DEC_FRAMES_AWK='
  /v4l2_seq: file=/ { v = ""; b = 1 }
  { i = index($0, "frames=")
    if (i > 0) { s = substr($0, i + 7); sub(/[^0-9].*/, "", s); if (s != "") { v = s; b = 1 } } }
  END { print (v != "" ? v : (b ? "B" : "N")) }
'
dec_frames() {
  [ -f "$DECLOG" ] || { echo 0; return; }
  _v="$(tail -c "${MIB3_TR_DEC_TAIL:-65536}" "$DECLOG" 2>/dev/null | awk "$DEC_FRAMES_AWK")"
  case "$_v" in
    N|'') _v="$(awk "$DEC_FRAMES_AWK" "$DECLOG" 2>/dev/null)" ;;
  esac
  case "$_v" in ''|*[!0-9]*) _v=0 ;; esac   # "B" (banner, no counter yet) -> 0
  echo "$_v"
}

# *** THE FIRST-DECODED-FRAME EVENT (PHASE 2b, 2026-10-21). ***
# The PENDING gate used to wait for `dec_frames` to move, i.e. for a STATUS line --
# and v4l2_seq prints STATUS only every 200 loop iterations, which is ~2 s.  The
# phone has usually already sent a burst by then, so the gate added up to 2 s of
# pure wait to every switch (field: 05:23:33 -> 05:23:35).
# The decoder ALREADY prints a line per frame for frames 1-8 and every 50th
# (v4l2_seq.c:963):
#       FRAME <n> luma=<l> drops=<d> auskip=<a> send=<ms>ms
# so the event is "the first `FRAME ` line after the current decoder's start
# banner" -- no binary change, no pin move, and it is the true moment the decode
# path produced a picture (the line is printed immediately before the RGBA send).
# SCOPING IS THE WHOLE POINT: a bare `grep FRAME` over the whole append-only log
# would fire on the PREVIOUS session's frames, which is V1 in a new costume.  The
# same rule as dec_frames: a banner resets the answer, so only frames printed
# after the last `v4l2_seq: file=` count.
# WHY NOT the presenter's `idle-blank: OFF (a frame arrived)`: that only fires on a
# blank -> live transition, and after a neutral feed the presenter is never blank
# again, so it would miss every session after the first.
# NOT USED: a luma threshold.  The first frames are dark (FRAME 1 luma=16, FRAME 6
# luma=0) and a night-mode map is legitimately dark too, so any threshold would
# either reject a real frame or be arbitrary.  The switch itself takes 1-2 s, by
# which time the picture is live.
# BOUNDED to the same MIB3_TR_DEC_TAIL window, and it only ever runs while the
# controller is in PENDING (never on a NAV/CARPLAY tick).
DEC_FRAME_AWK='
  /v4l2_seq: file=/ { f = 0 }
  /^ *FRAME [0-9]/  { f = 1 }
  END { print (f ? 1 : 0) }
'
dec_first_frame() {
  [ -f "$DECLOG" ] || { echo 0; return; }
  # No banner in the window is not a "no": a decoder that started before the
  # window and is still delivering has FRAME lines in it, and awk answers 1 for
  # exactly that case (f is only reset by a banner it can see).
  _v="$(tail -c "${MIB3_TR_DEC_TAIL:-65536}" "$DECLOG" 2>/dev/null | awk "$DEC_FRAME_AWK")"
  case "$_v" in 1) echo 1 ;; *) echo 0 ;; esac
}
# Has THIS decoder process ever accepted a helper connection?  The decoder prints
# "online: FIRST CONNECTION -> renegotiate" on its first accept per process, and
# starts every process with "v4l2_seq: file=...".  So: does a FIRST CONNECTION
# appear after the last start line?  If not, the decoder is unused and its
# capture port has never been renegotiated -- safe to ADOPT instead of killing it.
dec_had_client() {
  [ -f "$DECLOG" ] || return 1
  awk '/v4l2_seq: file=/{seen=0} /online: FIRST CONNECTION/{seen=1} END{exit (seen?0:1)}' "$DECLOG" 2>/dev/null
}
# DEC_PID is the pid WE started (persisted).  pids_of alone is not enough: if the
# name lookup misses, the controller would think no decoder exists, start a second
# one, and leave the first holding the AU socket -- which is the documented wedge.
dec_alive()  { [ -n "$(pids_of "${MIB3_TR_DECNAME:-v4l2_seq}")" ] || { [ -n "${DEC_PID:-}" ] && alive "$DEC_PID"; }; }
pres_alive() { [ -n "$(pids_of "${MIB3_TR_EGLNAME:-mib3egl}")" ]; }

# ============================================================================
# actuators (the ONLY things that touch the system)
# ============================================================================
smack_grants() {
  # the shim runs as carplay::app and AF_UNIX connect is checked BOTH ways; the
  # base image allows neither direction.  RAM-only, idempotent, cheap.
  chsmack -a carplay::app "$AUSOCK" >/dev/null 2>&1
  [ -w /sys/fs/smackfs/load2 ] || return 0
  echo "common::lib carplay::app rwx" > /sys/fs/smackfs/load2 2>/dev/null
  echo "carplay::app common::lib rwx" > /sys/fs/smackfs/load2 2>/dev/null
}

presenter_up() {
  pres_alive && { tlog "$STATE" actuator presenter=already-running pid="$(pids_of "${MIB3_TR_EGLNAME:-mib3egl}")"; return 0; }
  [ -x "$EGL" ] || { tlog "$STATE" actuator presenter=MISSING path="$EGL"; return 1; }
  # NOTE the 4th positional: 0 = UNBOUNDED.  A positive value (the house runbook
  # says 3600) makes mib3egl self-terminate after secs*fps frames -- an hour at
  # 30 fps -- and when the presenter goes, MS-S logs "does not exist", stops
  # encoding our surface, and the cluster retains the last frame: the lie comes
  # back on a long drive.  A controller that is meant to run all day must pass 0.
  XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/platform/weston}" \
  WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
    nohup "$EGL" "$IVIID" "$W" "$H" 0 00ff00 --persist --fps 30 \
      --rgba-sock "$RGBASOCK" --idle-blank-ms "${MIB3_TR_IDLE_BLANK_MS:-8000}" >> "$EGLLOG" 2>&1 &
  bc "presenter:start"
  _i=0; while [ "$_i" -lt 8 ]; do pres_alive && break; sleep 1; _i=$((_i+1)); done
  pres_alive || { tlog "$STATE" actuator presenter=FAIL; return 1; }
  tlog "$STATE" actuator presenter=started pid="$(pids_of "${MIB3_TR_EGLNAME:-mib3egl}")" wait="${_i}s"
  return 0
}

# A NEW decoder process per session is an INVARIANT: the vendor's capture-port
# renegotiate is safe only on a decoder's FIRST connection, and a second
# renegotiate mid-stream wedges the output queue (capret=0, nfree=0).  So we
# never reuse: stop, then start fresh.
decoder_down() {
  _p="$(pids_of "${MIB3_TR_DECNAME:-v4l2_seq}")"
  if [ -z "$_p" ] && [ -n "${DEC_PID:-}" ] && alive "$DEC_PID"; then _p="$DEC_PID"; fi
  if [ -z "$_p" ]; then tlog "$STATE" actuator decoder=already-absent; DEC_PID=""; return 0; fi
  bc "decoder:stop $_p"
  _t0="$(now_ms)"
  kill $_p 2>/dev/null
  # SIGTERM + reap, polled at 50 ms: a fixed 1 s sleep per step is 1 s the driver
  # waits for nothing.  SIGKILL stays as the 6 s belt (the vidc wedge).
  _i=0
  while [ "$_i" -lt 120 ]; do alive "$_p" || break; nap 0.05; _i=$((_i+1)); done
  if alive "$_p"; then
    kill -9 $_p 2>/dev/null
    _i=0; while [ "$_i" -lt 40 ]; do alive "$_p" || break; nap 0.05; _i=$((_i+1)); done
  fi
  tlog "$STATE" actuator decoder=stopped pid="$_p" wait="$(( $(now_ms) - _t0 ))ms"
  DEC_PID=""
  return 0
}
decoder_up() {
  # *** ADOPT A PRE-ARMED DECODER -- do not kill it. ***
  # The phone emits roughly ONE keyframe per stream and it arrives in the first
  # second.  Starting a decoder here costs ~6-8 s of V4L2 setup, during which the
  # shim has no helper and DROPS that keyframe (measured 2026-10-20: shim
  # `sink: helper absent` then `FIRST IDR at frame=1`, decoder `keyframe wait
  # timed out -> feeding best-effort` => permanently artifacted picture).
  # So bring_up.sh pre-arms one BEFORE the phone can connect, and this function
  # only starts a decoder when there is none, or when the one present has
  # already served a session (the fresh-decoder-per-session invariant).
  _p="$(pids_of "${MIB3_TR_DECNAME:-v4l2_seq}")"
  if [ -n "$_p" ] && ! dec_had_client; then
    tlog "$STATE" actuator decoder=adopted pid="$_p" note=pre-armed-no-client
    return 0
  fi
  decoder_down >/dev/null 2>&1
  [ -x "$DEC" ] || { tlog "$STATE" actuator decoder=MISSING path="$DEC"; return 1; }
  # the AU socket is OURS (the decoder listens on it).  NEVER remove the
  # presenter's rgba socket here: the presenter owns that path, and unlinking it
  # would leave a live presenter whose socket nobody can reach.
  rm -f "$AUSOCK" 2>/dev/null
  bc "decoder:start"
  # *** ONE GENERATION, ONE QLIM (2026-10-26) ***: this default is 4 to match
  # the binary's own default (V4L2_RGBA_QLIM, raised 2026-10-25) and
  # bring_up.sh's MIB3_RGBA_QLIM.  It was still 2 here -- a decoder started by
  # the controller after a mid-session restart silently shipped with the OLD
  # 1.125-frame drop threshold, i.e. the exact 31%-of-frames loss F2 fixed.
  # The three launch sites (bring_up, transition_ctl, hand) must not disagree.
  # MIB3_NV12=1 (+ MIB3_NV12_AT=N): the operator's REAL-NV12 dump window (the
  # artifact measurement, now available in ONLINE mode too -- see v4l2_seq.c).
  if [ -n "${MIB3_NV12:-}" ]; then
    V4L2_FDFIELD=3 V4L2_IN_SOCK="$AUSOCK" V4L2_RGBA_SOCK="$RGBASOCK" V4L2_OUTN=8 \
    V4L2_RGBA_QLIM="${MIB3_RGBA_QLIM:-4}" V4L2_NV12=1 V4L2_NV12_AT="${MIB3_NV12_AT:-0}" \
      nohup "$DEC" online 0 >> "$DECLOG" 2>&1 &
  else
    V4L2_FDFIELD=3 V4L2_IN_SOCK="$AUSOCK" V4L2_RGBA_SOCK="$RGBASOCK" V4L2_OUTN=8 \
    V4L2_RGBA_QLIM="${MIB3_RGBA_QLIM:-4}" \
      nohup "$DEC" online 0 >> "$DECLOG" 2>&1 &
  fi
  _i=0; while [ "$_i" -lt 8 ]; do [ -S "$AUSOCK" ] && break; sleep 1; _i=$((_i+1)); done
  smack_grants
  DEC_PID="$!"
  [ -z "$(pids_of "${MIB3_TR_DECNAME:-v4l2_seq}")" ] || DEC_PID="$(pids_of "${MIB3_TR_DECNAME:-v4l2_seq}")"
  tlog "$STATE" actuator decoder=started pid="${DEC_PID:-?}" sock=$([ -S "$AUSOCK" ] && echo up || echo MISSING) wait="${_i}s"
  save_state
  return 0
}

# build the neutral frame once (py3 / py2 / dd-doubling; the unit's python is 2.7)
mk_neutral() {
  _need=$((W * H * 4))
  [ -f "$NEUTRAL" ] && [ "$(wc -c < "$NEUTRAL" 2>/dev/null | tr -d ' ')" = "$_need" ] && return 0
  mkdir -p "${NEUTRAL%/*}" 2>/dev/null
  if have python3; then
    python3 -c "import sys;sys.stdout.buffer.write(b'\x80\x80\x80\xff'*($W*$H))" > "$NEUTRAL" 2>/dev/null && return 0
  fi
  if have python; then
    python -c "import sys;sys.stdout.write('\x80\x80\x80\xff'*($W*$H))" > "$NEUTRAL" 2>/dev/null && return 0
  fi
  # last resort: no interpreter -- grow a 4-byte pixel by doubling (busybox dd)
  printf '\200\200\200\377' > "${NEUTRAL}.p" 2>/dev/null || return 1
  _n=4
  while [ "$_n" -lt "$_need" ]; do cat "${NEUTRAL}.p" "${NEUTRAL}.p" > "${NEUTRAL}.q" 2>/dev/null && mv "${NEUTRAL}.q" "${NEUTRAL}.p"; _n=$((_n * 2)); done
  dd if="${NEUTRAL}.p" of="$NEUTRAL" bs="$_need" count=1 2>/dev/null
  rm -f "${NEUTRAL}.p" "${NEUTRAL}.q" 2>/dev/null
  [ "$(wc -c < "$NEUTRAL" 2>/dev/null | tr -d ' ')" = "$_need" ]
}

# Put a NEUTRAL frame on the surface we own.  This is the step that actually
# stops the cluster showing a road that is no longer there: the presenter keeps
# re-committing whatever was fed to it last, so one neutral frame is enough --
# and the socket is released again immediately, so the next session's decoder
# can take it.
neutral_show() {
  [ -x "$RGBAFEED" ] || { tlog "$STATE" neutral=SKIP no-rgba_feed path="$RGBAFEED"; return 1; }
  mk_neutral || { tlog "$STATE" neutral=FAIL cannot-build "$NEUTRAL"; return 1; }
  # WAIT FOR THE PRESENTER TO LET GO (an event, not a sleep).  The presenter has
  # ONE rgba client: while the decoder still holds it a feeder can only retry.
  # The decoder is stopped immediately before this, so the log line arrives within
  # milliseconds -- the old code waited up to a second per attempt for it, which
  # is most of the "neutral frame: 5 s" the field measured.
  _w=0
  while [ "$_w" -lt 40 ]; do
    grep -qa 'rgba-sock client gone -> re-accept' "$EGLLOG" 2>/dev/null && break
    alive "$(pids_of "${MIB3_TR_DECNAME:-v4l2_seq}")" || break
    nap 0.05; _w=$((_w+1))
  done
  _i=0
  while [ "$_i" -lt "$FEED_TRIES" ]; do
    _ft0="$(now_ms)"
    "$RGBAFEED" "$RGBASOCK" "$NEUTRAL" "$W" "$H" --count "$FEEDCOUNT" --fps 10 \
        > "$RUN/neutral.log" 2>&1 &
    _fp=$!
    _g=0; while [ "$_g" -lt $(( FEED_T * 20 )) ]; do alive "$_fp" || break; nap 0.05; _g=$((_g+1)); done
    alive "$_fp" && { kill -9 "$_fp" 2>/dev/null; tlog "$STATE" neutral=TIMEOUT wait="$(( $(now_ms) - _ft0 ))ms"; }
    if grep -qa 'frames sent' "$RUN/neutral.log" 2>/dev/null; then
      tlog "$STATE" neutral=OK frames="$(grep -ao 'frames sent[= ]*[0-9]*' "$RUN/neutral.log" | tail -n1 | sed 's/frames sent[= ]*//')" cost="$(( $(now_ms) - _ft0 ))ms"
      return 0
    fi
    _i=$((_i+1)); nap 0.2
  done
  tlog "$STATE" neutral=FAIL presenter-not-reading sock="$RGBASOCK"
  return 1
}

# ============================================================================
# the switch (both directions) -- always deliver_ctl.sh's oracle-gated verbs
# ============================================================================
# HOW THE CONTROLLER CALLS THE SWITCH (2026-10-26 hardening):
#   * the deliver_ctl output is APPENDED to delivery.log, and only the bytes
#     THIS call appended may be used for reporting.  The old code grepped the
#     WHOLE file for the last "HANDOVER TIMING" line -- so a failed attempt
#     that printed none REPLAYED a stale breakdown from an earlier run,
#     byte-identical every time (measured on the car 2026-10-25:
#     "breakdown stage=back total=15360ms ... rescue=10400ms" after every
#     rc=1).  The breakdown is now scoped to the appended window, and an
#     attempt that printed no timing line says so instead of lying.
#   * on failure the LAST verdict line of the appended window is copied to the
#     timeline, so a fast rc=1 (the 240 ms mystery) is diagnosable from ONE
#     attempt: "ABORT: another deliver_ctl", "HANDOVER NO-GO: no presenter",
#     "bad display uri", ... instead of a bare rc.
switch_locked_call() {
  # switch_locked_call <log-tag> <deliver-verb> <args...> -> rc
  #   <log-tag> keeps the OLD line shapes exactly: "to-carplay" logs
  #   "switch=to-carplay rc=.. cost=..ms", "to-nav" logs "back=to-nav ...".
  _tag="$1"; _dir="$2"; shift 2
  [ -f "$CTL" ] || { tlog "$STATE" "switch=FAIL no-ctl path=$CTL"; SW_REASON="no deliver_ctl at $CTL"; return 1; }
  _sz0=0
  [ -f "$RUN/delivery.log" ] && _sz0="$(wc -c < "$RUN/delivery.log" 2>/dev/null | tr -d ' ')"
  case "$_sz0" in ''|*[!0-9]*) _sz0=0 ;; esac
  _t0="$(now_ms)"
  sh "$CTL" handover "$@" >> "$RUN/delivery.log" 2>&1
  _rc=$?
  _d=$(( $(now_ms) - _t0 ))
  case "$_dir" in
    to-nav) tlog "$STATE" "back=to-nav rc=$_rc cost=${_d}ms" ;;
    *)      tlog "$STATE" "switch=$_dir rc=$_rc cost=${_d}ms" ;;
  esac
  # ONLY the bytes this call appended (empty window = the call said nothing).
  _seg=""
  if [ "$_sz0" -gt 0 ]; then _seg="$(tail -c +$((_sz0 + 1)) "$RUN/delivery.log" 2>/dev/null)"
  elif [ -s "$RUN/delivery.log" ]; then _seg="$(cat "$RUN/delivery.log" 2>/dev/null)"; fi
  SW_REASON=""
  if [ "$_rc" != "0" ]; then
    SW_REASON="$(printf '%s\n' "$_seg" | grep -aE "ABORT|NO-GO|FAIL|WARN|refused|cannot|missing" | tail -n 1)"
    [ -n "$SW_REASON" ] && tlog "$STATE" "switch-reason: $(printf '%s' "$SW_REASON" | cut -c1-160)"
  fi
  SW_BREAKDOWN="$(printf '%s\n' "$_seg" | grep -a "HANDOVER TIMING" | tail -n 1)"
  if [ -n "$SW_BREAKDOWN" ]; then
    tlog "$STATE" "breakdown $(printf '%s' "$SW_BREAKDOWN" | sed 's/^.*HANDOVER TIMING //')"
  elif [ "$_rc" = "0" ]; then
    tlog "$STATE" "breakdown: none printed this attempt (rc=0 -- a DEGRADED/no-oracle run)"
  else
    tlog "$STATE" "breakdown: none printed this attempt (failed before the first timed step)"
  fi
  return $_rc
}
switch_to_carplay() {
  # ONE locked invocation for the whole up direction (2026-10-20).  It used to be
  # three (`park`, `attach`, `arm`) -- three lock cycles, three oracle checks and
  # three gate waits, which is where most of the field's 65 s went.  Inside,
  # `handover switch` still does the SAME steps in the SAME order (see its header):
  # park+attach in one MS-S window, the framerate write after the churn and before
  # the resync, then the resync/arm -- which is the trigger the ECU re-acquires on.
  presenter_up || true
  bc "switch:to-carplay"
  FPSD="${MIB3_STREAM_FPS_DEFAULT:-15}"
  case "$FPSD" in ''|0) FPSD=0 ;; esac
  switch_locked_call up to-carplay switch "$FPSD"
  return $?
}
hand_back_to_nav() {
  bc "switch:to-nav"
  switch_locked_call down to-nav back "$RESTORE_T"
  return $?
}

# ============================================================================
# state
# ============================================================================
STATE="NAV"; HOLD_START=0; FRAME_LAST=0; ATTEMPTS=0; T_ENTER=0; T_FB=0; PAUSE_LOGGED=0
NAV_CC_BASE=0; PEND_F_BASE=0; FLAPS=0; T_END=0; DEC_PID=""
STATE_LOADED=0
load_state() {
  [ -f "$TSTATE" ] || return 0
  # shellcheck disable=SC2046
  set -- $(cat "$TSTATE" 2>/dev/null)
  _nf=$#
  [ "$_nf" -ge 6 ] || return 0
  STATE="$1"; HOLD_START="$2"; FRAME_LAST="$3"; ATTEMPTS="$4"; T_ENTER="$5"; T_FB="$6"
  # 2026-10-20: the connect-edge base, the PENDING frame base, the flap counter and
  # the last-end time are persisted too -- a controller restart (or the one-tick
  # `once` verb the bench uses) must not re-fire a transition it already acted on,
  # and the flap backoff must survive across ticks to count flaps at all.
  [ "$_nf" -ge 7 ]  && SEQ="$7"
  [ "$_nf" -ge 10 ] && { NAV_CC_BASE="$8"; PEND_F_BASE="$9"; FLAPS="${10}"; }
  [ "$_nf" -ge 11 ] && T_END="${11}"
  [ "$_nf" -ge 12 ] && DEC_PID="${12}"
  case "$SEQ" in ''|*[!0-9]*) SEQ=0 ;; esac
  case "$NAV_CC_BASE" in ''|*[!0-9]*) NAV_CC_BASE=0 ;; esac
  case "$PEND_F_BASE" in ''|*[!0-9]*) PEND_F_BASE=0 ;; esac
  case "$FLAPS" in ''|*[!0-9]*) FLAPS=0 ;; esac
  case "$T_END" in ''|*[!0-9]*) T_END=0 ;; esac
  case "${DEC_PID:-}" in ''|*[!0-9]*) DEC_PID="" ;; esac
  STATE_LOADED=1
  return 0
}
save_state() { printf '%s %s %s %s %s %s %s %s %s %s %s %s\n' "$STATE" "${HOLD_START:-0}" "${FRAME_LAST:-0}" \
                 "${ATTEMPTS:-0}" "${T_ENTER:-0}" "${T_FB:-0}" "${SEQ:-0}" \
                 "${NAV_CC_BASE:-0}" "${PEND_F_BASE:-0}" "${FLAPS:-0}" "${T_END:-0}" "${DEC_PID:-0}" \
                 > "$TSTATE" 2>/dev/null; }

# set_state <NEW> <why>  -- logs the transition with its duration in ms
set_state() {
  _new="$1"; _why="$2"; _now="$(now_ms)"
  if [ "$_new" = "$STATE" ]; then return 0; fi
  _dwell=0
  [ "${T_ENTER:-0}" != "0" ] && _dwell=$(( _now - T_ENTER ))
  tlog "$_new" "ENTER why=$_why from=$STATE dwell=${_dwell}ms"
  STATE="$_new"; T_ENTER="$_now"; HOLD_START=0
  [ "$_new" = "PENDING" ] && ATTEMPTS=0
  # entering NAV: rebase the "has a frame arrived" marker, so a NEW session is
  # recognised by FRAMES ADVANCING from here -- see the NAV branch.
  [ "$_new" = "NAV" ] && { NAV_F_BASE="$(dec_frames)"; NAV_CC_BASE="${CC:-0}"; PEND_F_BASE=0; }
  save_state
}

# how long a stalled frame counter must last before we call the session over.
# A second session starting soon after an end is a FLAP (the unit measured 8
# connects / 7 disconnects in one run), and each teardown costs a decoder start
# plus a TransportStream rebuild.  So the hold DOUBLES per recent flap, up to
# HOLDMAX: a normal disconnect reacts in HOLD, a flapping one settles instead of
# thrashing.  It resets after FLAPWIN seconds of stability.
#
# THE VALUE IS DELIVERED IN WHOLE MILLISECONDS, and that is not a style choice:
# HOLD is fractional (1.5 s) and `$(( HOLD * 1000 ))` in a shell whose arithmetic
# is integer-only is a FATAL syntax error -- it killed the controller outright on
# the desk, which on the car is a stale map with nothing watching it.  awk does the
# arithmetic, the shell only ever compares integers.
hold_ms() {
  awk -v h="${HOLD:-1.5}" -v m="${HOLDMAX:-6}" -v f="${FLAPS:-0}" 'BEGIN{
    v = h + 0; mx = m + 0; n = f + 0;
    if (n > 4) n = 4;
    for (i = 0; i < n; i++) v = v * 2;
    if (v > mx) v = mx;
    printf "%d", v * 1000 + 0.5
  }'
}

# ============================================================================
# the SHUTDOWN work (bounded; both verdicts logged)
# ============================================================================
do_shutdown() {
  _why="$1"
  t0="$(now_ms)"
  # a session that ends soon after the previous one is a FLAP: remember it, so
  # the next stall is judged with a longer hold (see hold_for) instead of tearing
  # the cluster down twice in a minute.  Cleared once a session has been stable.
  if [ "${T_END:-0}" != "0" ] && [ $(( t0 - T_END )) -le $(( FLAPWIN * 1000 )) ]; then
    FLAPS=$(( ${FLAPS:-0} + 1 ))
  else
    FLAPS=0
  fi
  T_END="$t0"
  # enter SHUTDOWN as a real state FIRST, so every actuator line below is labelled
  # with the phase it belongs to (not with the phase we are leaving)
  set_state "SHUTDOWN" "$_why"
  tlog "SHUTDOWN" "BEGIN why=$_why flaps=$FLAPS (decoder -> neutral -> nav)"
  # 1. stop the decoder: (a) the presenter's input stops growing, (b) the NEXT
  #    session must get a NEW decoder process anyway.
  decoder_down || true
  t_dec="$(( $(now_ms) - t0 ))"
  # 2. neutral frame -- the unconditional half.  Without it the cluster keeps
  #    our last decoded map for as long as MS-S keeps encoding our surface.
  neutral_show || true
  t_neu="$(( $(now_ms) - t0 ))"
  tlog "SHUTDOWN" "t_decoder=${t_dec}ms t_neutral=${t_neu}ms"
  # 3. hand the cluster back to the car's own map (the pleasant half; can fail).
  _tb="$(now_ms)"
  if hand_back_to_nav; then
    t_nav="$(( $(now_ms) - t0 ))"
    tlog "SHUTDOWN" "t_back=$(( $(now_ms) - _tb ))ms"
    tlog "SHUTDOWN" "END verdict=NAV t_total=${t_nav}ms"
    set_state "NAV" "handback-ok"
  else
    t_fb="$(( $(now_ms) - t0 ))"
    tlog "SHUTDOWN" "t_back=$(( $(now_ms) - _tb ))ms"
    tlog "SHUTDOWN" "END verdict=FALLBACK t_total=${t_fb}ms reason=nav-handback-refused"
    T_FB="$t_fb"
    set_state "FALLBACK" "handback-failed"
  fi
  # 4. RE-ARM THE DECODER NOW, not at the next PENDING.  The phone emits roughly
  #    ONE keyframe per stream, in the first second, and the shim DROPS frames
  #    while no helper is connected.  Starting the decoder when the next session
  #    appears costs 6-8 s of V4L2 setup, so that keyframe is gone by the time the
  #    AU socket exists -> mid-GOP decode -> artifacts that never heal (measured
  #    on the car 2026-10-20).  An idle, listening decoder costs nothing (no phone
  #    -> no frames), and the next session's shim connects to it immediately.
  if [ "$STATE" != "CARPLAY" ]; then
    decoder_up || true
    tlog "$STATE" "re-armed decoder for the next session (listening, no client)"
  fi
}

# ============================================================================
# one evaluation tick
# ============================================================================
eval_once() {
  shim_counts
  if [ "${STATE_LOADED:-0}" = "1" ] && shim_rotated; then
    tlog "$STATE" "note=shim-log-rotated (re-injected?) cc=$CC sc=$SC last=${LAST_CC:-?}/${LAST_SC:-?}"
  fi
  LAST_CC="$CC"; LAST_SC="$SC"
  F="$(dec_frames)"
  UP=0; session_up && UP=1

  case "$STATE" in
  NAV)
    # A phone session that is feeding us begins a PENDING window; the car's map
    # stays up for the whole of it (we touch nothing here -- that is the point).
    #
    # *** THE TRIGGER IS AN EDGE, NOT A LEVEL. ***
    # The shim logs `cluster/thread: conn fd=` / `stream connected` the instant
    # the phone dials.  Reacting to it here is what makes the switch feel event
    # driven instead of polled -- and it is safe as an EDGE because the count is
    # rebased every time we enter NAV (NAV_CC_BASE), so a stale tally from a
    # previous session cannot re-trigger it.  (The shim does NOT reliably log
    # `stream closed` -- measured 2026-10-20, three sessions, zero closes -- so a
    # count *difference* is meaningless; an EDGE is exactly right.)
    # Frames advancing stays as the second, independent trigger: it covers a
    # controller that was started after the phone had already connected.
    _go=0; _why=""
    if [ "${CC:-0}" -gt "${NAV_CC_BASE:-0}" ]; then _go=1; _why="connect-edge"
    elif [ "$F" -gt "${NAV_F_BASE:-0}" ] && dec_alive; then _go=1; _why="frames-advancing"; fi
    if [ "$_go" = "1" ]; then
      # A NEW decoder process per session is an invariant: the vendor's capture
      # port renegotiate is safe only on a decoder's FIRST connection, and a
      # second one mid-stream wedges the output queue.  So if the phone starts a
      # session and nothing is decoding, this controller starts a fresh decoder
      # (and never reuses one across sessions -- see SHUTDOWN/decoder_down).
      # ORDER IS LOAD-BEARING (chain_up.sh): the presenter owns ivi $IVIID and the
      # rgba socket the decoder writes into, so it goes up first.  It must be up
      # before the frames arrive -- and it must keep running for the whole drive.
      # the DETECTION is logged the instant it happens; the actuator work is timed
      # separately, because that is the number a latency question is actually about
      # (and on the car the decoder is pre-armed, so it is ~0 -- see bring_up.sh).
      tlog "NAV" "detect why=$_why cc=$CC base=$NAV_CC_BASE frames=$F hold=$(hold_ms)ms flaps=$FLAPS"
      _ta="$(now_ms)"
      if ! pres_alive; then presenter_up || true; fi
      if ! dec_alive; then decoder_up || true; fi
      PEND_F_BASE="$(dec_frames)"
      tlog "NAV" "actuators-ready in $(( $(now_ms) - _ta ))ms presenter=$(pres_alive && echo up || echo absent) decoder=$(dec_alive && echo up || echo absent)"
      set_state "PENDING" "$_why"
      FRAME_LAST="$F"; save_state
    fi
    ;;
  PENDING)
    # ONE decodable frame is enough.  It used to be three, on the theory that the
    # path had to be proven live first -- but a frame that reached the decoder's
    # RGBA socket IS the whole path (decode -> texture).  Waiting for three cost
    # two extra frame periods for nothing, and the presenter's dead-man switch
    # makes a partial first frame impossible to mistake for a picture.
    #
    # AND "A FRAME ARRIVED" IS NOW AN EVENT, NOT A COUNTER (PHASE 2b, 2026-10-21).
    # `dec_frames` only moves when the decoder prints a STATUS line, and it prints
    # one every 200 loop iterations -- ~2 s -- so the gate sat on a 2 s clock while
    # the phone had already sent a burst (field: detect 05:23:33, switch 05:23:35).
    # `dec_first_frame` reads the decoder's own per-frame line, scoped to the
    # RUNNING decoder (a banner resets it, exactly like dec_frames -- a bare
    # `grep FRAME` over the append-only log would fire on the previous session's
    # frames, which is V1 in a new costume).
    # The counter stays as the FALLBACK, so MIB3_TR_NFRAMES_EVENT=0 restores the
    # old gate exactly and a decoder that somehow prints no FRAME line still
    # switches.
    _ev=0
    [ "$NFRAMES_EVENT" = "1" ] && _ev="$(dec_first_frame)"
    if [ "$_ev" = "1" ] || [ "$F" -ge $(( ${PEND_F_BASE:-0} + NFRAMES )) ]; then
      tlog "PENDING" "frames=$F (+$((F - ${PEND_F_BASE:-0})) since detect) event=$_ev >= $NFRAMES -> switching"
      if switch_to_carplay; then
        set_state "CARPLAY" "switch-ok frames=$F"
        FRAME_LAST="$F"; save_state; return 0
      fi
      ATTEMPTS=$((ATTEMPTS + 1))
      if [ "$ATTEMPTS" -ge "$MAXATT" ]; then
        tlog "PENDING" "giving-up after $ATTEMPTS attempts (nav is still up; nothing of ours is on screen)"
        set_state "NAV" "switch-failed-attempts=$ATTEMPTS"; save_state; return 0
      fi
      tlog "PENDING" "switch-attempt-$ATTEMPTS-failed; retrying (nav still up)"
      save_state; sleep "$RETRY"; return 0
    fi
    if [ -z "${PEND_START:-}" ]; then
      PEND_START="$(now_ms)"; tlog "PENDING" "pend-timer-start max=${PENDMAX}s"
    fi
    _timedout=0
    [ $(( $(now_ms) - PEND_START )) -ge $(( PENDMAX * 1000 )) ] && _timedout=1
    if [ "$UP" = "0" ] || [ "$_timedout" = "1" ]; then
      do_shutdown "pending-timeout up=$UP frames=$F waited=$(( $(now_ms) - PEND_START ))ms"
      return 0
    fi
    ;;
  CARPLAY|FALLBACK)
    if [ "$STATE" = "CARPLAY" ]; then
      _now="$(now_ms)"
      if [ "$F" -gt "$FRAME_LAST" ]; then
        if [ "$HOLD_START" != "0" ]; then
          _d=$(( _now - HOLD_START )); [ "$_d" -lt 0 ] && _d=0
          _cap=$(( $(hold_ms) * 10 ))
          [ "$_d" -gt "$_cap" ] && _d="$(hold_ms)"
          tlog "CARPLAY" "frames-resumed after ${_d}ms"
        fi
        HOLD_START=0; FRAME_LAST="$F"; PAUSE_LOGGED=0; save_state
      elif [ "$UP" = "0" ]; then
        do_shutdown "stream-closed (shim log: $SC closed / $CC connected)"
        return 0
      else
        # ---- THE TIMER MUST NEVER DECIDE ALONE (bug found on the car 2026-10-20).
        # That day SHUTDOWN fired with `frames-stalled 604014ms (hold=6s)` on a
        # stream that was climbing: HOLD_START held a stale value (5), so
        # `now - 5` looked like a ten-minute stall and the controller tore down a
        # healthy session (nav -> black).  The guards below are what make the
        # number trustworthy:
        #   (a) numeric, not zero, not in the future;
        #   (b) NOT older than the moment we entered CARPLAY -- any HOLD_START
        #       predating T_ENTER is left over from another state and is garbage;
        #   (c) the elapsed value is clamped, and an implausible one re-arms the
        #       timer instead of firing.
        # NOTE what is deliberately NOT used: the decoder log's mtime.  v4l2_seq
        # prints a STATUS line every 200 loop iterations even with no frames, so
        # its log is ALWAYS fresh -- using it as evidence would make a real
        # disconnect undetectable.  The frame counter is the only honest signal.
        case "$HOLD_START" in ''|*[!0-9]*) HOLD_START=0 ;; esac
        [ "$HOLD_START" != "0" ] && [ "$HOLD_START" -gt "$_now" ] && HOLD_START=0
        [ "$HOLD_START" != "0" ] && [ "${T_ENTER:-0}" != "0" ] && \
          [ "$HOLD_START" -lt "${T_ENTER:-0}" ] && HOLD_START=0
        if [ "$HOLD_START" = "0" ]; then
          HOLD_START="$_now"; save_state
          tlog "CARPLAY" "hold-start frames=$F (waiting $(hold_ms) ms before declaring the session over)"
        fi
        _held=$(( _now - HOLD_START )); [ "$_held" -lt 0 ] && _held=0
        # PLAUSIBILITY, and why the bound is what it is: a stale HOLD_START from
        # another state shows up as an enormous elapsed time, so we re-arm.  But
        # the bound must sit ABOVE every wait the state machine can legitimately
        # still be inside -- and that is NOT just PAUSE_MAX: the HOLD itself can
        # grow (flaps double it, up to HOLDMAX=6 s) while PAUSE_MAX is
        # operator-tunable.  If the bound sat below the hold, the re-arm would
        # fire at the bound, reset _held to 0, and the stall detector could
        # NEVER fire -- an infinite re-arm loop (found on the desk 2026-10-26:
        # hold=6 s with PAUSE_MAX=0 re-armed every ~5 s forever, the session
        # never ended).  So the bound is: hold + 5 s, and PAUSE_MAX + 5 s,
        # whichever is LARGER.
        _plaus=$(( PAUSE_MAX * 1000 + 5000 ))
        _phold="$(hold_ms)"
        [ -n "$_phold" ] || _phold=1500
        [ "$_plaus" -lt $(( _phold + 5000 )) ] && _plaus=$(( _phold + 5000 ))
        if [ "$_held" -gt "$_plaus" ]; then
          tlog "CARPLAY" "hold-rearmed (implausible elapsed ${_held}ms > ${_plaus}ms) frames=$F"
          HOLD_START="$_now"; save_state; _held=0
        fi
        _hms="$(hold_ms)"
        [ -n "$_hms" ] || _hms=1500
        if [ "$_held" -ge "$_hms" ]; then
          # ---- A PAUSE IS NOT AN END (2026-10-24) --------------------------
          # The phone stops video for ~1.5 s at a scene change (guidance start and
          # stop, measured).  Tearing the session down for that destroyed a live
          # stream, and the re-arm then produced a black frame.  So: keep the
          # session, the RSI and the decoder exactly as they are; the presenter's
          # dead-man blank (8 s) already stops a stale picture from being a lie.
          #
          # NO DECODER RESTART HERE.  The first version restarted the decoder to
          # provoke a keyframe via the shim's helper-connect path; measured on the
          # car it attached only 3 times out of 8, drove 97 connect failures and
          # tripped the shim's EAGAIN bound -- and produced zero frames.  A real
          # keyframe request needs a shim verb, which is a different change.
          if [ "$_held" -ge $(( PAUSE_MAX * 1000 )) ]; then
            do_shutdown "silence ${_held}ms > PAUSE_MAX ${PAUSE_MAX}s (hold=$_hms ms) frames=$F up=$UP"
            return 0
          fi
          if [ "${PAUSE_LOGGED:-0}" = "0" ] || [ $(( _now - ${PAUSE_LOGGED:-0} )) -ge 5000 ]; then
            tlog "CARPLAY" "pause ${_held}ms frames=$F up=$UP -- keeping the session (no teardown, no restart); hands back at ${PAUSE_MAX}s"
            PAUSE_LOGGED="$_now"
          fi
        fi
      fi
      return 0
    fi
    # FALLBACK: the neutral frame is up and the nav hand-back was refused.  Retry
    # it a bounded number of times -- MS-S's 1 Hz sampling can simply have missed
    # our element removal on the first pass -- then stop and say so once.
    # Bounded, and then rare: after FBTRIES we stop hammering MS-S (each attempt
    # costs a TransportStream rebuild) and only try again every FBPERIOD seconds,
    # because MS-S's own TryRestartTask may revive the nav surface on its own.
    _now="$(now_ms)"
    if [ "$ATTEMPTS" -gt "$FBTRIES" ]; then
      if [ "${T_FB:-0}" = "0" ] || [ $(( _now - T_FB )) -ge $(( FBPERIOD * 1000 )) ]; then
        tlog "FALLBACK" "periodic-retry after give-up (cluster shows the NEUTRAL frame, not a stale map)"
        ATTEMPTS=0; T_FB="$_now"; save_state
      else
        return 0
      fi
    fi
    ATTEMPTS=$((ATTEMPTS + 1))
    if [ "$ATTEMPTS" -gt "$FBTRIES" ]; then
      tlog "FALLBACK" "nav-handback gave up after $FBTRIES attempts; the cluster shows the NEUTRAL frame (not a stale map). Next attempt in ${FBPERIOD}s."
      T_FB="$_now"; save_state; return 0
    fi
    if hand_back_to_nav; then
      set_state "NAV" "handback-ok-retry=$ATTEMPTS"
    else
      tlog "FALLBACK" "retry-$ATTEMPTS/$FBTRIES refused; sleeping ${RETRY}s"
      save_state; sleep "$RETRY"
    fi
    ;;
  SHUTDOWN)
    # a previous run died inside SHUTDOWN: finish the job rather than sit here
    tlog "SHUTDOWN" "resuming an interrupted shutdown"
    do_shutdown "resumed"
    ;;
  *) tlog "$STATE" "note=unknown-state (recovering to NAV)"; set_state "NAV" "unknown-state-recovered" ;;
  esac
  return 0
}

# ============================================================================
# verbs
# ============================================================================
acquire() {
  if mkdir "$TLOCK" 2>/dev/null; then echo $$ > "$TLOCK/pid"; trap 'release' EXIT INT TERM; return 0; fi
  _p="$(cat "$TLOCK/pid" 2>/dev/null)"
  # *** PID RECYCLING + ZOMBIES (2026-10-26; deliver_ctl has had this since
  #     2026-10-20, this lock had not).  `kill -0` alone is NOT proof the holder
  #     is alive: pids are recycled, so a dead controller's number can belong to
  #     an unrelated process and the lock then looks held FOREVER (deliver_ctl
  #     measured exactly this: pid 12008 absent from ps, blocking every
  #     switch).  And a ZOMBIE child answers kill -0 with an EMPTY cmdline, so
  #     an empty cmdline is "dead" too.  Only a live pid whose cmdline still
  #     says transition_ctl counts as held.
  #     NO /proc (the desk bench runs on macOS): fall back to the OLD behaviour
  #     -- kill -0 is all there is, so a live pid is treated as held.  On the
  #     unit /proc always exists; the zombie distinction only matters there.
  if [ -n "$_p" ] && kill -0 "$_p" 2>/dev/null; then
    if [ -r "/proc/$_p/cmdline" ]; then
      _cmd="$(tr "\0" " " < /proc/$_p/cmdline 2>/dev/null)"
      case "$_cmd" in
        *transition_ctl*)
          say "ABORT: transition_ctl is already running (pid $_p, cmd: $(printf '%s' "$_cmd" | cut -c1-100))"
          return 1 ;;
        "")
          say "stale lock: pid $_p is a zombie (kill -0 ok, cmdline empty) -- reclaiming" ;;
        *)
          say "stale lock: pid $_p is alive but is NOT a transition_ctl -- reclaiming" ;;
      esac
    else
      say "ABORT: transition_ctl is already running (pid $_p; /proc unreadable -- kill -0 only)"
      return 1
    fi
  fi
  say "stale lock (pid '${_p:-?}' gone) -- reclaiming"
  rm -rf "$TLOCK"; mkdir "$TLOCK" 2>/dev/null && { echo $$ > "$TLOCK/pid"; trap 'release' EXIT INT TERM; return 0; }
  say "ABORT: cannot take the lock $TLOCK"; return 1
}
release() {
  if [ -f "$TLOCK/pid" ] && [ "$(cat "$TLOCK/pid" 2>/dev/null)" = "$$" ]; then rm -rf "$TLOCK" 2>/dev/null; fi
  if [ "${LONG_RUN:-0}" = "1" ]; then
    case "$STATE" in
      CARPLAY) tlog "CARPLAY" "WARN controller-exit-while-CARPLAY (the cluster will freeze when the phone leaves; run: sh $0 to-nav)" ;;
    esac
  fi
}

do_status() {
  load_state
  shim_counts
  printf 'state        : %s (entered %s, %s ms ago)\n' "$STATE" "${T_ENTER:-0}" \
      "$([ "${T_ENTER:-0}" != "0" ] && echo $(( $(now_ms) - T_ENTER )) || echo 0)"
  printf 'session      : %s  (shim log: %s connected / %s closed)\n' \
         "$(session_up && echo UP || echo down)" "$CC" "$SC"
  printf 'frames       : %s (last seen %s)  decoder=%s pid=%s  presenter=%s\n' \
         "$(dec_frames)" "${FRAME_LAST:-?}" "$(dec_alive && echo up || echo absent)" \
         "${DEC_PID:-?}" "$(pres_alive && echo up || echo absent)"
  printf 'hold/attempts: hold_start=%s attempts=%s\n' "${HOLD_START:-0}" "${ATTEMPTS:-0}"
  printf 'oracle       : %s\n' "$([ -s "$RUN/mss.log" ] && echo "capturing ($RUN/mss.log)" || echo 'silent')"
  echo "--- timeline (last 12) ---"
  tail -n 12 "$TLOG" 2>/dev/null
  return 0
}

do_run() {
  _mins="${1:-0}"     # 0 = forever
  LONG_RUN=1
  load_state
  # NAV entry marker: frames must ADVANCE past this to count as a new session,
  # and the connect COUNT must exceed NAV_CC_BASE for the edge trigger.
  if [ "${STATE:-}" = "NAV" ]; then NAV_F_BASE="$(dec_frames)"; shim_counts; NAV_CC_BASE="${CC:-0}"; fi
  trap 'release' EXIT INT TERM
  nap_backend
  nap_probe
  tailc_ok
  tlog "$STATE" "RUN begin mins=$_mins poll=${POLL}s nap=$(nap_mode) hold=$(hold_ms)ms holdmax=${HOLDMAX}s nframes=$NFRAMES nfevent=$NFRAMES_EVENT shimcount=$([ "$TAILC_OK" = "1" ] && echo incremental || echo whole-file)"
  get_state_enter
  _end=0
  [ "$_mins" -gt 0 ] 2>/dev/null && _end=$(( $(now_ms) + _mins * 60000 ))
  _lastsig=""; _lasteval=0
  TICKMS="$(awk -v a="$TICKMAX" 'BEGIN{printf "%d", a*1000+0.5}')"
  [ "${TICKMS:-0}" -gt 0 ] || TICKMS=500
  while : ; do
    _now="$(now_ms)"
    _sig="$(logsig)"
    # EVENT GATE: the logs only grow, so their size is a free "something happened"
    # signal.  A full evaluation runs the moment either log changes, and at least
    # every TICKMAX so the hold/PENDING timers stay honest -- that is what keeps a
    # sub-second tick cheap instead of five greps a second for nothing.
    if [ "$_sig" != "$_lastsig" ] || [ $(( _now - _lasteval )) -ge "$TICKMS" ]; then
      eval_once
      _lastsig="$(logsig)"; _lasteval="$(now_ms)"
    fi
    [ "$_end" != "0" ] && [ "$(now_ms)" -ge "$_end" ] && { tlog "$STATE" "RUN end (minutes up)"; break; }
    nap "$POLL"
  done
  if [ "$EXIT_RESTORE" = "1" ] && [ "$STATE" = "CARPLAY" ]; then do_shutdown "controller-exit"; fi
  return 0
}
get_state_enter() {
  # keep T_ENTER/PEND_START meaningful across invocations
  case "$STATE" in
    PENDING) PEND_START="${PEND_START:-$(now_ms)}" ;;
  esac
}

case "${1:-}" in
  run)      acquire || exit 1; shift; do_run "${1:-0}" ;;
  once)     acquire || exit 1; load_state; eval_once; save_state; do_status ;;
  status)   load_state; do_status ;;
  log)      shift; tail -n "${1:-40}" "$TLOG" 2>/dev/null ;;
  stop)     load_state
            _p="$(cat "$TLOCK/pid" 2>/dev/null)"
            if [ -n "$_p" ] && kill -0 "$_p" 2>/dev/null; then
              tlog "$STATE" "MANUAL stop pid=$_p"
              kill "$_p" 2>/dev/null
              _i=0; while [ "$_i" -lt 5 ] && kill -0 "$_p" 2>/dev/null; do sleep 1; _i=$((_i+1)); done
              kill -9 "$_p" 2>/dev/null
              rm -rf "$TLOCK" 2>/dev/null
              say "controller stopped (pid $_p).  Nothing now watches the stream --"
              say "  the presenter's idle-blank will still stop a stale frame after"
              say "  its threshold, but the cluster will NOT be handed back automatically."
            else
              rm -rf "$TLOCK" 2>/dev/null
              say "controller not running (cleared any stale lock)"
            fi ;;
  to-nav)   acquire || exit 1; load_state
            tlog "$STATE" "MANUAL to-nav"; decoder_down || true; neutral_show || true
            if hand_back_to_nav; then set_state "NAV" "manual"; else set_state "FALLBACK" "manual"; fi
            decoder_up || true     # re-arm for the next session (see do_shutdown S4)
            save_state ;;
  to-neutral) acquire || exit 1; load_state; tlog "$STATE" "MANUAL to-neutral"; neutral_show || true; save_state ;;
  to-carplay) acquire || exit 1; load_state; tlog "$STATE" "MANUAL to-carplay"
            if switch_to_carplay; then set_state "CARPLAY" "manual"; else set_state "PENDING" "manual-failed"; fi
            FRAME_LAST="$(dec_frames)"; save_state ;;
  *) echo "usage: $0 {run [minutes]|once|status|log [n]|stop|to-nav|to-neutral|to-carplay}"; exit 2 ;;
esac
