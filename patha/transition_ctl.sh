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
POLL="${MIB3_TR_POLL:-1}"              # seconds per tick
HOLD="${MIB3_TR_HOLD:-6}"              # a frame stall shorter than this is a blip, not an end
PENDMAX="${MIB3_TR_PENDMAX:-45}"       # the keyframe gate is ~3 s worst case; 45 s is "not delivering"
NFRAMES="${MIB3_TR_NFRAMES:-3}"        # frames needed to call the decode path live
MAXATT="${MIB3_TR_MAXATT:-3}"          # switch attempts before we stop trying
RETRY="${MIB3_TR_RETRY:-5}"            # seconds between switch attempts
RESTORE_T="${MIB3_TR_RESTORE_T:-10}"   # oracle gate for the nav hand-back (MS-S samples 1 Hz)
FEED_T="${MIB3_TR_FEED_T:-12}"         # wall guard on the neutral feeder
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
now_ms() {
  if [ -r /proc/uptime ]; then
    _u="$(cut -d' ' -f1 /proc/uptime 2>/dev/null)"
    case "$_u" in
      *.*) _s="${_u%%.*}"; _f="${_u#*.}"; _f="${_f}00"; _f="${_f%"${_f#??}"}"
           printf '%s' "$(( _s * 1000 + _f ))" ;;
      *)   printf '%s' "$(( ${_u:-0} * 1000 ))" ;;
    esac
  else
    printf '%s' "$(( $(date +%s) * 1000 ))"
  fi
}
utc() { date -u +%H:%M:%S 2>/dev/null || echo "--:--:--"; }

SEQ=0
# NB: the counter must be bumped in THIS shell.  `$(nextseq)` would run in a
# command substitution's subshell, where the increment is lost -- every line then
# carries seq=1 (measured; it made the sequence column useless for correlation).
seq_inc() { SEQ=$((SEQ + 1)); }

# log <STATE> <event> [k=v ...]  -- one line: monotonic ms, UTC, global seq
tlog() {
  _st="$1"; shift
  seq_inc
  printf '%s %s %s %s %s\n' "$(now_ms)" "$(utc)" "$SEQ" "$_st" "$*" >> "$TLOG" 2>/dev/null
  sync 2>/dev/null
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
# process substitution, no arrays)
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
shim_counts() { CC="$(count_in "$SHIMLOG" 'cluster/thread: stream connected')"
                SC="$(count_in "$SHIMLOG" 'cluster/thread: stream closed')"
                case "$CC" in ''|*[!0-9]*) CC=0 ;; esac
                case "$SC" in ''|*[!0-9]*) SC=0 ;; esac; }
session_up()  { [ "$CC" -gt "$SC" ]; }
# a shrink means the shim was re-injected (a fresh log): treat as a new world
shim_rotated() { [ "${CC:-0}" -lt "${LAST_CC:-0}" ] || [ "${SC:-0}" -lt "${LAST_SC:-0}" ]; }

# the decoder's REAL frame counter.  NOTE THE TRAP: `FRAME n luma=` lines are
# printed only for #1-8 and every 50th, so counting log lines is wrong.  The
# STATUS line's `frames=` is the counter.
dec_frames() {
  [ -f "$DECLOG" ] || { echo 0; return; }
  _v="$(grep -ao 'frames=[0-9][0-9]*' "$DECLOG" 2>/dev/null | tail -n 1 | sed 's/frames=//')"
  [ -n "$_v" ] || _v=0
  echo "$_v"
}
dec_alive()  { [ -n "$(pids_of "${MIB3_TR_DECNAME:-v4l2_seq}")" ]; }
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
  XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/platform/weston}" \
  WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
    nohup "$EGL" "$IVIID" "$W" "$H" 3600 00ff00 --persist --fps 30 \
      --rgba-sock "$RGBASOCK" >> "$EGLLOG" 2>&1 &
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
  if [ -z "$_p" ]; then tlog "$STATE" actuator decoder=already-absent; return 0; fi
  bc "decoder:stop $_p"
  kill $_p 2>/dev/null
  _i=0; while [ "$_i" -lt 6 ]; do alive "$_p" || break; sleep 1; _i=$((_i+1)); done
  alive "$_p" && kill -9 $_p 2>/dev/null
  tlog "$STATE" actuator decoder=stopped pid="$_p" wait="${_i}s"
  return 0
}
decoder_up() {
  decoder_down >/dev/null 2>&1
  [ -x "$DEC" ] || { tlog "$STATE" actuator decoder=MISSING path="$DEC"; return 1; }
  # the AU socket is OURS (the decoder listens on it).  NEVER remove the
  # presenter's rgba socket here: the presenter owns that path, and unlinking it
  # would leave a live presenter whose socket nobody can reach.
  rm -f "$AUSOCK" 2>/dev/null
  bc "decoder:start"
  V4L2_FDFIELD=3 V4L2_IN_SOCK="$AUSOCK" V4L2_RGBA_SOCK="$RGBASOCK" V4L2_OUTN=8 \
    nohup "$DEC" online 0 >> "$DECLOG" 2>&1 &
  _i=0; while [ "$_i" -lt 8 ]; do [ -S "$AUSOCK" ] && break; sleep 1; _i=$((_i+1)); done
  smack_grants
  tlog "$STATE" actuator decoder=started pid="$(pids_of "${MIB3_TR_DECNAME:-v4l2_seq}")" sock=$([ -S "$AUSOCK" ] && echo up || echo MISSING) wait="${_i}s"
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
  _i=0
  while [ "$_i" -lt 4 ]; do
    "$RGBAFEED" "$RGBASOCK" "$NEUTRAL" "$W" "$H" --count "$FEEDCOUNT" --fps 10 \
        > "$RUN/neutral.log" 2>&1 &
    _fp=$!
    _g=0; while [ "$_g" -lt "$FEED_T" ]; do alive "$_fp" || break; sleep 1; _g=$((_g+1)); done
    alive "$_fp" && { kill -9 "$_fp" 2>/dev/null; tlog "$STATE" neutral=TIMEOUT wait="${_g}s"; }
    if grep -qa 'frames sent' "$RUN/neutral.log" 2>/dev/null; then
      tlog "$STATE" neutral=OK frames="$(grep -ao 'frames sent[= ]*[0-9]*' "$RUN/neutral.log" | tail -n1 | sed 's/frames sent[= ]*//')"
      return 0
    fi
    _i=$((_i+1)); sleep 1
  done
  tlog "$STATE" neutral=FAIL presenter-not-reading sock="$RGBASOCK"
  return 1
}

