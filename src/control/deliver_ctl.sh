#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# ============================================================================
# deliver_ctl.sh -- the RSI handover under pathb_daemon.sh: park the car's nav
#                   stream so our sender can take the cluster's port, and hand
#                   it back.  Runs ON THE UNIT only.
#
#   *** LIVE-ONLY. RAM-only. It NEVER touches a service (carplay / multi-display /
#       rsi-display-management / displaymgr); it only POSTs/DELETEs RSI elements
#       and starts/stops OUR OWN processes.  A reboot is the full undo. ***
#
# MECHANISM (do not re-derive):
#   multi-screen-server (MS-S) streams whatever ivi surface id is written on a
#   `displaymanagement.renderingSurfaces` element's `internalIdentifier`, and that
#   id is assigned at CREATE from a table COMPILED INTO rsi-display-management:
#       navi -> 19   test -> 3000   navi-kombi-renderingsurface -> 33
#   Deleting the navi-kombi element stops MS-S's cluster stream and frees its
#   (ip,port); re-creating it, or a resync, starts the car's map again.
#   THREATS HANDLED: rsi-display-management holds elements in RAM (a restart wipes
#   ours -- we re-check, never assume); a FAILED stream start is NEVER retried, so
#   the only retry is `{"resync":true}`; the ECU re-acquires on a stream restart.
#
# USAGE (on the unit; pathb_daemon.sh drives park/back and oracle start/stop):
#   sh deliver_ctl.sh status            # read-only: element ids, MS-S fds, last decision
#   sh deliver_ctl.sh handover park     # delete the nav element, gated on STOP STREAMING
#   sh deliver_ctl.sh handover back     # put the car's nav back, gated on its encoder
#   sh deliver_ctl.sh oracle start      # start the MS-S live-log capture (the ORACLE)
#   sh deliver_ctl.sh oracle stop|tail  # stop it / show the last decisions
#
# SAFETY: on every mutating step a one-line breadcrumb (write + sync) lands on the
#   STICK at <stick>/PART1/deliver.bc -- a fault here can reset the unit and wipe
#   /dev/shm, so the stick is the only surviving record.  (We do NOT touch the
#   shim's own mib3shim.bc -- a root-created file carries the wrong SMACK label:
#   THE LABEL TRAP.)
# ============================================================================
set -u
# All of these are overridable (with MIB3_DLV_STICK for the state dir) so the
# controller can run against a fake RSI and MS-S log on the desk.  On the unit the
# defaults are the live values.
RSI="${MIB3_DLV_RSI:-127.0.0.1}"
DMP="${MIB3_DLV_DMP:-49600}"      # displaymanagement
LOGPORT="${MIB3_DLV_LOGPORT:-54321}"  # MS-S live log (the oracle)
SPNAME=navi-kombi-streamparameters
NAVNAME=navi-kombi-renderingsurface
OURNAME=test      # the retired Path A element; back removes a leftover one
NAVID=33          # the car nav engine's own ivi surface (name navi-kombi-renderingsurface)

stick_path() {
  [ -n "${MIB3_DLV_STICK:-}" ] && { printf '%s' "$MIB3_DLV_STICK"; return 0; }
  for d in /tmp/USB/PORT1 /tmp/USB/PORT2 /tmp/USB/PORT3 /tmp/USB/PORT4; do
    [ -d "$d/PART1" ] && { printf '%s/PART1' "$d"; return 0; }
  done
  return 1
}
if STICK="$(stick_path)"; then
  mkdir -p "$STICK" 2>/dev/null
  BCF="$STICK/deliver.bc"; MSSLOG="$STICK/mss.log"; LOCK="$STICK/deliver.lock"; OPID="$STICK/oracle.pid"
else
  BCF="${MIB3_DLV_RAM:-/dev/shm}/deliver.bc"; MSSLOG="${MIB3_DLV_RAM:-/dev/shm}/mss.log"; LOCK="${MIB3_DLV_RAM:-/dev/shm}/deliver.lock"; OPID="${MIB3_DLV_RAM:-/dev/shm}/oracle.pid"
fi
# the oracle's own liveness record (size + timestamp of the previous invocation);
# see oracle_ensure.  Kept next to the pid so a rotation/restart resets both.
OSTATE="$OPID.state"

bc() { printf '[%s] %s\n' "$(date -u +%Y%m%dT%H%M%SZ)" "$*" >> "$BCF" 2>/dev/null; sync 2>/dev/null; \
       printf '[bc] %s\n' "$*"; }
say() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }

