#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# ============================================================================
# pathb_daemon.sh -- Path B's resident supervisor.
#
# WHAT IT IS.  One process, one loop, four states.  It owns the lease on the
# cluster's video feed: either the car's own nav produces it, or we do.  It is the
# only thing that decides, and it decides from EVENTS, never from a clock.  Its
# predecessor (`pathb_ctl.sh up|down`) was a chain of blocking verbs written for a
# human at a terminal; every defect that shape produced -- sleeps as
# synchronisation, a network probe as a boolean, log lines as control flow -- is
# gone by construction here.
#
# THE CONTRACT (all paths under <ram>, default /dev/shm):
#   IN   pathb_conn.state    from mib3clusterstream: conn ready bound au out ms seq
#                            `au` = access units RECEIVED from the shim, and THIS is
#                            the session signal.  A socket is not: on wireless
#                            CarPlay the phone's TCP connection stays ESTABLISHED
#                            after the phone leaves, so "connected" outlives the
#                            session by hours.
#   IN   pathb_daemon.cmd    one-shot operator command ("release")
#   IN   pathb_daemon.pause  present = never take over
#   IN   /proc/net/udp{,6}   is the port free? THE proof, not a hint
#   OUT  pathb_sender.ctl    "bind <port>" | "unbind"
#   OUT  pathb_daemon.state  what the CLI reads
#   OUT  pathb_daemon.log    one line per decision, monotonic ms
#
# THE STATES (`owner` is the one resource everybody wants):
#   IDLE      owner=nav.  Watching `au`; once frames have arrived for READY_STABLE
#             the phone has a live cluster map and we take over.
#   TAKEOVER  owner=nobody (transient): park the nav element, bind the port the
#             instant the kernel says it is free, wait for one emitted AU.
#   ACTIVE    owner=us.  When `au` stops advancing for GRACE the session is over,
#             however healthy the socket still looks.
#   RELEASE   owner=nobody (transient): unbind (which frees the port), prove it in
#             the kernel, hand the nav element back, confirm.
#   DEGRADED  a step did not complete: retry the release on a backoff, never arm.
#
# THE INVARIANTS (structural, not comments -- no state can break them):
#   1. We are never the producer without a bound port (ACTIVE requires bound).
#   2. We never hand the cluster back while we still hold the port: MS-S sets
#      SO_REUSEPORT and on a failed bind it falls back to an EPHEMERAL port and
#      sends into a void the ECU drops.  Measured 2026-10-27: cluster dark.
#   3. We never leave the nav element deleted.  Every failure path runs RELEASE.
#   4. Every wait is CONDITION or DEADLINE.  No sleep exists "to give it time".
#
# WHAT IT DOES NOT DO: no decode, no surface, no display call, no thread in carplay,
# no log parsing as control flow, no pidfile as evidence, no probe of the wire.  It
# reads files, writes files, and makes the RSI calls that already exist in
# deliver_ctl.sh.
#
# USAGE (on the unit):  sh pathb_daemon.sh run|once|status|stop
#
# ENV (defaults are the live unit values; every wait has a deadline):
#   PATHB_RAM / PATHB_SOCK / PATHB_MUXER / PATHB_DELIVER
#                           /dev/shm, <ram>/mib3cluster.sock,
#                           /tmp/kit/mib3clusterstream, sibling deliver_ctl.sh
#   PATHB_BENCH=1           skip the unit-only preparation steps
#   PATHB_POLL_MS           200    loop tick
#   PATHB_READY_STABLE_MS   400    frames must be arriving this long (debounce)
#   PATHB_GRACE_MS          4500   no new AU this long -> the session is over.
#                                  The phone pauses 1.5-2.3 s whenever its map is
#                                  static or a scene changes, and 3.1-4.1 s at
#                                  guidance start/stop on WIRED CarPlay (measured
#                                  2026-11-02); at 1.5 s every one of those read as
#                                  a disconnect (release 3.3 s, re-take 2.6 s = the
#                                  "5 second" switch the owner saw).
#   PATHB_RETRY_MS          2000   backoff after a failed takeover
#   PATHB_TAKEOVER_MS       9000   whole takeover deadline (park + bind + emit)
#   (the release has no deadline of its own: deliver_ctl.sh's handover gates are
#    already bounded by MS-S's own 1 Hz clock.)
#   PATHB_LOG_MAX           65536  log truncated to its last half beyond this
#   PATHB_D_PARK_CMD / PATHB_D_BACK_CMD / PATHB_D_PORTFREE_CMD   bench seams
# ============================================================================
set -u