# ============================================================================
# the switch (both directions) -- always deliver_ctl.sh's oracle-gated verbs
# ============================================================================
switch_to_carplay() {
  [ -f "$CTL" ] || { tlog "$STATE" switch=FAIL no-ctl path="$CTL"; return 1; }
  presenter_up || true
  bc "switch:to-carplay"
  sh "$CTL" handover full >> "$RUN/delivery.log" 2>&1
  _rc=$?
  tlog "$STATE" switch=to-carplay rc="$_rc"
  return $_rc
}
hand_back_to_nav() {
  [ -f "$CTL" ] || { tlog "$STATE" back=FAIL no-ctl; return 1; }
  bc "switch:to-nav"
  sh "$CTL" handover back "$RESTORE_T" >> "$RUN/delivery.log" 2>&1
  _rc=$?
  tlog "$STATE" back=to-nav rc="$_rc"
  return $_rc
}

# ============================================================================
# state
# ============================================================================
STATE="NAV"; HOLD_START=0; FRAME_LAST=0; ATTEMPTS=0; T_ENTER=0; T_FB=0
STATE_LOADED=0
load_state() {
  [ -f "$TSTATE" ] || return 0
  # shellcheck disable=SC2046
  set -- $(cat "$TSTATE" 2>/dev/null)
  [ $# -ge 6 ] || return 0
  STATE="$1"; HOLD_START="$2"; FRAME_LAST="$3"; ATTEMPTS="$4"; T_ENTER="$5"; T_FB="$6"
  [ $# -ge 7 ] && SEQ="$7"
  case "$SEQ" in ''|*[!0-9]*) SEQ=0 ;; esac
  STATE_LOADED=1
  return 0
}
save_state() { printf '%s %s %s %s %s %s %s\n' "$STATE" "${HOLD_START:-0}" "${FRAME_LAST:-0}" \
                 "${ATTEMPTS:-0}" "${T_ENTER:-0}" "${T_FB:-0}" "${SEQ:-0}" > "$TSTATE" 2>/dev/null; }

# set_state <NEW> <why>  -- logs the transition with its duration in ms
set_state() {
  _new="$1"; _why="$2"; _now="$(now_ms)"
  if [ "$_new" = "$STATE" ]; then return 0; fi
  _dwell=0
  [ "${T_ENTER:-0}" != "0" ] && _dwell=$(( _now - T_ENTER ))
  tlog "$_new" "ENTER why=$_why from=$STATE dwell=${_dwell}ms"
  STATE="$_new"; T_ENTER="$_now"; HOLD_START=0
  [ "$_new" = "PENDING" ] && ATTEMPTS=0
  save_state
}

# ============================================================================
# the SHUTDOWN work (bounded; both verdicts logged)
# ============================================================================
do_shutdown() {
  _why="$1"
  t0="$(now_ms)"
  # enter SHUTDOWN as a real state FIRST, so every actuator line below is labelled
  # with the phase it belongs to (not with the phase we are leaving)
  set_state "SHUTDOWN" "$_why"
  tlog "SHUTDOWN" "BEGIN why=$_why (decoder -> neutral -> nav)"
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
  if hand_back_to_nav; then
    t_nav="$(( $(now_ms) - t0 ))"
    tlog "SHUTDOWN" "END verdict=NAV t_total=${t_nav}ms"
    set_state "NAV" "handback-ok"
  else
    t_fb="$(( $(now_ms) - t0 ))"
    tlog "SHUTDOWN" "END verdict=FALLBACK t_total=${t_fb}ms reason=nav-handback-refused"
    T_FB="$t_fb"
    set_state "FALLBACK" "handback-failed"
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
    # a phone session that is feeding us begins a PENDING window; the car's map
    # stays up for the whole of it (we touch nothing here -- that is the point)
    # A session starts PENDING only on evidence that a session is STARTING: the
    # shim's own `stream connected` line, or frames arriving while a decoder is
    # alive.  "frames > 0 in the decoder log" alone is NOT evidence -- after a
    # SHUTDOWN the log still holds the last count, and trusting it would restart
    # CarPlay delivery with no phone attached.
    _go=0
    [ "$UP" = "1" ] && _go=1
    if [ "$F" -gt 0 ] && dec_alive; then _go=1; fi
    if [ "$_go" = "1" ]; then
      # A NEW decoder process per session is an invariant: the vendor's capture
      # port renegotiate is safe only on a decoder's FIRST connection, and a
      # second one mid-stream wedges the output queue.  So if the phone starts a
      # session and nothing is decoding, this controller starts a fresh decoder
      # (and never reuses one across sessions -- see SHUTDOWN/decoder_down).
      if ! dec_alive; then decoder_up || true; fi
      set_state "PENDING" "session-up=$UP frames=$F dec=$(dec_alive && echo up || echo absent)"
      FRAME_LAST="$F"; save_state
    fi
    ;;
  PENDING)
    if [ "$F" -ge "$NFRAMES" ]; then
      tlog "PENDING" "frames=$F >= $NFRAMES -> switching"
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
      if [ "$F" -gt "$FRAME_LAST" ]; then
        if [ "$HOLD_START" != "0" ]; then
          tlog "CARPLAY" "frames-resumed after $(( $(now_ms) - HOLD_START ))ms"
        fi
        HOLD_START=0; FRAME_LAST="$F"; save_state
      elif [ "$UP" = "0" ]; then
        do_shutdown "stream-closed (shim log: $SC closed / $CC connected)"
        return 0
      else
        if [ "$HOLD_START" = "0" ]; then
          HOLD_START="$(now_ms)"; save_state
          tlog "CARPLAY" "hold-start frames=$F (waiting ${HOLD}s before declaring the session over)"
        fi
        _held=$(( $(now_ms) - HOLD_START ))
        if [ "$_held" -ge $(( HOLD * 1000 )) ]; then
          do_shutdown "frames-stalled ${_held}ms (hold=${HOLD}s) frames=$F up=$UP"
          return 0
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
  *) set_state "NAV" "unknown-state-recovered($_STATE=$STATE)" ;;
  esac
  return 0
}