# ---------------------------------------------------------------------------
# TIME, and the SUB-SECOND WAIT.
#
# The gates used to poll once a second, so a decision MS-S had already taken was
# noticed up to 1000 ms late.  MS-S samples at ~1 Hz but its LOG is a stream, so
# the waits poll at OPOLL (100 ms) and return the moment the line appears, each
# one reporting how long it actually took.  (MIB3_DLV_OPOLL=1 restores the old
# cadence.)
#
# *** BASE 10, ALWAYS. ***  /proc/uptime prints e.g. `2289.08`; truncating the
# fraction and adding it with $(( )) makes `08`/`09` invalid OCTAL, so the whole
# arithmetic aborts ("value too great for base") and now_ms returned the EMPTY
# STRING for ~20 % of seconds.  `10#` is a bashism busybox ash lacks, so the
# portable cure is to STRIP the leading zeros -- the value is then a plain decimal
# literal in ash, dash and bash.  Three strips suffice because the fraction is
# truncated to three digits first.  MIB3_DLV_UPTIME exists so the desk bench can
# assert the 08/09 cases.
now_ms() {
  _uf="${MIB3_DLV_UPTIME:-/proc/uptime}"
  if [ -r "$_uf" ]; then
    _u="$(cut -d' ' -f1 "$_uf" 2>/dev/null)"
    _s="${_u%%.*}"; case "$_s" in ''|*[!0-9]*) _s=0 ;; esac
    case "$_u" in *.*) _f="${_u#*.}" ;; *) _f="" ;; esac
    _f="${_f}000"; _f="${_f%"${_f#???}"}"      # exactly the first three digits
    _f="${_f#0}"; _f="${_f#0}"; _f="${_f#0}"   # 08/09 -> 8/9, 000 -> empty
    printf '%s' "$(( _s * 1000 + ${_f:-0} ))"
    return 0
  fi
  _n="$(date +%s%N 2>/dev/null)"
  case "$_n" in
    ''|*[!0-9]*) printf '%s' "$(( $(date +%s) * 1000 ))" ;;
    *) if [ ${#_n} -ge 13 ]; then printf '%s' "$(( _n / 1000000 ))"; else printf '%s' "$(( _n * 1000 ))"; fi ;;
  esac
}
# nap <seconds>, fractional when the platform's sleep supports it (the unit's is
# busybox; the probe costs at most a second, once, and its verdict is printed).
NAP_F=""
nap_probe() {
  [ -n "$NAP_F" ] && return 0
  # 4 x 0.25 s = 1.0 s nominal, judged in MILLISECONDS: comparing whole seconds
  # made the verdict a coin flip (a 0.5 s probe only crosses a second boundary half
  # the time), and a wrong verdict costs exactly the second this exists to save.
  _a="$(now_ms)"; _i=0
  while [ "$_i" -lt 4 ]; do sleep 0.25 2>/dev/null; _i=$((_i+1)); done
  _b="$(now_ms)"
  if [ $(( _b - _a )) -ge 900 ]; then NAP_F=f; else NAP_F=i; fi
  say "wait cadence: sleep is $([ "$NAP_F" = f ] && echo 'fractional (poll 100 ms)' || echo 'INTEGER-ONLY (poll 1 s)')"
  return 0
}
nap() {
  _sec="${1:-0}"
  nap_probe
  if [ "$NAP_F" = "f" ]; then sleep "$_sec" 2>/dev/null; return 0; fi
  case "$_sec" in
    ''|0|0.0|0.00*) sleep 1 ;;
    0.*)       # busybox `sleep` is INTEGER-ONLY on this unit, so `nap 0.1` used to
               # burn a full second -- every gate poll, every retry.
               # busybox HAS a usleep applet and it is accurate (measured 110 ms for
               # 100 ms).  That is the difference between a 1 s and a 100 ms poll.
               _us="$(awk -v s="$_sec" 'BEGIN{printf "%d", s*1000000}' 2>/dev/null)"
               if [ -n "$_us" ]; then busybox usleep "$_us" 2>/dev/null || sleep 1
               else sleep 1; fi ;;
    *)         sleep "$_sec" ;;
  esac
}
OPOLL="${MIB3_DLV_OPOLL:-0.1}"        # oracle poll interval (s)
# wait until the wall clock is in a NEW second: the streamParameters resync verb
# is timestamp-latched at 1 s resolution, so a flip inside one second can be a
# silent no-op (session 6 hit exactly that).  Waiting for the second to turn is
# what makes the 0->1 transition a real one -- and it costs at most 1 s.
nap_next_second() {
  _s="$(date +%s)"
  _i=0
  while [ "$(date +%s)" = "$_s" ] && [ "$_i" -lt 40 ]; do nap 0.05; _i=$((_i+1)); done
}
# per-step timing.  Each step measures from the END of the previous one (tmark),
# so the printed parts ADD UP to the total -- chasing a stopwatch across three
# shared variables got this wrong once already, and a breakdown that does not sum
# is worse than no breakdown.
TIMING=""; T_MARK=0; T_SUM=0; T_LAST=0
tmark() { T_MARK="$(now_ms)"; }
tstep() {
  _now2="$(now_ms)"
  _d=$(( _now2 - ${T_MARK:-_now2} )); [ "$_d" -lt 0 ] && _d=0
  TIMING="$TIMING $1=${_d}ms"; T_SUM=$(( T_SUM + _d )); T_LAST="$_d"
  say "  timing: $1=${_d}ms"
  T_MARK="$_now2"
}