HERE=$(cd "$(dirname "$0")" 2>/dev/null && pwd || echo /tmp/kit)
RAM=${PATHB_RAM:-/dev/shm}
SOCK=${PATHB_SOCK:-$RAM/mib3cluster.sock}
STATE=$RAM/pathb_daemon.state
LOG=$RAM/pathb_daemon.log
PIDF=$RAM/pathb_daemon.pid
CMDF=$RAM/pathb_daemon.cmd
PAUSEF=$RAM/pathb_daemon.pause
SSTATE=${PATHB_SENDER_STATE:-$RAM/pathb_conn.state}
SCTL=${PATHB_SENDER_CTL:-$RAM/pathb_sender.ctl}
SPIDF=$RAM/pathb_sender.pid
SLOG=$RAM/pathb_sender.log
MUXER=${PATHB_MUXER:-/tmp/kit/mib3clusterstream}
DELIVER=${PATHB_DELIVER:-$HERE/deliver_ctl.sh}
DELAYLOG=$RAM/pathb_deliver.log
DST=${PATHB_DST:-ff14::2:4%eth0.5}
GPORT=${PATHB_GROUP_PORT:-42800}
BENCH=${PATHB_BENCH:-0}
SCOMM=${PATHB_D_SENDER_COMM:-}     # bench seam: expected /proc comm of the sender

POLL=${PATHB_POLL_MS:-200}
READY_STABLE=${PATHB_READY_STABLE_MS:-400}
GRACE=${PATHB_GRACE_MS:-4500}
RETRY=${PATHB_RETRY_MS:-2000}
TAKEOVER_MS=${PATHB_TAKEOVER_MS:-9000}
LOG_MAX=${PATHB_LOG_MAX:-65536}

PARK_CMD=${PATHB_D_PARK_CMD:-}
BACK_CMD=${PATHB_D_BACK_CMD:-}
FREE_CMD=${PATHB_D_PORTFREE_CMD:-}

# state (single instance, written by the loop)
ST=IDLE
ST_SINCE=0
ST_REASON=-
PORT=""
STARTED=0
SENDER_PID=""
KNOBS_PID=""
ORACLE_DONE=0
WHEEL_DONE=0
PREPARED=0
SOCK_READY=0
DISC_AT=0
ORA_AT=0
S_CONN=0; S_READY=0; S_BOUND=0; S_AU=0; S_OUT=0; S_MS=0
LAST_AU=-1; LAST_FLOW=0; FLOW_SINCE=0; DEAD_SINCE=0; NEXT_RETRY=0; RETRY_AT=0; CONN_SINCE=0

# ------------------------------------------------------------------ plumbing
now_ms(){
  _uf=${MIB3_DLV_UPTIME:-/proc/uptime}
  if [ -r "$_uf" ]; then
    _u=$(cut -d' ' -f1 "$_uf" 2>/dev/null)
    _s=${_u%%.*}; case "$_s" in ''|*[!0-9]*) _s=0 ;; esac
    case "$_u" in *.*) _f=${_u#*.} ;; *) _f="" ;; esac
    _f=${_f}000; _f=${_f%"${_f#???}"}
    _f=${_f#0}; _f=${_f#0}; _f=${_f#0}
    printf '%s' "$(( _s * 1000 + ${_f:-0} ))"
    return 0
  fi
  if command -v perl >/dev/null 2>&1; then
    _p=$(perl -MTime::HiRes=time -e 'printf "%d", time()*1000' 2>/dev/null)
    case "${_p:-}" in ''|*[!0-9]*) ;; *) printf '%s' "$_p"; return 0 ;; esac
  fi
  _n=$(date +%s 2>/dev/null); case "${_n:-}" in ''|*[!0-9]*) _n=0 ;; esac
  printf '%s' "$(( _n * 1000 ))"
}

NAP_FRAC=1
sleep 0.05 2>/dev/null || {
  # busybox sleep here rejects fractions, but `usleep` IS an applet (verified on
  # the unit 2026-11-02: `usleep 200000` works).  Without this the whole loop ran
  # at 1 s granularity -- poll, port race and grace all coarsened together.
  NAP_FRAC=0
  usleep 50000 2>/dev/null || NAP_MODE=none
}
NAP_MODE=${NAP_MODE:-}
nap_ms(){
  if [ "$NAP_FRAC" = "1" ]; then
    sleep "$(( $1 / 1000 )).$(printf '%03d' "$(( $1 % 1000 ))")" 2>/dev/null || sleep 1
  elif [ "$NAP_MODE" != "none" ]; then
    usleep "$(( $1 * 1000 ))" 2>/dev/null || sleep 1
  else
    sleep 1
  fi
}

log(){
  _l="$(now_ms) state=$ST $*"
  printf '%s\n' "$_l" >>"$LOG" 2>/dev/null
  [ "${PATHB_D_VERBOSE:-0}" = "1" ] && printf '%s\n' "$_l"
  # bounded: keep the last half when it grows past LOG_MAX
  _sz=$(wc -c <"$LOG" 2>/dev/null | tr -d ' ')
  case "${_sz:-}" in ''|*[!0-9]*) return 0 ;; esac
  if [ "$_sz" -gt "$LOG_MAX" ]; then
    _keep=$(( LOG_MAX / 2 ))
    tail -c "$_keep" "$LOG" >"$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG" 2>/dev/null
  fi
}