# ============================================================================
# verbs
# ============================================================================
acquire() {
  if mkdir "$TLOCK" 2>/dev/null; then echo $$ > "$TLOCK/pid"; trap 'release' EXIT INT TERM; return 0; fi
  _p="$(cat "$TLOCK/pid" 2>/dev/null)"
  if [ -n "$_p" ] && kill -0 "$_p" 2>/dev/null; then
    say "ABORT: transition_ctl is already running (pid $_p)"; return 1
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
  printf 'frames       : %s (last seen %s)  decoder=%s presenter=%s\n' \
         "$(dec_frames)" "${FRAME_LAST:-?}" "$(dec_alive && echo up || echo absent)" "$(pres_alive && echo up || echo absent)"
  printf 'hold/attempts: hold_start=%s attempts=%s\n' "${HOLD_START:-0}" "${ATTEMPTS:-0}"
  printf 'oracle       : %s\n' "$([ -s "$RUN/mss.log" ] && echo "capturing ($RUN/mss.log)" || echo 'silent')"
  echo "--- timeline (last 12) ---"
  tail -n 12 "$TLOG" 2>/dev/null
}

do_run() {
  _mins="${1:-0}"     # 0 = forever
  LONG_RUN=1
  load_state
  trap 'release' EXIT INT TERM
  tlog "$STATE" "RUN begin mins=$_mins poll=${POLL}s hold=${HOLD}s nframes=$NFRAMES"
  get_state_enter
  _end=0
  [ "$_mins" -gt 0 ] 2>/dev/null && _end=$(( $(now_ms) + _mins * 60000 ))
  while : ; do
    eval_once
    [ "$_end" != "0" ] && [ "$(now_ms)" -ge "$_end" ] && { tlog "$STATE" "RUN end (minutes up)"; break; }
    sleep "$POLL"
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
  to-nav)   acquire || exit 1; load_state
            tlog "$STATE" "MANUAL to-nav"; decoder_down || true; neutral_show || true
            if hand_back_to_nav; then set_state "NAV" "manual"; else set_state "FALLBACK" "manual"; fi
            save_state ;;
  to-neutral) acquire || exit 1; load_state; tlog "$STATE" "MANUAL to-neutral"; neutral_show || true; save_state ;;
  to-carplay) acquire || exit 1; load_state; tlog "$STATE" "MANUAL to-carplay"
            if switch_to_carplay; then set_state "CARPLAY" "manual"; else set_state "PENDING" "manual-failed"; fi
            FRAME_LAST="$(dec_frames)"; save_state ;;
  *) echo "usage: $0 {run [minutes]|once|status|log [n]|to-nav|to-neutral|to-carplay}"; exit 2 ;;
esac
