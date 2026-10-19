#!/bin/sh
# ============================================================================
# viewarea.sh -- the VIEW-AREA lane's one-stop driver (2026-10-19).
#
# WHAT IT IS FOR (plain language)
#   The cluster has a VIEW button that cycles its layout: one layout is a
#   full-panel map, another is two round gauges with the map in the middle.
#   When the driver picks the gauge layout, the CLUSTER ECU paints the gauges
#   over the video we send -- and the PHONE was never told its window shrank,
#   so Apple Maps keeps drawing as if it owned the whole panel and a large part
#   of it disappears behind the gauges.
#
#   This script is how we tell the phone.  It writes one line to the shim's
#   control inbox; the shim turns it into the CarPlay `updateViewArea` command,
#   which re-frames the cluster scene into a predeclared view area whose
#   `safeArea` is the region between the gauges.
#
# RUN ORDER ON THE CAR (one trip; RAM-only; no service is ever restarted)
#   sh viewarea.sh labels                 # SMACK both ways + label the inbox socket
#   sh viewarea.sh status                 # is the shim there? are the knobs on?
#   sh viewarea.sh idx                    # READ-ONLY: what did we declare, and what
#                                         #   does the car's own nav think its cluster
#                                         #   map region is?  (problem A probe)
#   sh viewarea.sh on                     # tell the operator which knobs to inject with
#   ... inject with MIB3_SHIM_VIEWAREAS=1, plug the phone, get the map on the cluster ...
#   sh viewarea.sh 1                      # switch to the gauge view area
#   sh viewarea.sh check                  # did the PHONE accept it? (status 0)
#   sh viewarea.sh next                   # cycle (wrapping over the declared count)
#   sh viewarea.sh 0                      # back to the full-panel area
#
# THE HONEST BIT (read the lane doc, section "problem A")
#   Nothing on the head unit was found that tells us the layout changed, so the
#   switch here is MANUAL.  `idx` is the evidence-gathering step: it prints the
#   nav engine's own cluster renderer geometry so that a future automation can be
#   built on a signal we have actually seen, not on a guess.
#
# SAFETY: read-only RSI in `idx`; everywhere else it writes ONE short line to an
# AF_UNIX socket in /dev/shm.  No service is restarted, nothing outside RAM is
# written, sink stays `helper`.  A reboot is the full undo.  Never pkill.
# ============================================================================
KIT="${MIB3_KIT:-/tmp/kit}"
CTL="$KIT/mib3ctl"
STICK="${MIB3_DLV_STICK:-/dev/shm}"          # the stick often remounts read-only
SOCK="${MIB3_SHIM_ZOOM_SOCK:-/dev/shm/mib3zoom.sock}"
SHIMLOG="${MIB3_SHIM_LOG:-/tmp/mib3shim.log}"
PORT="${MIB3_MR_PORT:-49500}"

say(){ echo "[viewarea] $*"; }

labels(){
  say "SMACK: both directions + socket label (an AF_UNIX connect is checked BOTH ways)"
  echo "common::lib carplay::app rwx" > /sys/fs/smackfs/load2 2>/dev/null && say "  rule common::lib -> carplay::app ok" || say "  [!] rule 1 failed"
  echo "carplay::app common::lib rwx" > /sys/fs/smackfs/load2 2>/dev/null && say "  rule carplay::app -> common::lib ok" || say "  [!] rule 2 failed"
  if [ -S "$SOCK" ]; then
    chsmack -a carplay::app "$SOCK" 2>/dev/null && say "  socket labelled carplay::app" || say "  [!] chsmack failed (socket exists)"
  else
    say "  socket $SOCK does not exist yet -- the shim creates it when the"
    say "  type-111 cluster stream is set up (i.e. once the phone connects)"
  fi
}

status(){
  say "shim log: ${SHIMLOG}"
  if [ -f "$SHIMLOG" ]; then
    say "knobs as the running shim reported them:"
    grep -m1 'knobs:' "$SHIMLOG" 2>/dev/null | sed 's/^/    /'
    say "viewAreas declared: $(grep -c 'cluster/va/rect' "$SHIMLOG" 2>/dev/null || echo 0) area line(s)"
    say "updateViewArea shipped: $(grep -c 'cluster/va/cmd: index shipped' "$SHIMLOG" 2>/dev/null || echo 0)"
    say "applied (phone status 0): $(grep -c 'cluster/va/applied' "$SHIMLOG" 2>/dev/null || echo 0)"
    say "refused (phone non-zero): $(grep -c 'cluster/va/refused' "$SHIMLOG" 2>/dev/null || echo 0)"
    say "skipped-superseded cmds:  $(grep -c 'cluster/cmd/skipped-' "$SHIMLOG" 2>/dev/null || echo 0)"
    say "last ack lines:"
    grep 'cluster/cmd/ack' "$SHIMLOG" 2>/dev/null | tail -4 | sed 's/^/    /'
  else
    say "[!] $SHIMLOG does not exist -- the shim is not injected (or /tmp was wiped by a reboot)"
  fi
  say "inbox socket: $(if [ -S "$SOCK" ]; then echo "present $SOCK"; else echo "absent"; fi)"
}