# ---------------------------------------------------------------------------
# PHASE 0 SUB-TIMERS -- PASSIVE: they only add log fields.
#
# `tstep mutations` reported ~3 s in the field for a DELETE and a POST that each
# cost 20-30 ms.  That span also carried a `wc -c` over a
# file reaching hundreds of MB, TWO breadcrumbs that each do a `sync`, three list
# GETs and, in `back`, a 2 MB `tail | grep`; so each is timed on its own and the
# printed parts SUM to the step:
#     del get post sync mark grep lists   (+ other = precisely the remainder)
# Nothing here reads a value, changes an order, a budget or a wire call: every timer
# is a pair of now_ms() calls around a call that was already there.  `other` is
# COMPUTED, never estimated, so the sum stays exact when a future edit adds an
# untimed statement -- a previous version exceeded its own total unnoticed.
# COST: two now_ms() per timed call; now_ms is a fork on the unit (~1-3 ms at load
# 50), so this adds a few tens of ms to a 12 s switch.  Stated because it is the one
# way Phase 0 is not free.
# ---------------------------------------------------------------------------
SUBL=0; SUBM=0; SUBS=0; SUBD=0; SUBG=0; SUBP=0; SUBR=0; SUB_T0=0
sub_reset() { SUBL=0; SUBM=0; SUBS=0; SUBD=0; SUBG=0; SUBP=0; SUBR=0; SUB_T0=0; }
sub0() { SUB_T0="$(now_ms)"; }
sub_add() {   # sub_add <lists|mark|sync|del|get|post|grep>
  _sd=$(( $(now_ms) - ${SUB_T0:-0} )); [ "$_sd" -lt 0 ] && _sd=0
  case "$1" in
    lists) SUBL=$(( SUBL + _sd )) ;;
    mark)  SUBM=$(( SUBM + _sd )) ;;
    sync)  SUBS=$(( SUBS + _sd )) ;;
    del)   SUBD=$(( SUBD + _sd )) ;;
    get)   SUBG=$(( SUBG + _sd )) ;;
    post)  SUBP=$(( SUBP + _sd )) ;;
    grep)  SUBR=$(( SUBR + _sd )) ;;
  esac
}
sub_str() {   # sub_str <step-total-ms> -> " mut_del=..ms ... mut_other=..ms"
  _st=$(( SUBL + SUBM + SUBS + SUBD + SUBG + SUBP + SUBR ))
  _so=$(( ${1:-0} - _st )); [ "$_so" -lt 0 ] && _so=0
  printf ' mut_del=%sms mut_get=%sms mut_post=%sms mut_sync=%sms mut_mark=%sms mut_grep=%sms mut_lists=%sms mut_other=%sms' \
         "$SUBD" "$SUBG" "$SUBP" "$SUBS" "$SUBM" "$SUBR" "$SUBL" "$_so"
}
# tline <stage> <total-ms> [suffix] -- ONE printer for the breakdown, so every
# exit path prints the same field set (and a field session greps ONE shape):
#   stage=.. total=.. sum=.. <steps..> mut_*=.. flush=.. reqs=METHOD:ms/rc,..
MUTSUB=""; EXTRA=""
tline() {
  _r="${REQ%,}"
  say "HANDOVER TIMING stage=$1 total=$2ms sum=${T_SUM}ms$TIMING$MUTSUB$EXTRA${_r:+ reqs=$_r}${3:-}"
}
# the same per-request record for the verbs that print no breakdown
rsi_report() { [ -n "${REQ:-}" ] && say "  rsi: ${REQ%,}"; return 0; }


# --- lockfile (busybox has no flock; mkdir is atomic) --------------------------
acquire() {
  if mkdir "$LOCK" 2>/dev/null; then echo "$$" > "$LOCK/pid"; trap 'release' EXIT; trap 'exit 130' INT; trap 'exit 143' TERM; return 0; fi
  _p="$(cat "$LOCK/pid" 2>/dev/null)"
  # A pid is NOT proof of a live deliver_ctl: pids are recycled, so a dead holder's
  # number can belong to an unrelated process and the lock then looks held FOREVER
  # (measured 2026-10-20: pid 12008, absent from `ps`, blocked every switch and froze
  # the cluster on the previous surface).  Check the cmdline too.  A ZOMBIE answers
  # kill -0 with an EMPTY cmdline (2026-10-26) and is a dead holder as well.  With NO
  # /proc (the desk bench) kill -0 is all there is: a live pid counts as held.
  if [ -n "$_p" ] && kill -0 "$_p" 2>/dev/null; then
    if [ -r "/proc/$_p/cmdline" ]; then
      _cmd="$(tr "\0" " " < /proc/$_p/cmdline 2>/dev/null)"
      case "$_cmd" in
        *deliver_ctl*)
          say "ABORT: another deliver_ctl is running (pid $_p, cmd: $(printf '%s' "$_cmd" | cut -c1-100)). Refusing to double-apply."
          return 1 ;;
        "")
          say "stale lock: pid $_p is a zombie (alive to kill -0, cmdline empty) -- reclaiming" ;;
        *)
          say "stale lock: pid $_p is alive but is NOT a deliver_ctl -- reclaiming" ;;
      esac
    else
      say "ABORT: another deliver_ctl is running (pid $_p; /proc unreadable -- kill -0 only). Refusing to double-apply."
      return 1
    fi
  fi
  say "stale lock (pid '${_p:-?}' gone) -- reclaiming"
  rm -rf "$LOCK"; mkdir "$LOCK" 2>/dev/null && { echo "$$" > "$LOCK/pid"; trap 'release' EXIT; trap 'exit 130' INT; trap 'exit 143' TERM; return 0; }
  say "ABORT: cannot take the lock"; return 1
}
release() { [ -n "${LOCK:-}" ] && [ -f "$LOCK/pid" ] && [ "$(cat "$LOCK/pid" 2>/dev/null)" = "$$" ] && rm -rf "$LOCK" 2>/dev/null; }