state_write(){
  printf 'state=%s since_ms=%s started_ms=%s reason=%s port=%s bound=%s conn=%s ready=%s au=%s out=%s paused=%s sender=%s\n' \
    "$ST" "$ST_SINCE" "$STARTED" "$ST_REASON" "${PORT:--}" "$S_BOUND" "$S_CONN" "$S_READY" \
    "$S_AU" "$S_OUT" "$(paused && echo yes || echo no)" "${SENDER_PID:--}" >"$STATE" 2>/dev/null
}

set_state(){  # set_state <state> <reason>
  [ "$1" = "$ST" ] || { ST=$1; ST_SINCE=$(now_ms); }
  ST_REASON=${2:--}
  log "-> $ST ($ST_REASON)"
  state_write
}

paused(){ [ -f "$PAUSEF" ]; }
me(){ cat "$PIDF" 2>/dev/null; }

# --------------------------------------------------------------- the sender
proc_comm(){  # basename of pid $1's executable, truncated like /proc comm
  if [ -r "/proc/$1/comm" ]; then
    _c=""; read -r _c <"/proc/$1/comm" 2>/dev/null || true
    printf '%s' "$_c"
  else
    ps -o comm= -p "$1" 2>/dev/null | sed 's|.*/||' | cut -c1-15
  fi
}

sender_alive(){
  _p=${SENDER_PID:-}
  [ -n "$_p" ] || _p=$(cat "$SPIDF" 2>/dev/null)
  case "${_p:-}" in ''|*[!0-9]*) return 1 ;; esac
  kill -0 "$_p" 2>/dev/null || return 1
  _want=${SCOMM:-$(basename "$MUXER" | cut -c1-15)}
  _got=$(proc_comm "$_p")
  # a recycled pid is not our sender
  [ "$_got" = "$_want" ] || return 1
  SENDER_PID=$_p
  return 0
}

sender_start(){
  sender_alive && return 0
  [ -x "$MUXER" ] || { log "sender: not executable ($MUXER)"; return 1; }
  rm -f "$SCTL"
  : >"$SLOG"
  # --monitor: own the AU socket now, bind the cluster's UDP port only on
  # 'bind <port>' (see the ctl block in mib3clusterstream.c).  NO PORT IS NEEDED
  # TO START: the port travels with the bind command.  Requiring one here made
  # the daemon unable to arm on a fresh boot where multi-screen-server has not
  # created its stream yet (measured on the car 2026-11-02).
  if [ -n "${PORT:-}" ]; then
    nohup "$MUXER" --in "unix:$SOCK" --dst "$DST" --port "$GPORT" --sport "$PORT" \
          --conn-state "$SSTATE" --ctl "$SCTL" --monitor >>"$SLOG" 2>&1 &
  else
    nohup "$MUXER" --in "unix:$SOCK" --dst "$DST" --port "$GPORT" \
          --conn-state "$SSTATE" --ctl "$SCTL" --monitor >>"$SLOG" 2>&1 &
  fi
  SENDER_PID=$!
  printf '%s\n' "$SENDER_PID" >"$SPIDF"
  # Bounded: only ask "did it stay up".  The socket itself is labelled/chmodded
  # by prepare_tick on every tick, so nothing here waits for it (a sender that
  # is slow to create its socket must not hold the state machine up).
  _t=$(now_ms)
  while [ $(( $(now_ms) - _t )) -lt 500 ]; do
    kill -0 "$SENDER_PID" 2>/dev/null || { log "sender: exited during start"; SENDER_PID=""; return 1; }
    nap_ms 50
  done
  return 0
}

sender_stop(){  # bounded ladder: TERM, 1 s, KILL, 2 s
  _p=${SENDER_PID:-$(cat "$SPIDF" 2>/dev/null)}
  case "${_p:-}" in ''|*[!0-9]*) return 0 ;; esac
  kill "$_p" 2>/dev/null
  _i=0
  while [ "$_i" -lt 10 ]; do
    kill -0 "$_p" 2>/dev/null || break
    nap_ms 100; _i=$((_i+1))
  done
  kill -9 "$_p" 2>/dev/null
  _i=0
  while [ "$_i" -lt 20 ]; do
    kill -0 "$_p" 2>/dev/null || break
    nap_ms 100; _i=$((_i+1))
  done
  rm -f "$SPIDF"
  SENDER_PID=""
  return 0
}

read_sender(){
  S_CONN=0; S_READY=0; S_BOUND=0; S_AU=0; S_OUT=0; S_MS=0
  [ -r "$SSTATE" ] || return 0
  _l=""; read -r _l <"$SSTATE" 2>/dev/null || true
  for _kv in $_l; do
    case "$_kv" in
      conn=*)  S_CONN=${_kv#conn=} ;;
      ready=*) S_READY=${_kv#ready=} ;;
      bound=*) S_BOUND=${_kv#bound=} ;;
      au=*)    S_AU=${_kv#au=} ;;
      out=*)   S_OUT=${_kv#out=} ;;
      ms=*)    S_MS=${_kv#ms=} ;;
    esac
  done
  case "$S_CONN"  in ''|*[!0-9]*) S_CONN=0 ;; esac
  case "$S_READY" in ''|*[!0-9]*) S_READY=0 ;; esac
  case "$S_BOUND" in ''|*[!0-9]*) S_BOUND=0 ;; esac
  case "$S_AU"    in ''|*[!0-9]*) S_AU=0 ;; esac
  case "$S_OUT"   in ''|*[!0-9]*) S_OUT=0 ;; esac
  return 0
}

