#!/bin/sh
# ============================================================================
# patha_auto.sh -- PATH A, AUTONOMOUS: arm the delivery the moment CarPlay starts
#                  feeding cluster frames.  Runs ON THE UNIT.  No laptop needed.
#
# WHY: the operator cannot have the phone in CarPlay AND the laptop tethered, so the
# switch must not depend on anyone being connected.  This watcher detects the trigger
# the operator described -- a route started in APPLE MAPS OVER CARPLAY, i.e. the phone
# beginning to send the type-111 cluster stream -- and performs the switch itself.
#
# 2026-10-16 HARDENING (LANE_CLUSTER_DELIVERY_IMPLEMENT_2026-10-16.md):
#   * LOCKFILE -- duplicate instances raced and produced {"code":400,"Invalid JSON"}
#     when a raced GET returned an empty URI.  We now take a mkdir lock (the same one
#     deliver_ctl.sh uses), so two instances can never both apply.
#   * NO BLIND POSTS -- the switch itself is delegated to deliver_ctl.sh, which
#     validates every URI (non-empty + /displaymanagement/*) before POSTing and is
#     idempotent, so a re-fire is a no-op rather than a corrupting double-create.
#   * RESTORE-ON-EXIT -- if the watcher is removed or times out AFTER flipping, it
#     restores the car's nav element so the cluster is never left pointed at a dead
#     surface.  (A reboot is still the full undo; the helper/presenter keep running
#     until you kill them.)
#
# EVERYTHING IS LOGGED TO THE STICK: a fault in carplay/multi-display is
# FailureAction=reboot-immediate and wipes /dev/shm, so the stick is the only place a
# post-mortem survives.  This script never touches a service.  RAM-only otherwise.
#
# USAGE (on the unit, in the background):
#   nohup sh patha_auto.sh [minutes] > /dev/null 2>&1 &
# ============================================================================
set -u
HERE="$(dirname "$0")"
CTL="$HERE/deliver_ctl.sh"
[ -f "$CTL" ] || CTL=/tmp/kit/deliver_ctl.sh

# Overridable for the desk mock (harness/run_delivery.sh); defaults are the unit's.
HELPERLOG="${MIB3_DLV_HELPERLOG:-/dev/shm/mib3helper.log}"
HELPERSOCK="${MIB3_DLV_HELPERSOCK:-/dev/shm/mib3cluster.sock}"
if [ -n "${MIB3_DLV_STICK:-}" ]; then
  STICK="$MIB3_DLV_STICK"
else
  for d in /tmp/USB/PORT1 /tmp/USB/PORT2 /tmp/USB/PORT3 /tmp/USB/PORT4; do
    [ -d "$d/PART1" ] && { STICK="$d/PART1"; break; }
  done
fi
STICK="${STICK:-${MIB3_DLV_RAM:-/dev/shm}}"
mkdir -p "$STICK" 2>/dev/null
LOG="$STICK/patha_auto.log"
LOCK="$STICK/patha_auto.lock"

have() { command -v "$1" >/dev/null 2>&1; }
pids_of() { if have pidof; then pidof "$1" 2>/dev/null; elif have pgrep; then pgrep -x "$1" 2>/dev/null; fi; }
say() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*" >> "$LOG"; sync 2>/dev/null; }
frames_now() { grep -ao 'frames=[0-9x]*\|frames total [0-9]*' "$HELPERLOG" 2>/dev/null | tail -n1; }

# --- single-instance lock (mkdir is atomic on busybox) --------------------------
if ! mkdir "$LOCK" 2>/dev/null; then
  _p="$(cat "$LOCK/pid" 2>/dev/null)"
  if [ -n "$_p" ] && kill -0 "$_p" 2>/dev/null; then
    say "duplicate instance detected (pid $_p already watching) -- exiting"
    echo "patha_auto already running (pid $_p)"; exit 0
  fi
  rm -rf "$LOCK"; mkdir "$LOCK" 2>/dev/null || { echo "cannot take lock $LOCK"; exit 1; }
fi
echo $$ > "$LOCK/pid"
FLIPPED=0
release() { [ "$(cat "$LOCK/pid" 2>/dev/null)" = "$$" ] && rm -rf "$LOCK" 2>/dev/null; }
trap 'if [ "$FLIPPED" = "1" ]; then say "exit: restoring the car nav element"; sh "$CTL" restore >>"$LOG" 2>&1; fi; release' EXIT INT TERM

say "start (stick=$STICK pid=$$); waiting for CarPlay frames"
[ -S "$HELPERSOCK" ] || say "WARN: helper socket absent ($HELPERSOCK) -- is the helper running?"

LAST=""
N="${1:-40}"          # minutes
i=0
while [ "$i" -lt $((N*12)) ]; do
  F="$(frames_now)"
  if [ "$FLIPPED" = "0" ]; then
    # TRIGGER: frames are being received = CarPlay is feeding the cluster stream.
    # Accept BOTH formats: the Cinemo helper logs hex ("frames=0x5"), our
    # v4l2_seq decoder logs DECIMAL to /tmp/dec.log ("frames=7" in its STATUS
    # lines; that counter only advances on REAL decoded frames).  Point
    # MIB3_DLV_HELPERLOG at whichever log your architecture writes.
    case "$F" in
      frames=0x1|frames=0x[1-9a-f]*|frames=[1-9]*|"frames total "*) ;;
      *) sleep 5; i=$((i+1)); continue ;;
    esac
    say "TRIGGER: frames arriving ($F) -- performing the switch via deliver_ctl.sh"
    # deliver_ctl validates its own URIs and takes its own lock; if it aborts we do NOT
    # mark flipped, so the next tick can retry cleanly.
    if sh "$CTL" apply >>"$LOG" 2>&1; then
      FLIPPED=1
      say "SWITCH APPLIED. NOW WATCH THE CLUSTER."
      say "  if it does not change: sh $CTL restore   (or: sh patha_deliver.sh toggle)"
    else
      say "apply FAILED (see deliver_ctl output above); will retry next tick"
    fi
  else
    if [ "$F" != "$LAST" ]; then
      S="$(ls -l /proc/"$(pids_of multi-screen-server)"/fd 2>/dev/null | grep -cE 'video|ion|dri')"
      say "frames=$F  msf_fds=$S"
      LAST="$F"
    fi
  fi
  [ $((i % 12)) -eq 0 ] && say "alive (i=$i, frames=${F:-none}, flipped=$FLIPPED)"
  sleep 5; i=$((i+1))
done
say "exit (flipped=$FLIPPED)"
exit 0
