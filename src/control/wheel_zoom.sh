#!/bin/sh
# ============================================================================
# wheel_zoom.sh -- the STEERING-WHEEL zoom lane's one-stop driver (2026-10-18).
#
# WHAT IT DOES (plain language)
#   The wheel's up/down does not reach CarPlay.  It reaches the CAR'S OWN nav,
#   which zooms its map and tells the cluster to paint a zoom/scale overlay --
#   a map nobody is looking at any more, because the cluster now shows our
#   CarPlay stream.  This script starts the watcher that notices each wheel
#   detent on the unit's own renderer interface and hands it to the shim, which
#   tells the PHONE to zoom its cluster map (changeMapZoomLevel).
#
# RUN ORDER ON THE CAR (one trip; RAM-only; nothing is restarted)
#   sh wheel_zoom.sh labels      # SMACK both ways + label the inbox socket
#   sh wheel_zoom.sh probe       # READ-ONLY: what does the maprenderer expose?
#   sh wheel_zoom.sh start       # start the watcher (background, stick log)
#   ... plug the phone, open Maps on the cluster, then roll the wheel ...
#   sh wheel_zoom.sh check       # did the shim actually send the command?
#   sh wheel_zoom.sh stop        # stop the watcher
#
# WHY `probe` COMES FIRST
#   The zoom COMMAND is pinned down by the reference implementation (rgi) and the
#   bench; what is NOT proved from the desk is WHICH interface carries the wheel's
#   step.  navsd posts {relativeSteps} to the renderer's discreteInput (the shipped
#   code says so) but only the car can confirm it, so `probe` prints the raw
#   renderers/discreteInputs and the answer is evidence, not opinion.  If `check`
#   shows no `cluster/zoom` lines, run `watch` (logs every event) and read the file.
#
# SAFETY: read-only RSI (GET + WebSocket subscribe).  No service is restarted,
# no display call is made, nothing is written outside /dev/shm and the stick.
# A reboot is the full undo.  This script never uses pkill (it matches the
# caller's own command line); it kills by pidfile.
# ============================================================================
KIT="${MIB3_KIT:-/tmp/kit}"
WHEEL="$KIT/mib3wheel"
STICK="${MIB3_DLV_STICK:-/dev/shm}"          # the stick often remounts read-only
LOG="$STICK/wheel.log"
PIDFILE="/dev/shm/wheel.pid"
SOCK="${MIB3_SHIM_ZOOM_SOCK:-/dev/shm/mib3zoom.sock}"
SHIMLOG="${MIB3_SHIM_LOG:-/tmp/mib3shim.log}"
PORT="${MIB3_MR_PORT:-49500}"

say(){ echo "[wheel] $*"; }

labels(){
  # The shim (label carplay::app) creates the inbox socket; our watcher (root)
  # connects to it.  An AF_UNIX connect is checked BOTH ways on this image, so
  # both rules are needed -- without them the shim logs `bind failed` or the
  # watcher logs `inbox.connect errno=13`.
  say "SMACK: both directions + socket label"
  echo "common::lib carplay::app rwx" > /sys/fs/smackfs/load2 2>/dev/null && say "  rule common::lib -> carplay::app ok" || say "  [!] rule 1 failed"
  echo "carplay::app common::lib rwx" > /sys/fs/smackfs/load2 2>/dev/null && say "  rule carplay::app -> common::lib ok" || say "  [!] rule 2 failed"
  if [ -S "$SOCK" ]; then
    chsmack -a carplay::app "$SOCK" 2>/dev/null && say "  socket labelled carplay::app" || say "  [!] chsmack failed (socket exists)"
  else
    say "  socket $SOCK does not exist yet -- it appears when the shim sees the cluster stream"
  fi
}

probe(){
  [ -x "$WHEEL" ] || { say "missing $WHEEL -- run push_kit.sh"; exit 1; }
  say "=== probe (read-only) ==="
  "$WHEEL" --probe --probe-all --port "$PORT" 2>&1
  say "=== reading this ==="
  say "  RENDERER name=... zoomMode=auto|manual distancePerPixel_milli=..."
  say "  zoomMode=auto means the map re-scales with vehicle speed: the backstop"
  say "  tap ignores those changes on purpose.  The authoritative tap is"
  say "  /maprenderer/discreteInputs (navsd's relativeSteps)."
}

start(){
  [ -x "$WHEEL" ] || { say "missing $WHEEL -- run push_kit.sh"; exit 1; }
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    say "already running (pid $(cat "$PIDFILE"))"; return 0
  fi
  say "starting watcher -> $LOG"
  : > "$LOG"
  "$WHEEL" --port "$PORT" --socket "$SOCK" --dump >> "$LOG" 2>&1 &
  echo $! > "$PIDFILE"
  sleep 1
  if kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    say "running (pid $(cat "$PIDFILE")).  head -20 $LOG"
    head -20 "$LOG" 2>/dev/null | sed 's/^/    /'
  else
    say "[!] it died immediately; log:"; sed -n 1,20p "$LOG" 2>/dev/null | sed 's/^/    /'
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
  say "inbox socket: $(if [ -S "$SOCK" ]; then echo "present $SOCK"; else echo "absent"; fi)"
  say "shim zoom lines: $(grep -c 'cluster/zoom' "$SHIMLOG" 2>/dev/null || echo 0)"
  say "  (0 is normal until the phone negotiates the cluster stream: the shim"
  say "   creates the inbox at the type-111 stream SETUP, not before)"
  say "last events:"; tail -6 "$LOG" 2>/dev/null | sed 's/^/    /'
}

check(){
  say "=== did the phone get the zoom command? ==="
  N=$(grep -c 'cluster/zoom' "$SHIMLOG" 2>/dev/null || echo 0)
  say "cluster/zoom lines in $SHIMLOG: $N"
  grep 'cluster/zoom' "$SHIMLOG" 2>/dev/null | tail -12 | sed 's/^/    /'
  if [ "$N" -gt 0 ] && grep -q 'changeMapZoomLevel rc=0x0' "$SHIMLOG" 2>/dev/null; then
    say "RESULT: the shim shipped changeMapZoomLevel and the SDK accepted it (rc=0)."
    say "        If the cluster map still does not zoom, the phone/app is"
    say "        ignoring the semantic zoom (see the lane doc, fallbacks)."
  elif [ "$N" -gt 0 ]; then
    say "RESULT: steps arrived but the send did not report rc=0 -- read the lines above."
  else
    say "RESULT: no step reached the shim."
    say "        a) is the watcher running and did IT see events?  -> tail $LOG"
    say "        b) if it saw none, the wheel does not post relativeSteps on this"
    say "           build: run: $WHEEL --port $PORT --dump --no-renderers"
    say "           and paste the EV lines (that names the real tap)."
    say "        c) if it saw events but the shim logged nothing, the shim was"
    say "           injected without the zoom path (rebuild/re-inject) or the"
    say "           socket label is wrong (sh wheel_zoom.sh labels)."
  fi
}

CMD="${1:-status}"
case "$CMD" in
  labels|probe|start|stop|status|check) "$CMD" ;;
  *) cat <<EOF
usage: sh wheel_zoom.sh {labels|probe|start|stop|status|check}
  labels  SMACK rules both ways + chsmack the inbox socket
  probe   READ-ONLY: GET the unit's maprenderer renderers + discreteInputs
  start   start the watcher (background; logs to \$MIB3_DLV_STICK/wheel.log)
  stop    stop it (by pidfile -- never pkill)
  status  is it running, is the socket there, how many steps the shim shipped
  check   the acceptance check after rolling the wheel
EOF
  exit 2 ;;
esac