ctl(){ printf '%s\n' "$1" >"$SCTL" 2>/dev/null; }

# ------------------------------------------------------------------- the port
port_free(){
  if [ -n "$FREE_CMD" ]; then sh -c "$FREE_CMD" >/dev/null 2>&1; return $?; fi
  case "${PORT:-}" in ''|*[!0-9]*) return 1 ;; esac
  _hex=$(printf '%04X' "$PORT" 2>/dev/null) || return 1
  for _f in /proc/net/udp6 /proc/net/udp; do
    [ -r "$_f" ] || continue
    if awk -v want=":$_hex" 'NR>1 { n=split($2,a,":"); if (":" a[n] == want) { found=1 } }
                            END { exit(found?0:1) }' "$_f" 2>/dev/null; then
      return 1                      # held
    fi
  done
  return 0                          # the kernel does not have it
}

discover_port(){
  _m=""
  for c in "$HERE/pathb/msport.py" "$HERE/msport.py" /tmp/kit/msport.py; do
    [ -f "$c" ] && { _m=$c; break; }
  done
  [ -n "$_m" ] || { log "port: msport.py not found"; return 1; }
  _o=$(${PATHB_MPY:-python} "$_m" 2>/dev/null)
  case "${_o:-}" in
    ''|*[!0-9]*)
      # MS-S is parked, or has just restarted: it legitimately owns no socket,
      # so discovery is empty.  The port we last saw is still the right one --
      # MS-S binds it once per process.  Without this, a daemon restart while
      # the cluster is ours can never find the port again (measured on the car
      # 2026-11-02: "discovery gave nothing" -> no sender -> takeover with no
      # port -> park -> fail -> hand back).
      _r=$(cat "$RAM/pathb_sport" 2>/dev/null)
      case "${_r:-}" in
        ''|*[!0-9]*) log "port: discovery gave nothing ($_o)"; return 1 ;;
        *) PORT=$_r; log "port: $PORT (recorded; MS-S owns no socket right now)"; return 0 ;;
      esac ;;
    *) PORT=$_o
       printf '%s\n' "$PORT" >"$RAM/pathb_sport" 2>/dev/null
       log "port: MS-S source port $PORT"
       return 0 ;;
  esac
}

# ------------------------------------------------------------------ RSI calls
park(){   # delete the nav element; bounded inside deliver_ctl
  if [ -n "$PARK_CMD" ]; then sh -c "$PARK_CMD"; return $?; fi
  sh "$DELIVER" handover park >>"$DELAYLOG" 2>&1
}
back(){
  if [ -n "$BACK_CMD" ]; then sh -c "$BACK_CMD"; return $?; fi
  sh "$DELIVER" handover back >>"$DELAYLOG" 2>&1
}

diagnose(){
  mkdir -p "$RAM/pathb_diag" 2>/dev/null
  tail -n 12 "$SLOG" >"$RAM/pathb_diag/sender.log" 2>/dev/null
  tail -n 12 "$DELAYLOG" >"$RAM/pathb_diag/deliver.log" 2>/dev/null
  state_write
  cp "$STATE" "$RAM/pathb_diag/state" 2>/dev/null
  log "diagnostics archived in $RAM/pathb_diag"
}