# --- oracle lookups are bounded ------------------------------------------------
# The oracle log grows ~8 MB/min and is never rotated, so a FULL grep costs seconds
# (`status` measured 17.3 s).  Every lookup here only wants RECENT history:
# MIB3_DLV_ORACLE_TAIL bytes (default 2 MB) is minutes of it.
otail() { tail -c "${MIB3_DLV_ORACLE_TAIL:-2000000}" "$MSSLOG" 2>/dev/null; }

# --- tiny RSI helpers ----------------------------------------------------------
have() { command -v "$1" >/dev/null 2>&1; }
pids_of() { if have pidof; then pidof "$1" 2>/dev/null; elif have pgrep; then pgrep -x "$1" 2>/dev/null; fi; }
# h <method> <url> [body] -- ONE RSI request.
# It records how long the request took and what curl returned.  Every request
# carries `--max-time 8`, so an expiry (curl rc 28) was indistinguishable from a
# success and H3 ("the 11 s run was one curl expiry") was unanswerable without the
# status; the cost and rc are reported as `reqs=` on the HANDOVER TIMING line.
# Nothing else changes: same timeout, one attempt, no retries, body to stdout, and
# the function still returns curl's own status.
H_RC=0; H_MS=0; REQ=""
h() { _hm="$1"; _hu="$2"; _hb="${3:-}"
  _h0="$(now_ms)"
  if [ -n "$_hb" ]; then curl -sS --max-time 8 -X "$_hm" -H 'Content-Type: application/json' --data "$_hb" "$_hu"
  else curl -sS --max-time 8 -X "$_hm" "$_hu"; fi
  H_RC=$?
  H_MS=$(( $(now_ms) - _h0 )); [ "$H_MS" -lt 0 ] && H_MS=0
  REQ="$REQ$_hm:${H_MS}ms/rc${H_RC},"
  return $H_RC; }