# READ-ONLY evidence: what is the car's own nav doing for the cluster?
idx(){
  say "=== what WE declared (from the shim log) ==="
  grep -E 'cluster/va/rect|count declared' "$SHIMLOG" 2>/dev/null | tail -12 | sed 's/^/    /'
  say "=== what the CAR'S OWN nav thinks (read-only RSI) ==="
  say "maprenderer is http://127.0.0.1:$PORT/maprenderer/renderers"
  if [ -x "$KIT/mib3wheel" ]; then
    "$KIT/mib3wheel" --probe --port "$PORT" 2>&1 | grep -E '^RENDERER|^PROBE' | sed 's/^/    /'
  else
    say "  (no $KIT/mib3wheel -- run push_kit.sh; or: python3 - <<'PY' ...)"
  fi
  say "READ THIS as problem (A): the 'kombi' renderer's viewPort is the region the"
  say "car's own nav draws its cluster map into.  If viewPort/zoomArea MOVE when the"
  say "VIEW button is pressed, that is a readable layout signal we can automate on."
  say "If they do not, the head unit is not being told -- and the switch stays manual."
}

on(){
  say "to arm the view-area path, re-inject the shim with these env knobs set"
  say "(target_env.py sets them live, between phone plugs -- no rebuild):"
  say "    MIB3_SHIM_VIEWAREAS=1        declare multiple view areas (default off)"
  say "    MIB3_SHIM_VA_COUNT=2         how many (default 2)"
  say "    MIB3_SHIM_VA1=0,0,1280,480,400,40,480,400   visible + safe rect for area 1"
  say "    MIB3_SHIM_VA_VISIBLE=0       keep the FULL visible frame (the rgi-proven shape)"
  say "    MIB3_SHIM_CMDACK=1           (default) make the phone's verdict visible"
  say "verify the shim log; the knob banner prints VIEWAREAS/VA_COUNT/VA_CMD."
}

check(){
  say "=== did the phone accept the switch? ==="
  grep -E 'cluster/va/cmd: index shipped|cluster/cmd/ack|cluster/va/applied|cluster/va/refused' \
       "$SHIMLOG" 2>/dev/null | tail -16 | sed 's/^/    /'
  if grep -q 'cluster/va/applied' "$SHIMLOG" 2>/dev/null; then
    say "RESULT: the phone reported status 0 -> the view area was APPLIED."
    say "        If the map still runs under the gauges, the safeArea NUMBER is wrong,"
    say "        not the mechanism: re-run the calibration (see the lane doc, problem B)."
  elif grep -q 'cluster/va/refused' "$SHIMLOG" 2>/dev/null; then
    say "RESULT: the phone REFUSED it (non-zero status).  Read the status above."
    say "        A refusal usually means the index is out of range for what /info"
    say "        declared -- check that VIEWAREAS=1 was on at INJECT time (the advert"
    say "        is built once per session) and re-read the knobs banner."
  elif grep -q 'cluster/va/cmd: index shipped' "$SHIMLOG" 2>/dev/null; then
    say "RESULT: shipped, but no completion came back.  The session may have been"
    say "        superseded (grep cluster/cmd/ack/stale) or the phone never answered."
  else
    say "RESULT: nothing was shipped.  Run 'status': the usual causes are the knob"
    say "        off (cluster/va/cmd: skipped ...), no cluster uuid yet, or the"
    say "        socket not being written (did the tool exit non-zero?)."
  fi
}

if [ ! -x "$CTL" ]; then
  CTL="${MIB3_KIT:-/tmp/kit}/mib3ctl"
fi

CMD="${1:-status}"
case "$CMD" in
  labels|status|idx|on|check) "$CMD" ;;
  view|next|prev|zoom|appear|line)
    [ -x "$CTL" ] || { say "missing $CTL -- run push_kit.sh"; exit 1; }
    shift
    case "$CMD" in
      view) exec "$CTL" --socket "$SOCK" view "$@" ;;
      *)    exec "$CTL" --socket "$SOCK" "$CMD" "$@" ;;
    esac ;;
  0|1|2|3|4|5|6|7)
    [ -x "$CTL" ] || { say "missing $CTL -- run push_kit.sh"; exit 1; }
    exec "$CTL" --socket "$SOCK" view "$CMD" ;;
  *)
    cat <<EOF
usage: sh viewarea.sh {labels|status|idx|on|check|<index>|next|prev|zoom in|out|appear map|ui M S}
  labels   SMACK rules both ways + chsmack the inbox socket
  status   shim knobs, how many areas were declared, what has been applied/refused
  idx      READ-ONLY: our declared areas + the car's own nav cluster geometry (problem A)
  on       the exact env knobs to inject with (does not change anything itself)
  <index>  switch to that view-area index (0 = full panel)
  next|prev  step the index, wrapping over the declared count
  zoom in|out   mirror a steering-wheel zoom step
  appear map|ui <mode> <setting>   per-surface appearance
  check    the acceptance check: did the phone report status 0?
EOF
    exit 2 ;;
esac