# ------------------------------------------------------------------ the lease
takeover(){  # -> 0 on success; the caller sets ACTIVE
  _t0=$(now_ms)
  # The port may not exist yet: multi-screen-server creates its socket when it
  # first has something to stream.  Discover it HERE, at the moment we actually
  # need it, rather than refusing to arm at boot.
  if [ -z "${PORT:-}" ]; then
    discover_port || { log "TAKEOVER: MS-S has no stream yet, so there is no port to take -- not arming"; return 1; }
  fi
  log "TAKEOVER: phone has frames (au=$S_AU); park + bind"
  park & _parkpid=$!
  _bound=0; _tk_done=0; _tk_done_at=0
  while :; do
    [ $(( $(now_ms) - _t0 )) -lt "$TAKEOVER_MS" ] || break
    # If the park has already RETURNED and failed, waiting out the full deadline
    # buys nothing: stop early (bounded grace for the socket to close).
    if ! kill -0 "$_parkpid" 2>/dev/null; then
      if [ "$_tk_done" = "0" ]; then _tk_done=1; _tk_done_at=$(now_ms); fi
      [ $(( $(now_ms) - _tk_done_at )) -gt 1500 ] && break
    fi
    if port_free; then
      ctl "bind $PORT"
      _w=$(now_ms)
      while [ $(( $(now_ms) - _w )) -lt 800 ]; do
        read_sender
        [ "$S_BOUND" = "$PORT" ] && { _bound=1; break; }
        nap_ms 50
      done
      [ "$_bound" = "1" ] && break
      ctl "unbind"
    fi
    nap_ms 100
  done
  if [ "$_bound" != "1" ]; then
    log "TAKEOVER FAIL: the port never became ours within ${TAKEOVER_MS}ms"
    wait "$_parkpid" 2>/dev/null
    return 1
  fi
  log "TAKEOVER: port $PORT is ours after $(( $(now_ms) - _t0 ))ms"

  # The park must ALSO have succeeded: a nav element left in place means MS-S
  # would rebuild its stream on a port we are holding, fail to bind, and send
  # from an ephemeral port the ECU drops.
  wait "$_parkpid" 2>/dev/null; _prc=$?
  if [ "$_prc" != "0" ]; then
    log "TAKEOVER FAIL: park rc=$_prc -- giving the port back"
    return 1
  fi

  # FIRST EMIT is the proof the wire is ours and decodable -- not "bound".
  while :; do
    read_sender
    [ "$S_OUT" -gt 0 ] && break
    if [ $(( $(now_ms) - _t0 )) -ge "$TAKEOVER_MS" ]; then
      log "TAKEOVER FAIL: bound but nothing was emitted within ${TAKEOVER_MS}ms"
      return 1
    fi
    nap_ms 100
  done
  log "TAKEOVER OK: first AU on the wire after $(( $(now_ms) - _t0 ))ms"
  return 0
}

release(){  # <reason> -> 0 only when the cluster is genuinely back with the nav
  _why=${1:-release}
  _t0=$(now_ms)
  read_sender
  # DID WE EVER HOLD IT?  If not, a busy port is MS-S's own and perfectly normal
  # (a takeover can fail during the park, before any bind).  Only the holder can
  # block a hand-back -- that is the 2026-10-27 outage this check exists for.
  _had=${S_BOUND:-0}
  log "RELEASE ($_why): unbind, prove the port, hand back (we held: $_had)"
  ctl "unbind"
  _w=$(now_ms)
  while [ $(( $(now_ms) - _w )) -lt 2500 ]; do
    read_sender
    [ "$S_BOUND" = "0" ] && break
    nap_ms 50
  done
  if [ "$_had" != "0" ] && ! port_free; then
    log "RELEASE WARN: udp/$PORT still bound after unbind -- not handing back yet"
    diagnose
    return 1
  fi
  _i=0; _rc=1
  while [ "$_i" -lt 2 ]; do
    if back; then _rc=0; break; fi
    _i=$((_i+1)); nap_ms 500
  done
  if [ "$_rc" != "0" ]; then
    log "RELEASE FAIL: hand-back refused after $(( $(now_ms) - _t0 ))ms"
    diagnose
    return 1
  fi
  log "RELEASE OK: the car's map owns the cluster again ($(( $(now_ms) - _t0 ))ms)"
  return 0
}

# THE STATE IS DERIVED FROM WHAT WE STILL HOLD, NOT FROM WHAT WE TRIED.
# `release` can fail after a partial job -- the port is freed but MS-S has not
# rebuilt its stream, or the RSI refused.  Saying IDLE there would be a lie that
# lasts until the next phone session: IDLE means "the car's nav owns the cluster".
settle(){  # <reason-if-fine> <release-rc>
  read_sender
  if [ "${2:-1}" != "0" ]; then
    # The hand-back itself did not complete.  Note we do NOT then check
    # port_free: after a GOOD hand-back MS-S owns the port, so "not free" is the
    # success case.  The only question is whether we still hold it.
    set_state DEGRADED "hand-back-failed"
    return 1
  fi
  if [ "${S_BOUND:-0}" != "0" ]; then
    set_state DEGRADED "still-hold-port"
    return 1
  fi
  set_state IDLE "${1:-idle}"
  return 0
}

# ------------------------------------------------------------------- prepare
smack_open(){
  [ -w /smack/load2 ] || return 1          # not ready yet (or not a SMACK unit)
  for _r in "carplay::app carplay::app rwx" \
            "carplay::app common::lib rwx" \
            "common::lib carplay::app rwx"; do
    echo "$_r" >/smack/load2 2>/dev/null || true
  done
  return 0
}