de() { tr -d '\\'; }
line_of() { printf '%s' "$1" | sed 's/},{/}\
{/g' | grep "\"name\":\"$2\"" | head -n1; }
uri_of()  { line_of "$1" "$2" | sed -n 's/.*"uri":"\([^"]*\)".*/\1/p' | de | head -n1; }
id_of()   { line_of "$1" "$2" | sed -n 's/.*"internalIdentifier":"\([^"]*\)".*/\1/p' | head -n1; }
# -f: an HTTP error must fail the read, or an empty list reads as "element absent"
uris() { curl -fsS --max-time 8 "http://$RSI:$DMP/displaymanagement/renderingSurfaces"; }
disps(){ curl -fsS --max-time 8 "http://$RSI:$DMP/displaymanagement/displays"; }
sps()  { curl -fsS --max-time 8 "http://$RSI:$DMP/displaymanagement/streamParameters"; }
valid_uri() { case "$1" in /displaymanagement/renderingSurfaces/*|/displaymanagement/streamParameters/*|/displaymanagement/displays/*) return 0;; *) return 1;; esac; }

# The oracle is MS-S's own live log (port 54321) -- the ONLY way to read the share
# decision without eyes on the dash. Started by `oracle start` (the supervisor's
# prepare step) and re-checked by every handover.  IDEMPOTENT and APPEND-only.
#
# HYGIENE: the capture is ONE `curl -sN --max-time 3600` writing to tmpfs at
# ~8 MB/min, and it is the eyes of every gate.  Two things that are NOT proof used
# to be trusted: "the file is non-empty" and "the pid is alive".
#   * curl dies (MS-S restart, socket reset, the 3600 s cap, OOM) -> the file stays
#     non-empty and every later gate waits out its budget, reporting a switch
#     failure that never happened;
#   * the pid is RECYCLED -> `oracle_off` kills an innocent process while the real
#     capture keeps growing the file.
# So: the pid must match OUR curl's cmdline; liveness is GROWTH (measured against the
# size/time the previous invocation recorded, so the check is free on the hot path);
# and the file rotates at a size cap so an all-day drive cannot fill tmpfs.
ORACLE_KEEP="${MIB3_DLV_ORACLE_KEEP:-4194304}"     # bytes kept when rotating (minutes of log)
ORACLE_MAX="${MIB3_DLV_ORACLE_MAX:-67108864}"      # rotate above this (64 MB ~ 8 min at 8 MB/min)
ORACLE_STALE_MS="${MIB3_DLV_ORACLE_STALE_MS:-8000}" # no growth for this long = dead capture

cmdline_of() {   # /proc on the unit, ps on the desk (macOS has no /proc)
  if [ -r "/proc/$1/cmdline" ]; then tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null
  else ps -o command= -p "$1" 2>/dev/null; fi
}
oracle_pid() {
  [ -f "$OPID" ] || return 1
  _p="$(cat "$OPID" 2>/dev/null)"
  case "$_p" in ''|*[!0-9]*) return 1 ;; esac
  kill -0 "$_p" 2>/dev/null || return 1
  # alive is not enough -- it must be OUR curl, on OUR log port
  case "$(cmdline_of "$_p")" in
    *curl*"$RSI:$LOGPORT"*) printf '%s' "$_p"; return 0 ;;
  esac
  return 1
}
# rotate at the cap.  ATOMIC: the kept tail is written to a sibling and renamed over
# the log, so a reader (oracle_since / otail) sees the old file or the new one, never
# a half-written one.  It runs ONLY from oracle_on, i.e. before the invocation has
# taken a byte mark, and only one deliver_ctl runs at a time (the lock), so no gate
# can be mid-flight.  A mark taken BEFORE a rotation is larger than the new size, and
# oracle_since treats that as "rotated: start over" (reads from 0) -- correct, since
# the retained tail is new history, not a stale hit.
oracle_rotate() {
  _sz="$(oracle_mark)"
  [ "${_sz:-0}" -ge "$ORACLE_MAX" ] || return 0
  say "oracle: ${_sz} B is over the cap -- rotating (keep the last ${ORACLE_KEEP} B)"
  tail -c "$ORACLE_KEEP" "$MSSLOG" > "$MSSLOG.rot" 2>/dev/null || { rm -f "$MSSLOG.rot"; return 0; }
  mv -f "$MSSLOG.rot" "$MSSLOG" 2>/dev/null || { rm -f "$MSSLOG.rot"; return 0; }
  say "oracle: rotated, now $(oracle_mark) B"
  return 0
}
oracle_start() {
  mkdir -p "$(dirname "$MSSLOG")" 2>/dev/null
  _s0="$(oracle_mark)"
  # curl writes the file itself, so $! is the curl pid (killable) and the harness'
  # pkill pattern ("curl -sN --max-time 3600 http://127.0.0.1:$LOGPORT") still matches.
  nohup curl -sN --max-time 3600 "http://$RSI:$LOGPORT/" >> "$MSSLOG" 2>/dev/null &
  echo $! > "$OPID" 2>/dev/null
  # readiness is an EVENT (a line ARRIVING), not a fixed 2 s sleep -- and it is
  # growth, not size: after a rotation the file is non-empty before curl starts.
  _i=0
  while [ "$_i" -lt 20 ]; do
    [ "$(oracle_mark)" -gt "${_s0:-0}" ] && break
    nap 0.1; _i=$((_i+1))
  done
  if [ "$(oracle_mark)" -gt "${_s0:-0}" ]; then say "oracle live (pid $(cat "$OPID" 2>/dev/null)) -> $MSSLOG"; return 0; fi
  say "WARN: oracle produced nothing yet (port $LOGPORT closed?)"
  return 1
}
oracle_on() {
  P="$(oracle_pid)"
  if [ -n "$P" ]; then say "oracle already live (pid $P) -> $MSSLOG"; return 0; fi
  oracle_rotate
  oracle_start
  return 0
}
# oracle_ensure -- the start-of-run check: restart a capture that is dead, and
# detect a capture that is alive but NOT DELIVERING (growth, not size).
oracle_ensure() {
  if ! P="$(oracle_pid)"; then
    _old="$(cat "$OPID" 2>/dev/null)"
    [ -n "${_old:-}" ] && say "oracle: pid '$_old' is gone or is not our curl -- starting a fresh capture"
    rm -f "$OPID" 2>/dev/null
    oracle_rotate
    oracle_start; return $?
  fi
  _sz="$(oracle_mark)"
  _psz=""; _pt=""
  if [ -f "$OSTATE" ]; then
    read -r _psz _pt < "$OSTATE" 2>/dev/null || { _psz=""; _pt=""; }
  fi
  case "$_psz" in ''|*[!0-9]*) _psz="" ;; esac
  case "$_pt"  in ''|*[!0-9]*) _pt="" ;; esac
  if [ -n "$_psz" ] && [ -n "$_pt" ] && [ "$_sz" = "$_psz" ] \
     && [ $(( $(now_ms) - _pt )) -ge "$ORACLE_STALE_MS" ]; then
    say "oracle: capture pid $P has NOT grown in $(( $(now_ms) - _pt ))ms -- restarting it"
    oracle_off
    oracle_rotate
    oracle_start; return $?
  fi
  printf '%s %s\n' "$_sz" "$(now_ms)" > "$OSTATE" 2>/dev/null
  return 0
}
oracle_off() {
  P="$(oracle_pid)"
  if [ -n "$P" ]; then kill "$P" 2>/dev/null && say "oracle stopped (pid $P)"; else say "oracle not running"; fi
  rm -f "$OPID" "$OSTATE" 2>/dev/null
  return 0
}
oracle_tail() {
  echo "--- MS-S log (share/stream decisions) ---"
  otail | grep -aiE "STREAMING|RESYNC|startShare|stopShare|configure|input_caps|surface_state|surfaceId|does not exist|framerate" 2>/dev/null \
    | sed 's/^.*\]//' | grep -avE "avg time" | tail -n 24
  echo "--- (end) ---"
}
msf_fds() { ls -l /proc/"$(pids_of multi-screen-server)"/fd 2>/dev/null | grep -cE 'video|ion|dri'; }

status() {
  R="$(uris)"; D="$(disps)"; S="$(sps)"
  echo "== stick / lock =="; echo "  stick    : ${STICK:-<none - RAM fallback>}"; echo "  lock     : $LOCK $([ -d "$LOCK" ] && echo '(HELD pid'"$(cat "$LOCK/pid" 2>/dev/null)"')')"
  echo "== elements =="; printf '%s' "$R" | sed 's/},{/}\
{/g' | grep -o '"internalIdentifier":"[^"]*","name":"[^"]*"'
  echo "== wanted uris =="
  echo "  display          : $(uri_of "$D" navi-kombi-display)"
  echo "  streamParameters : $(uri_of "$S" $SPNAME)"
  echo "  nav element      : $(uri_of "$R" $NAVNAME)  (internalIdentifier $(id_of "$R" $NAVNAME))"
  echo "  our element      : $(uri_of "$R" $OURNAME)  (internalIdentifier $(id_of "$R" $OURNAME))"
  echo "== MS-S encoder fds (want 11 when streaming, 6 parked) =="; echo "  fds=${_fds:=$(msf_fds)}"
  echo "== last MS-S decision =="; grep -aE "STREAMING|surfaceId: [0-9]+|startShare \[id" "$MSSLOG" 2>/dev/null | sed 's/^.*\[//' | tail -n2
}

# ============================================================================
# HANDOVER -- gated on MS-S's OWN decisions (the oracle), not on fixed sleeps:
#   park    delete the nav element, then WAIT for `STOP STREAMING` (the nav's
#           TransportStream died -> (ip,port) is free for our sender)
#   back    re-create the nav element (or resync it), then WAIT for
#           `share_surface_id = 33` (the car's encoder is built again)
# A park MS-S did not process leaves the nav's TransportStream holding (ip,port).
# ============================================================================

# A MARK is a BYTE OFFSET, not a line number: the oracle grows ~8 MB/min, so the old
# `sed -n 'N,$p'` gate read the whole file every pass -- fine at 1 Hz, unusable at
# 10 Hz.  tail -c from the mark reads only what is new.
oracle_mark() { if [ -f "$MSSLOG" ]; then wc -c < "$MSSLOG" 2>/dev/null | tr -d ' '; else echo 0; fi; }
oracle_since() {  # oracle_since <byte-offset>
  _off="${1:-0}"
  _sz="$(oracle_mark)"
  [ "${_sz:-0}" -lt "${_off:-0}" ] && _off=0     # rotated/re-created: start over
  tail -c "+$(( _off + 1 ))" "$MSSLOG" 2>/dev/null
}
# wait_oracle_ms <ERE> <budget-ms> [byte-mark]  -> 0 if a NEW line matched.
# Sub-second polling, millisecond budget, and it reports how long the gate took.
wait_oracle_ms() {
  wo_re="$1"; wo_budget="${2:-20000}"; wo_from="${3:-0}"
  wo_t0="$(now_ms)"
  while :; do
    if [ -f "$MSSLOG" ]; then
      if oracle_since "$wo_from" | grep -aqE "$wo_re"; then
        LAST_WAIT=$(( $(now_ms) - wo_t0 )); return 0
      fi
    fi
    if [ $(( $(now_ms) - wo_t0 )) -ge "$wo_budget" ]; then
      LAST_WAIT=$(( $(now_ms) - wo_t0 )); return 1
    fi
    nap "$OPOLL"
  done
}
wait_oracle() { wait_oracle_ms "$1" "$(( ${2:-20} * 1000 ))" "${3:-0}"; }

oracle_live() { [ -s "$MSSLOG" ] 2>/dev/null; }

handover_park() {
  R="$(uris)" || { say "HANDOVER PARK NO-GO: cannot read the RSI element list"; return 1; }
  NAVU="$(uri_of "$R" $NAVNAME)"
  _mk="$(oracle_mark)"
  if [ -z "$NAVU" ]; then
    say "nav element already absent (parked)"
    say "HANDOVER PARK: nav was already absent; no stop expected"
    return 0
  fi
  bc "handover:park $NAVU"; h DELETE "http://$RSI:$DMP$NAVU" >/dev/null; echo
  if [ "${NOOR:-0}" = "1" ]; then
    sleep 3; say "HANDOVER PARK: DEGRADED (no oracle) -- slept 3s instead of waiting for STOP STREAMING"
    return 0
  fi
  # WAIT for MS-S to say it stopped -- this is the (ip,port) being freed
  _park_t="${MIB3_DLV_PARK_T:-8}"
  if wait_oracle "STOP STREAMING|stopShare \[" "$_park_t" "$_mk"; then
    say "HANDOVER PARK OK: MS-S logged STOP STREAMING (nav TransportStream destroyed) after ${LAST_WAIT}ms"
    return 0
  fi
  say "HANDOVER PARK WARN: no STOP STREAMING within ${_park_t}s -- MS-S may not have observed the park"
  say "  (the nav's TransportStream may still hold (ip,port); the port is not ours)"
  return 1
}

# ----------------------------------------------------------------------------
# handover back [secs] -- THE DOWN DIRECTION: put the cluster back on the car's own
# map (ivi 33).  pathb_daemon.sh's release() runs it after every session and on
# every failure path.
#
# It is gated on MS-S's own decisions, and it knows the trap that makes a plain
# "delete ours, create the nav" leave the cluster on our stale frame:
#
#   THE RE-CREATED-NAV TRAP.  If the nav engine re-created its element during our
#   session (the documented behaviour), MS-S's diff identity (surfaceId, ip, port)
#   says the nav is "already present" -- so deleting OUR element runs
#   stopShare(ours) but NEVER startShare for the nav (no identity changed).
#   Result: no TransportStream at all, the cluster retains our last frame, and every
#   gate looks "fine".  Only a resync (stop+recreate) forces a start, so step 3
#   retries through one.  [STATIC]+[EXEC]
# ----------------------------------------------------------------------------
# resync is TIMESTAMP-GATED in updateStreamParametersElement @0x131a30: it fires only
# on resync==true AND a changed timestamp, at one-second resolution.  Re-POSTing true
# after it latched true is a NO-OP, so drive a real 0 -> 1 transition across a second.
back_resync() {
  if [ "${MIB3_DLV_RESYNC_FLIP:-1}" = "1" ]; then
    h POST "http://$RSI:$DMP$SP" '{"resync":false}' >/dev/null 2>&1; echo; nap_next_second
  fi
  h POST "http://$RSI:$DMP$SP" '{"resync":true}' >/dev/null 2>&1; echo
}
handover_back() {
  _to="${1:-20}"                       # the SLOW budget (rescue path)
  _fast=$(( ${MIB3_DLV_BACK_T:-3} * 1000 ))   # the FAST budget: one sample + encoder
  hb_t0="$(now_ms)"; TIMING=""; T_SUM=0; tmark
  MUTSUB=""; EXTRA=""; REQ=""; FLUSH=""
  sub_reset
  sub0; R="$(uris)" && D="$(disps)" && S="$(sps)"; _lr=$?; sub_add lists
  [ "$_lr" = 0 ] || { say "HANDOVER BACK NO-GO: cannot read the RSI lists"; return 1; }
  DISP="$(uri_of "$D" navi-kombi-display)"; SP="$(uri_of "$S" $SPNAME)"
  OURU="$(uri_of "$R" $OURNAME)"; NAVU="$(uri_of "$R" $NAVNAME)"
  valid_uri "$DISP" || { say "HANDOVER BACK NO-GO: bad display uri '$DISP'"; return 1; }
  valid_uri "$SP"   || { say "HANDOVER BACK NO-GO: bad sp uri '$SP'"; return 1; }
  sub0; _mk="$(oracle_mark)"; sub_add mark

  # ---- TRAP PREDICTION (cheap, and it saves the whole fast gate) -------------
  # If the nav engine re-created its element during our session, MS-S already sampled
  # identity (33, ip, port): it tried startShare, the (ip,port) dedup refused
  # (`Failed to create transportStream`) and it will NOT retry while that identity
  # stays in its stream list.  Two independent tells, either sufficient:
  #   (a) the nav element EXISTS while our session is live (the engine put it back);
  #   (b) the oracle said `Failed to create transportStream` since our share.
  # Neither is required: the fast gate below still catches the trap, it just pays its
  # budget first -- this is an accelerator, not the proof.  The 2 MB `tail | grep`
  # sits inside the `mutations` span, which is why it is timed separately.
  _trap=0
  [ -n "$NAVU" ] && _trap=1
  if [ "$_trap" = "0" ] && [ -f "$MSSLOG" ]; then
    sub0
    otail | grep -aqE "Failed to create transportStream" 2>/dev/null && _trap=1
    sub_add grep
  fi

  # [1] BOTH mutations in one sample window.  MS-S applies removals first, so a
  #     single pass does stopShare(ours) -> (ip,port) freed -> startShare(33).
  #     Waiting for `STOP STREAMING` first (the old step 2) cost a whole extra
  #     MS-S sample for no gain: nothing here depends on it.
  if [ -n "$OURU" ]; then
    sub0; bc "handover:back:delete-ours $OURU"; sub_add sync
    sub0; h DELETE "http://$RSI:$DMP$OURU" >/dev/null; sub_add del; echo
  else
    # Path B never attaches an element, so this is the normal case.  It says
    # nothing about the nav: the last `share_surface_id` in the log predates the
    # park, so only a share logged after _mk below is evidence.
    say "HANDOVER BACK: nothing of ours was attached"
  fi
  if [ -z "$NAVU" ]; then
    sub0; bc "handover:back:create-nav"; sub_add sync
    sub0; h POST "http://$RSI:$DMP/displaymanagement/renderingSurfaces" \
      "{\"name\":\"$NAVNAME\",\"display\":\"$DISP\",\"streamParameters\":\"$SP\",\"topLeftX\":0,\"topLeftY\":0,\"width\":1280,\"height\":480}" >/dev/null; sub_add post; echo
  else
    say "HANDOVER BACK: the nav element is already present (trap predicted: no startShare will come without a resync)"
  fi
  if [ "${NOOR:-0}" = "1" ]; then
    # No oracle: the trap still needs its resync, and the element list is the
    # only evidence left.
    [ "$_trap" = "1" ] && { bc "handover:back:resync"; back_resync; }
    _r2="$(uris)" && [ -n "$(uri_of "$_r2" $NAVNAME)" ] && [ -z "$(uri_of "$_r2" $OURNAME)" ] \
      || { say "HANDOVER BACK FAIL: DEGRADED (no oracle) and the nav element is not back"; return 1; }
    say "HANDOVER BACK: DEGRADED (no oracle) -- nav element present (trap=$_trap); the encoder is unverified"
    return 0
  fi
  tstep mutations
  MUTSUB="$(sub_str "$T_LAST")"
  _t_last_mut="$T_MARK"     # the moment the last mutation returned (step end)

  # [2] FAST gate: `share_surface_id = 33`, one MS-S sample + the encoder build.
  # THE GATE IS `share_surface_id`, NOT `startShare`: a `startShare [id = 33]` is
  # logged even when Weston has no ivi 33 (MS-S grabs, gets "does not exist", sets
  # holder=3 and builds NO encoder), so gating on startShare would report success
  # while the cluster shows nothing.  [STATIC]+[EXEC]
  if [ "$_trap" = "0" ]; then
    if wait_oracle_ms "share_surface_id = $NAVID([^0-9]|\$)" "$_fast" "$_mk"; then
      tstep gate
      # last mutation -> MS-S's line visible: how much of the gate is log flush
      FLUSH=$(( $(now_ms) - _t_last_mut )); [ "$FLUSH" -lt 0 ] && FLUSH=0
      EXTRA="$EXTRA flush=${FLUSH}ms"
      say "HANDOVER BACK OK: encoder built for the car's map ($NAVID) after ${LAST_WAIT}ms"
      tline back "$(( $(now_ms) - hb_t0 ))"
      return 0
    fi
    say "HANDOVER BACK: no nav share within ${MIB3_DLV_BACK_T:-3}s"
  fi

  # [3] THE RESCUE -- forced (predicted) or earned (the fast gate timed out).
  say "HANDOVER BACK: no nav share yet -- forcing a resync (re-created-nav trap)"
  bc "handover:back:resync"
  _mk2="$(oracle_mark)"
  back_resync
  _t_resync="$(now_ms)"
  if wait_oracle_ms "share_surface_id = $NAVID([^0-9]|\$)" $(( _to * 1000 )) "$_mk2"; then
    tstep rescue
    FLUSH2=$(( $(now_ms) - _t_resync )); [ "$FLUSH2" -lt 0 ] && FLUSH2=0
    EXTRA="$EXTRA flush2=${FLUSH2}ms"
    say "HANDOVER BACK OK (after resync): encoder built for $NAVID"
    tline back "$(( $(now_ms) - hb_t0 ))"
    return 0
  fi
  say "HANDOVER BACK FAIL: the car's map is not being shared again (no 'share_surface_id = $NAVID')"
  oracle_tail
  return 2
}

do_handover() {
  _st="$1"
  _to="${2:-}"
  bc "handover:start stage=$_st"
  # restart a dead/not-delivering capture before any gate uses it.  A non-empty
  # log is not proof: a capture that just failed to restart leaves the old one.
  if oracle_ensure && oracle_live; then NOOR=0; else
    NOOR=1
    say "HANDOVER WARN: the MS-S oracle is silent (port $LOGPORT closed?) -- gates DEGRADE to"
    say "  fixed waits and the pass/fail verdicts become unverifiable. Read the dash + MS-S fds."
  fi
  export NOOR
  # back puts what its RSI requests cost and what curl returned on its HANDOVER
  # TIMING line; park prints it here, so a `--max-time 8` expiry is visible in both.
  case "$_st" in
    park)   handover_park;   _rc=$?; rsi_report; return $_rc ;;
    back)   handover_back "${_to:-20}"; return $? ;;
  esac
}


case "${1:-}" in
  status)  status ;;
  handover) case "${2:-}" in park|back) ;; *) echo "usage: $0 handover {park|back [secs]}"; exit 2 ;; esac
           shift; acquire || exit 1; do_handover "$1" "${2:-}"; exit $? ;;
  oracle)  shift; case "${1:-start}" in
             start) oracle_on ;;
             stop)  oracle_off ;;
             tail)  oracle_tail ;;
             *) echo "usage: $0 oracle {start|stop|tail}"; exit 2 ;;
           esac ;;
  *) echo "usage: $0 {status|handover {park|back [secs]}|oracle {start|stop|tail}}"; exit 2 ;;
esac
