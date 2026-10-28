#!/bin/sh
# ============================================================================
# night_watch.sh -- the DAY/NIGHT watcher's one-stop driver (2026-10-26).
#
# WHAT IT DOES (plain language)
#   The centre CarPlay screen already follows the car's day/night theme: the
#   STOCK carplay binary subscribes the unit's own hmiintegration dayNight
#   element (:49570) and calls SetNightMode on its framework.  The CLUSTER map
#   does not -- our shim advertises a static nightMode and nobody mirrors the
#   car's theme to the cluster surface.  mib3night polls the SAME element the
#   stock binary reads and, on every day/night change, writes ONE line to the
#   shim's control inbox ('a map <mode> 0'), which the shim ships to the phone
#   as mapAppearanceUpdate.  No shim rebuild, no re-injection, no service.
#
# RUN ORDER ON THE CAR (one trip; RAM-only; nothing is restarted)
#   sh night_watch.sh probe      # READ-ONLY evidence: does :49570 answer, and
#                                # is there an element named "nightmode"?
#   sh night_watch.sh start      # start the watcher (background, /dev/shm log)
#   ... drive with the lights on/off; the cluster map should follow ...
#   sh night_watch.sh check      # did the shim ship mapAppearanceUpdate?
#   sh night_watch.sh stop      # stop the watcher
#
# THE ONE-SHOT FALLBACK TEST (before any watcher is started): with the car in
# NIGHT, press the button by hand and watch the cluster map:
#     /tmp/kit/mib3ctl a map 1 0
# If the map darkens, the whole chain (inbox -> appearance pump -> phone) works
# and mode 1 == night.  If it does NOT, try 'a map 2 0' (the appearanceMode
# numbering is CarKit's, not documented in the rgi reference -- one keypress
# settles it) and run the watcher with --night-mode <that number>.
#
# SAFETY: read-only HTTP GET on localhost + one short line to an AF_UNIX socket
# in /dev/shm.  No RSI mutation, no display call, no service.  A reboot is the
# full undo; killing the tool stops the mirroring instantly.
# ============================================================================
KIT="${MIB3_KIT:-/tmp/kit}"
NIGHT="$KIT/mib3night"
STICK="${MIB3_DLV_STICK:-/dev/shm}"
LOG="$STICK/night.log"
PIDFILE="/dev/shm/night.pid"
SOCK="${MIB3_SHIM_ZOOM_SOCK:-/dev/shm/mib3zoom.sock}"
SHIMLOG="${MIB3_SHIM_LOG:-/tmp/mib3shim.log}"
PORT="${MIB3_NIGHT_PORT:-49570}"
POLL="${MIB3_NIGHT_POLL_MS:-1000}"
TARGET="${MIB3_NIGHT_TARGET:-map}"

say(){ echo "[night] $*"; }

probe(){
  [ -x "$NIGHT" ] || { say "missing $NIGHT -- run push_kit.sh"; exit 1; }
  say "=== probe (read-only) ==="
  "$NIGHT" --probe --port "$PORT" 2>&1
  say "=== reading this ==="
  say "  want: status=200 and a body with an element named 'nightmode'."
  say "  night <=> that element is IN the collection (the rule the STOCK carplay"
  say "  binary implements for the centre screen -- corpus carplay)."
  say "  status=0: wrong port?  49570 is hmiintegration; 49506 is a DIFFERENT"
  say "  service (phonemecontentnetwork) -- do not use it."
}

start(){
  [ -x "$NIGHT" ] || { say "missing $NIGHT -- run push_kit.sh"; exit 1; }
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    say "already running (pid $(cat "$PIDFILE"))"; return 0
  fi
  say "starting watcher -> $LOG (poll ${POLL}ms, target '$TARGET')"
  : > "$LOG"
  "$NIGHT" --port "$PORT" --poll-ms "$POLL" --target "$TARGET" --socket "$SOCK" \
      >> "$LOG" 2>&1 &
  echo $! > "$PIDFILE"
  sleep 1
  if kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    say "running (pid $(cat "$PIDFILE")).  head -6 $LOG"
    head -6 "$LOG" 2>/dev/null | sed 's/^/    /'
  else
    say "[!] it died immediately; log:"; sed -n 1,12p "$LOG" 2>/dev/null | sed 's/^/    /'
  fi
}

stop(){
  if [ -f "$PIDFILE" ]; then
    P=$(cat "$PIDFILE")
    kill "$P" 2>/dev/null && say "stopped pid $P" || say "pid $P not running"
    rm -f "$PIDFILE"
  else
    say "no pidfile ($PIDFILE)"
  fi
}

status(){
  say "watcher: $(if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then echo "running pid $(cat "$PIDFILE")"; else echo "not running"; fi)"
  say "inbox socket: $(if [ -S "$SOCK" ]; then echo "present $SOCK"; else echo "absent (appears at the type-111 stream SETUP)"; fi)"
  say "shim appearance lines: $(grep -c 'cluster/appear' "$SHIMLOG" 2>/dev/null || echo 0)"
  say "last events:"; tail -6 "$LOG" 2>/dev/null | sed 's/^/    /'
}

check(){
  say "=== did the phone get the appearance command? ==="
  grep -a "cluster/appear" "$SHIMLOG" 2>/dev/null | tail -n 8 | sed 's/^/    /'
  say "  want: 'target (0=ui 1=map)'=1 and 'appearanceMode' following the car."
  say "  acks arrive as 'cluster/cmd/ack/type: mapAppearanceUpdate'."
}

case "${1:-}" in
  probe)  probe ;;
  start)  start ;;
  stop)   stop ;;
  status) status ;;
  check)  check ;;
  *) echo "usage: sh $0 {probe|start|stop|status|check}" >&2; exit 2 ;;
esac