knobs_once(){   # once per carplay process, never per session (it is a ptrace)
  [ "$BENCH" = "1" ] && return 0
  _cp=$(pidof carplay 2>/dev/null | awk '{print $1}')
  [ -n "${_cp:-}" ] || return 0
  [ "$KNOBS_PID" = "$_cp" ] && return 0
  [ -f /tmp/kit/target_env.py ] || return 0
  if /usr/bin/python /tmp/kit/target_env.py "$_cp" MIB3_SHIM_SINK=helper >/dev/null 2>&1; then
    # THE SHIM'S CONFIGURATION LIVES HERE, NOT IN A ONE-OFF ENV POKE, and a reboot
    # must reproduce it exactly:
    #   SINK=helper   frames must leave the process at all
    #   FPS=60        the phone uses this as its rate (26.6 fps delivered)
    #   VIEWAREAS=0   the multi-area card advert was tried on the car and is buggy
    #   URL=...       map-only, no ETA, no speed limit, card top-aligned
    for _kv in MIB3_SHIM_FPS=60 \
               MIB3_SHIM_VIEWAREAS=0 \
               "MIB3_SHIM_URL=maps:/car/instrumentcluster/map?showETA=no&maneuverLayout=topAligned&showSpeedLimit=no" \
               MIB3_SHIM_CMDACK=1; do
      /usr/bin/python /tmp/kit/target_env.py "$_cp" "$_kv" >/dev/null 2>&1
    done
    KNOBS_PID=$_cp
    log "prepare: shim knobs set for carplay pid $_cp (sink=helper fps=60 card=off, MHI2 scene)"
  fi
}

oracle_housekeeping(){
  # THE MS-S LOG IS AN ORACLE, NOT A DATABASE.  Every handover gate marks its
  # position in it with a `wc -c` over the WHOLE file, so an unrotated log makes
  # each gate cost its size: measured on the car 2026-11-02, 142 MB -> mut_mark
  # 1,440 ms, i.e. ~1.4 s of a 2.8 s takeover spent counting a log.  Rotate on a
  # timer (stop+start also re-opens the write side correctly; truncating in place
  # would leave curl writing at its old offset).
  [ "$BENCH" = "1" ] && return 0
  [ -f "$DELIVER" ] || return 0
  [ $(( $(now_ms) - ${ORA_AT:-0} )) -ge 15000 ] || return 0
  ORA_AT=$(now_ms)
  _sz=$(wc -c <"$RAM/mss.log" 2>/dev/null | tr -d ' ')
  case "${_sz:-}" in ''|*[!0-9]*) return 0 ;; esac
  if [ "$_sz" -gt 8388608 ]; then
    log "oracle: rotating (${_sz} B) -- gates must stay cheap"
    sh "$DELIVER" oracle stop  >/dev/null 2>&1
    sh "$DELIVER" oracle start >/dev/null 2>&1
  fi
  return 0
}

prepare_tick(){   # idempotent, cheap; the loop's housekeeping
  # SMACK rules and the AU socket's label are set ONCE per boot, not every tick:
  # they cannot change underneath us, and each is a fork.  Retried until the
  # kernel node is writable (early boot), then never again.
  if [ "$PREPARED" = "0" ]; then
    if [ "$BENCH" = "1" ] || smack_open; then PREPARED=1; fi
  fi
  if [ -z "${PORT:-}" ] && [ -n "${PATHB_PORT:-}" ]; then
    PORT=$PATHB_PORT; log "port: $PORT (PATHB_PORT override)"
  fi
  # THE SENDER RUNS WITH OR WITHOUT A PORT.  The port is only needed at bind
  # time, and on a fresh boot multi-screen-server may not have created its
  # socket yet -- so gating the sender on discovery meant "no sender, no arming"
  # exactly when the phone arrives first (measured on the car 2026-11-02).
  sender_start || return 1
  oracle_housekeeping
  if [ -z "${PORT:-}" ]; then
    # Discovery spawns python: retry on a 5 s backoff, never every tick.  A
    # failure here is normal (no stream yet) and is retried by the loop.
    if [ $(( $(now_ms) - ${DISC_AT:-0} )) -ge 5000 ]; then
      DISC_AT=$(now_ms)
      discover_port || log "prepare: no MS-S port yet -- will keep looking"
    fi
  fi
  if [ "$BENCH" != "1" ]; then
    knobs_once
    if [ "$ORACLE_DONE" = "0" ] && [ -f "$DELIVER" ]; then
      MIB3_DLV_STICK=/dev/shm; export MIB3_DLV_STICK
      if sh "$DELIVER" oracle start >/dev/null 2>&1; then ORACLE_DONE=1; log "prepare: oracle up"; fi
    fi
    if [ "$WHEEL_DONE" = "0" ]; then
      _wz=$HERE/wheel_zoom.sh
      [ -f "$_wz" ] || _wz=/tmp/kit/wheel_zoom.sh
      if [ -f "$_wz" ]; then
        sh "$_wz" labels >/dev/null 2>&1
        sh "$_wz" start >/dev/null 2>&1
        WHEEL_DONE=1
      fi
    fi
  fi
  # the shim runs as `carplay`: mode AND label are both needed, once per socket
  if [ -S "$SOCK" ] && [ "$SOCK_READY" = "0" ]; then
    chmod 0777 "$SOCK" 2>/dev/null
    if [ "$BENCH" != "1" ] && command -v chsmack >/dev/null 2>&1; then
      chsmack -a carplay::app "$SOCK" >/dev/null 2>&1 || true
    fi
    SOCK_READY=1
  fi
  return 0
}

