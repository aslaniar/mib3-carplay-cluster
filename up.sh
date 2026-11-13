#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# ============================================================================
# up.sh -- ONE command that brings the CarPlay cluster lane up.
#
#   REQUIRES ROOT.  Run it from a root shell on the head unit:
#
#       sh up.sh
#
# This script does not obtain root and this repository does not describe how.
# It expects an already-privileged shell; everything below assumes it.
#
# What it does, in order:
#   1. stages the kit from THIS directory into /tmp/kit, checking every file
#   2. verifies the pinned builds (md5) -- an unidentified binary is refused
#   3. injects the shim into the LIVE carplay (ptrace + GOT patch, no restart)
#   4. starts the supervisor, which takes the cluster when the phone streams
#
# Safe to run repeatedly.  It never restarts carplay, never reboots, and writes
# nothing outside /tmp and /dev/shm.  If a pin does not match it stops that step
# and says so -- it does not deploy a build it cannot identify.
#
# The shim only activates while <drive>/mib3shim.enable exists, and while it does
# the shim appends breadcrumbs to <drive>/mib3shim.bc.  If injection or the
# supervisor fails, up.sh says NOT READY and exits 1.
#
# Undo:  sh /tmp/kit/pathbctl release      (hand the cluster back to the car)
#        sh /tmp/kit/pathbctl stop         (stop the supervisor)
# A reboot clears everything on the unit.
# ============================================================================
set -u

# ---- the pinned builds (2026-11-02, field-verified on the car) -------------
SHIM_PIN=dc723befb0cd4c636ca2aa2a8f2c3743
SENDER_PIN=a9ab16d0bb8f29ed0ceda3f3ad69472d
WHEEL_PIN=e689515fc20a701f0e84d62d9dcd6251

KIT=/tmp/kit
LOG="$KIT/up.log"
HERE=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || HERE=""

now() { date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo '?'; }
say() { echo "$1" >&2; echo "[$(now)] $1" >>"$LOG" 2>/dev/null; return 0; }
step() { say "$*"; }
warn() { say "WARN: $*"; }
PROBLEMS=0
problem() { warn "$*"; PROBLEMS=$((PROBLEMS + 1)); }
die()  { say "STOP: $*"; exit 1; }

# ============================ 1. stage the kit =============================
[ "$(id -u)" = 0 ] || { echo "up.sh: must be run as root" >&2; exit 1; }
step "up.sh: root (uid=0), staging from ${HERE:-?}"

[ -n "$HERE" ] || die "cannot resolve my own directory"
mkdir -p "$KIT" || die "cannot create $KIT"

# src path in this tree -> flat name in /tmp/kit (the daemon expects flat)
STAGE="
bin/mib3shim.so:mib3shim.so
bin/mib3clusterstream:mib3clusterstream
bin/mib3wheel:mib3wheel
src/control/pathb_daemon.sh:pathb_daemon.sh
src/control/pathbctl:pathbctl
src/control/deliver_ctl.sh:deliver_ctl.sh
src/control/wheel_zoom.sh:wheel_zoom.sh
src/control/pathb/msport.py:msport.py
src/inject/run_inject.sh:run_inject.sh
src/inject/inject.py:inject.py
src/inject/inject_shim.py:inject_shim.py
src/inject/target_env.py:target_env.py
"

MISSING=""
for pair in $STAGE; do
  _src=${pair%%:*}; _dst=${pair##*:}
  if [ -f "$HERE/$_src" ]; then
    cp -f "$HERE/$_src" "$KIT/$_dst" || MISSING="$MISSING $_src"
  else
    MISSING="$MISSING $_src"
  fi
done
# the daemon looks for msport.py beside itself; keep a pathb/ copy too
[ -d "$KIT/pathb" ] || mkdir -p "$KIT/pathb" 2>/dev/null
[ -f "$KIT/msport.py" ] && cp -f "$KIT/msport.py" "$KIT/pathb/msport.py" 2>/dev/null

chmod 755 "$KIT"/*.sh "$KIT"/mib3shim.so "$KIT"/mib3clusterstream "$KIT"/mib3wheel 2>/dev/null
chmod 644 "$KIT"/*.py 2>/dev/null

[ -z "$MISSING" ] || die "not found in $HERE:$MISSING"
step "kit: staged into $KIT (nothing missing)"

# ============================ 2. verify the pins ===========================
md5of() { if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }
chk() { # chk <file-in-kit> <pin> <name>
  [ -f "$KIT/$1" ] || die "missing $1"
  _g=$(md5of "$KIT/$1" 2>/dev/null)
  [ "$_g" = "$2" ] || die "$3 md5 $_g != $2 -- refusing to deploy an unidentified build"
  step "pin: $3 ok ($_g)"
}
chk mib3shim.so       "$SHIM_PIN"   shim
chk mib3clusterstream "$SENDER_PIN" sender
chk mib3wheel         "$WHEEL_PIN"  wheel

# The shim's activation gate: without this file it loads but behaves stock.
MARKER=""
for _m in /tmp/USB/*/PART1/mib3shim.enable; do
  [ -f "$_m" ] && { MARKER=$_m; break; }
done
if [ -n "$MARKER" ]; then
  step "marker: $MARKER"
else
  warn "no mib3shim.enable on any USB drive -- the shim will stay stock until you add an empty file with that name to the root of the drive"
fi

# ================= 3. inject the shim into the LIVE carplay =================
CP=$(pidof carplay 2>/dev/null)
if [ -z "$CP" ]; then
  problem "carplay is not running -- skipping injection (start CarPlay, then re-run)"
else
  step "inject: carplay pid $CP"
  grep -q mib3shim "/proc/$CP/maps" 2>/dev/null && \
    step "inject: shim already mapped -- re-running the seam install anyway"
  if sh "$KIT/run_inject.sh" </dev/null >>"$LOG" 2>&1; then
    step "inject: seams installed"
  else
    problem "inject: run_inject.sh failed -- see $LOG"
  fi
fi

# ============================ 4. start the supervisor =======================
step "supervisor: starting (it takes the cluster when the phone streams)"
sh "$KIT/pathbctl" start </dev/null >>"$LOG" 2>&1 || problem "supervisor did not start -- see $LOG"

step "status:"
sh "$KIT/pathbctl" status </dev/null 2>&1 | while IFS= read -r l; do say "    $l"; done

step "watch:  tail -f /tmp/mib3shim.log   /   sh /tmp/kit/pathbctl status"
step "hand back any time:  sh /tmp/kit/pathbctl release"
if [ "$PROBLEMS" -gt 0 ]; then
  say "NOT READY: $PROBLEMS step(s) failed -- see the WARN lines above and $LOG"
  exit 1
fi
step "done. Next: connect the phone and start a route."
exit 0
