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
# Undo:  sh /tmp/kit/pathbctl release      (hand the cluster back to the car)
#        sh /tmp/kit/pathbctl stop         (stop the supervisor)
# A reboot also clears everything: nothing here is persistent.
# ============================================================================
set -u

# ---- the pinned builds (field-verified on the car) -------------------------
# The shim is the build with the cluster nav-text gate (the address and
# arrival-time bands) and the tuned safe area; both are field-verified.
SHIM_PIN=c789666fa9fccfcad45a5607e5785482
SENDER_PIN=a9ab16d0bb8f29ed0ceda3f3ad69472d
WHEEL_PIN=e689515fc20a701f0e84d62d9dcd6251

KIT=/tmp/kit
LOG="$KIT/up.log"
HERE=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || HERE=""

now() { date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo '?'; }
say() { echo "$1" >&2; echo "[$(now)] $1" >>"$LOG" 2>/dev/null; return 0; }
step() { say "$*"; }
warn() { say "WARN: $*"; }
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

# ================= 3. inject the shim into the LIVE carplay =================
CP=$(pidof carplay 2>/dev/null)
if [ -z "$CP" ]; then
  warn "carplay is not running -- skipping injection (start CarPlay, then re-run)"
else
  step "inject: carplay pid $CP"
  grep -q mib3shim "/proc/$CP/maps" 2>/dev/null && \
    step "inject: shim already mapped -- re-running the seam install anyway"
  if sh "$KIT/run_inject.sh" </dev/null >>"$LOG" 2>&1; then
    step "inject: seams installed"
  else
    warn "inject: run_inject.sh failed -- see $LOG"
  fi
fi

# ============== 3b. set the shim's knobs BEFORE the phone connects ==========
# The daemon re-asserts these at each session's prepare, but /info is built when
# the phone connects, which is before prepare, so a knob set only by the daemon
# would miss the first session after a reboot: the advert would carry the old
# full-panel safe area and the nav-text gate would still be armed but off.  Set
# them here, right after the injection and before a plug, so the first session
# is correct.  This is the same list as knobs_once in
# src/control/pathb_daemon.sh and the two must stay in step
# (tests/run_navtext.sh N12 checks that they are).
knobs() {
  _cp=$(pidof carplay 2>/dev/null | awk '{print $1}')
  [ -n "${_cp:-}" ] || { warn "knobs: carplay is not running -- the daemon will set them at prepare"; return 0; }
  [ -f "$KIT/target_env.py" ] || { warn "knobs: target_env.py missing -- skipping"; return 0; }
  _n=0
  for _kv in MIB3_SHIM_SINK=helper \
             MIB3_SHIM_FPS=60 \
             MIB3_SHIM_NAVTEXT=filter \
             MIB3_SHIM_VIEWAREAS=1 \
             MIB3_SHIM_VA_COUNT=4 \
             MIB3_SHIM_VA_VISIBLE=0 \
             MIB3_SHIM_VA_NOOUTSIDE=1 \
             MIB3_SHIM_VA_INITIAL=2 \
             "MIB3_SHIM_VA0=0,0,1280,480,0,0,1280,480" \
             "MIB3_SHIM_VA1=0,0,1280,480,0,0,1280,440" \
             "MIB3_SHIM_VA2=0,0,1280,480,0,0,1280,400" \
             "MIB3_SHIM_VA3=0,0,1280,480,0,0,1280,360" \
             "MIB3_SHIM_URL=maps:/car/instrumentcluster/map?showETA=no&maneuverLayout=topAligned&showSpeedLimit=no" \
             MIB3_SHIM_CMDACK=1; do
    /usr/bin/python "$KIT/target_env.py" "$_cp" "$_kv" >/dev/null 2>&1 && _n=$((_n+1))
  done
  step "knobs: $_n set on carplay pid $_cp (nav gate, safe area, fps/sink/url)"
}
knobs

# ============================ 4. start the supervisor =======================
step "supervisor: starting (it takes the cluster when the phone streams)"
sh "$KIT/pathbctl" start </dev/null >>"$LOG" 2>&1 || warn "supervisor did not start -- see $LOG"

step "status:"
sh "$KIT/pathbctl" status </dev/null 2>&1 | while IFS= read -r l; do say "    $l"; done

step "done. Next: connect the phone and start a route."
step "watch:  tail -f /tmp/mib3shim.log   /   sh /tmp/kit/pathbctl status"
step "hand back any time:  sh /tmp/kit/pathbctl release"
exit 0