reconcile(){   # run once at startup: never inherit a half-taken cluster
  read_sender
  if [ "${S_BOUND:-0}" != "0" ]; then
    log "reconcile: a previous sender still holds udp/$S_BOUND -- releasing"
    release "reconcile"; _r=$?
    settle "reconcile" "$_r"
  fi
  rm -f "$CMDF"
}

# ---------------------------------------------------------------------- loop
do_tick(){
  _now=$(now_ms)
  read_sender
  # A DEAD SENDER'S STATE FILE IS NOT EVIDENCE.  Without this the daemon keeps
  # parsing the last line a killed sender wrote, and a fresh sender starting at
  # au=0 looks like "no change" instead of a new session.
  if ! sender_alive; then
    S_CONN=0; S_READY=0; S_BOUND=0; S_AU=0; S_OUT=0
  fi

  if [ -f "$CMDF" ]; then
    _cmd=""; read -r _cmd <"$CMDF" 2>/dev/null || true
    rm -f "$CMDF"
    case "$_cmd" in
      release*) log "cmd: operator release"
                release "operator"; _r=$?
                settle "operator-release" "$_r"
                DEAD_SINCE=0; FLOW_SINCE=0; LAST_AU=$S_AU ;;
      *) log "cmd: unknown '$_cmd' -- ignored" ;;
    esac
  fi

  case "$ST" in
    IDLE)
      if [ "${S_BOUND:-0}" != "0" ]; then
        # should be unreachable: IDLE means the nav owns it.  If it ever happens,
        # say so and let DEGRADED clean it up rather than arming on top.
        set_state DEGRADED "holding-port-in-idle"
      else
        if [ "$S_AU" != "$LAST_AU" ]; then
          if [ "$FLOW_SINCE" = "0" ]; then FLOW_SINCE=$_now; fi
          LAST_AU=$S_AU; LAST_FLOW=$_now
        fi
        if paused; then
          FLOW_SINCE=0
        elif [ "$FLOW_SINCE" != "0" ] && [ $(( _now - LAST_FLOW )) -le $(( 2 * POLL )) ] \
           && [ $(( _now - FLOW_SINCE )) -ge "$READY_STABLE" ] \
           && [ "${S_AU:-0}" -gt 0 ] \
           && [ "$_now" -ge "$NEXT_RETRY" ] && [ -n "${PORT:-}" ]; then
          # ...and a port must exist.  Without one there is nothing to take, and
          # attempting a takeover would park + hand back every 2 s for no reason
          # (measured on the car 2026-11-02, fresh boot, MS-S not streaming yet).
          # Discovery is retried by prepare_tick; we arm as soon as it lands.
          set_state TAKEOVER "frames-flowing"
          if takeover; then
            set_state ACTIVE "lease-taken"; DEAD_SINCE=0; LAST_AU=$S_AU
          else
            NEXT_RETRY=$(( $(now_ms) + RETRY ))
            release "takeover-failed"; _r=$?
            settle "takeover-failed" "$_r"
            FLOW_SINCE=0; LAST_AU=$S_AU
          fi
        fi
      fi ;;
    ACTIVE)
      if [ "$S_AU" = "$LAST_AU" ]; then
        [ "$DEAD_SINCE" = "0" ] && DEAD_SINCE=$_now
        if [ $(( _now - DEAD_SINCE )) -ge "$GRACE" ]; then
          log "the session ended: no AU for $(( _now - DEAD_SINCE ))ms (socket=$S_CONN, wireless keeps it open)"
          release "no-frames"; _r=$?
          settle "session-ended" "$_r"
          NEXT_RETRY=$(( $(now_ms) + RETRY )); FLOW_SINCE=0; DEAD_SINCE=0
        fi
      else
        # TOLERATED SILENCE IS LOGGED: the phone pauses at scene changes and that is
        # not a disconnect, so every pause we sat through is recorded and the next
        # grace decision is data instead of a guess.  Measured on WIRED CarPlay
        # 2026-11-02: 3.1-4.1 s at guidance start/stop (wireless 1.5-2.3 s), which
        # is why GRACE is 4500 ms -- just above the worst pause.
        if [ "${DEAD_SINCE:-0}" != "0" ]; then
          _gap=$(( _now - DEAD_SINCE ))
          [ "$_gap" -ge 2000 ] && log "tolerated a ${_gap}ms silence (phone paused; not a disconnect)"
        fi
        LAST_AU=$S_AU; DEAD_SINCE=0
      fi
      if [ "$ST" = "ACTIVE" ]; then
        if ! sender_alive; then
          log "the sender died while we owned the cluster"
          release "sender-lost"; _r=$?
          settle "sender-lost" "$_r"
          NEXT_RETRY=$(( $(now_ms) + RETRY )); FLOW_SINCE=0; DEAD_SINCE=0
        elif [ "${S_BOUND:-0}" = "0" ]; then
          # THE KERNEL DECIDES OWNERSHIP, NOT THE FILE.  The sender rewrites its
          # state file ~5x/s, so a reading taken mid-write says "bound=0" and
          # nothing else is true -- and acting on it costs the driver the map for
          # seconds (measured on the car: one 3.5 s blink).  A port claim of zero is
          # therefore only real if the KERNEL also says the port is free; if it is
          # still bound, the sender is alive (checked above) and this was a bad read.
          if [ -n "${PORT:-}" ] && ! port_free; then
            log "ACTIVE: sender reported bound=0 but the kernel still holds udp/$PORT -- bad read, ignoring"
          else
            log "the sender released the port (the kernel agrees it is free)"
            release "port-lost"; _r=$?
            settle "port-lost" "$_r"
            NEXT_RETRY=$(( $(now_ms) + RETRY )); FLOW_SINCE=0; DEAD_SINCE=0
          fi
        fi
      fi ;;
    DEGRADED)
      # Something did not complete.  The cluster may still be showing our last
      # frame (or the nav may have no stream).  Retry the release on a backoff --
      # NEVER arm from here, and never pretend to be IDLE.
      if [ $(( _now - ${RETRY_AT:-0} )) -ge 5000 ]; then
        RETRY_AT=$_now
        log "DEGRADED: retrying the release (${ST_REASON:-?})"
        release "retry"; _r=$?
        if settle "recovered" "$_r"; then
          NEXT_RETRY=$(( $(now_ms) + RETRY )); FLOW_SINCE=0; LAST_AU=$S_AU
        fi
      fi ;;
  esac
  state_write
}

do_run(){
  _me=$$
  _old=$(me)
  if [ -n "${_old:-}" ] && [ "$_old" != "$_me" ] && kill -0 "$_old" 2>/dev/null; then
    echo "pathb_daemon: already running (pid $_old)"
    return 0
  fi
  printf '%s\n' "$_me" >"$PIDF"
  STARTED=$(now_ms); ST_SINCE=$STARTED
  : >"$LOG" 2>/dev/null
  log "daemon start pid=$_me poll=${POLL}ms ready=${READY_STABLE}ms grace=${GRACE}ms"
  if [ "$NAP_FRAC" = "1" ]; then log "clock: fractional sleep"
  elif [ "$NAP_MODE" != "none" ]; then log "clock: usleep ($(( POLL ))ms tick)"
  else log "WARN no fractional sleep and no usleep -- every wait degrades to 1s"; fi
  if [ "$BENCH" != "1" ]; then
    trap 'log "daemon stop (signal)"; read_sender; [ "${S_BOUND:-0}" != "0" ] && release "daemon-stop"; sender_stop; rm -f "$PIDF"; exit 0' INT TERM HUP
  else
    trap 'rm -f "$PIDF"; exit 0' INT TERM HUP
  fi
  # ONE SENDER, AND IT IS OURS.  A previous daemon's sender survives (it is nohup'd
  # and orphaned) and an OLDER BINARY then keeps running with the old behaviour
  # while every counter looks right -- measured on the car 2026-11-02: a restarted
  # daemon adopted a pre-fix sender and the flap continued.  So: kill it, reconcile,
  # then start a fresh one.
  if sender_alive; then
    log "start: stopping a previous daemon's sender (pid $SENDER_PID)"
    sender_stop
  fi
  reconcile
  prepare_tick || log "prepare: not ready yet -- will retry"
  # reconcile may already have settled into DEGRADED; do not overwrite it.
  [ "$ST_REASON" = "-" ] && set_state IDLE "boot"
  while :; do
    nap_ms "$POLL"
    prepare_tick >/dev/null 2>&1 || true
    do_tick
  done
}

do_status(){
  _p=$(me)
  if [ -n "${_p:-}" ] && kill -0 "$_p" 2>/dev/null; then
    echo "daemon:   RUNNING pid $_p"
  else
    echo "daemon:   not running"
  fi
  if [ -r "$STATE" ]; then
    echo "state:    $(cat "$STATE")"
  else
    echo "state:    (no state file)"
  fi
  _l=$(now_ms)
  echo "now_ms:   $_l"
  echo "log:      $LOG"
  tail -n 6 "$LOG" 2>/dev/null | sed 's/^/  /'
}

do_stop(){
  _p=$(me)
  if [ -z "${_p:-}" ] || ! kill -0 "$_p" 2>/dev/null; then
    echo "pathb_daemon: not running"
    rm -f "$PIDF"
    return 0
  fi
  kill "$_p" 2>/dev/null
  _i=0
  while [ "$_i" -lt 40 ]; do
    kill -0 "$_p" 2>/dev/null || break
    nap_ms 100; _i=$((_i+1))
  done
  kill -9 "$_p" 2>/dev/null
  rm -f "$PIDF"
  echo "pathb_daemon: stopped"
}

case "${1:-}" in
  run)    do_run ;;
  once)   STARTED=$(now_ms); ST_SINCE=$STARTED; prepare_tick || true; do_tick ;;
  status) do_status ;;
  stop)   do_stop ;;
  tick)   do_tick ;;
  *) echo "usage: $0 {run|once|status|stop}"; exit 2 ;;
esac
