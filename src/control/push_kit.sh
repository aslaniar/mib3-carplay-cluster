#!/bin/sh
# ============================================================================
# push_kit.sh -- stage the whole live kit into the unit's /tmp/kit in one shot.
#
#   sh push_kit.sh [ssh-host]        (default host: mib3)
#
# Run from anywhere; it resolves the repo from its own location.  /tmp is wiped
# by every reboot, so re-run this after each one.  Nothing here is persistent.
#
# NOTE: real.264 / test.264 are NOT in the repo.  Generate a smoke-test stream
# with  python3 shim/tools/mk_h264.py 4 1280 480 > test.264  (I_PCM only), or
# bring a stick capture.
# ============================================================================
set -e
HOST="${1:-mib3}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SHIM="$(cd "$HERE/.." && pwd)"
cd "$SHIM" || exit 1

# PRE-FLIGHT: the recommended triad pins (2026-10-19, DEPLOY_READY_2026-10-19.md).
# Abort BEFORE staging if a tool does not match, so a stale decoder/presenter
# cannot reach the car (the same discipline run_inject.sh applies to the shim).
md5of(){ if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }
SHIM_WANT=c2e2804ac77e2755e1b99f664e487a6f
V4L2_WANT=bff15e2a4e89ecff1afc06d8bc0b928e
EGL_WANT=7612f74aecc1141c11e715488fb25dea
checkpin(){ # checkpin <path> <want-md5>
  local got; got="$(md5of "$1")"
  if [ "$got" != "$2" ]; then
    echo "[!] PRE-FLIGHT: $1 md5 $got != want $2 -- rebuild with: sh build.sh && sh tools/build.sh"
    exit 1
  fi
}
checkpin mib3shim.so        "$SHIM_WANT"
checkpin tools/v4l2_seq     "$V4L2_WANT"
checkpin tools/mib3egl      "$EGL_WANT"
echo "[i] pre-flight pins OK: shim $SHIM_WANT / v4l2_seq $V4L2_WANT / mib3egl $EGL_WANT"

ssh "$HOST" 'mkdir -p /tmp/kit'

FILES="mib3shim.so mib3clusterhelper \
  inject/inject.py inject/inject_shim.py inject/target_env.py inject/run_inject.sh \
  live_kit/deliver_ctl.sh live_kit/chain_up.sh live_kit/chain_down.sh \
  live_kit/transition_ctl.sh \
  live_kit/v4l2_run.sh live_kit/v4l2_sweep.sh \
  live_kit/start_mss_log.sh live_kit/patha_deliver.sh live_kit/patha_auto.sh \
  live_kit/start_cluster_helper.sh live_kit/probe.sh \
  live_kit/wheel_zoom.sh live_kit/usb_diag.sh live_kit/viewarea.sh \
  live_kit/calib.sh tools/calib_grid.py \
  tools/v4l2_seq tools/mib3egl tools/ausock tools/socktest tools/rgba_feed \
  tools/mib3wheel tools/mib3ctl"

# WHY the delivery scripts are listed (they were NOT, and it cost a whole trip
# on 2026-10-17): start_mss_log.sh is the ORACLE -- MS-S's own log on port 54321.
# Without it the operator cannot read MS-S's decisions at all and every delivery
# diagnosis is blind.  patha_deliver.sh carries the ECU-side lever; patha_auto.sh
# is the hands-free watcher that flips delivery when the phone's frames arrive
# (essential, because the phone cannot be in the car and tethered to the laptop
# at the same time).  DO NOT remove them from this list.
# See MIB3_RE/CLOSEOUT_2026-10-17_DECODE.md S4 and LANE_MS_S_SHARE_TRIGGER_2026-10-17.md.
#
# transition_ctl.sh is the LIFECYCLE controller (2026-10-18): the state machine
# that keeps the car's own map up until we have a real frame, and hands the
# cluster back (or shows a neutral frame) when the phone leaves, so the cluster
# never freezes on a stale CarPlay map.  It is the operator's one entry point:
#   nohup sh /tmp/kit/transition_ctl.sh run & 

for f in $FILES; do
  [ -f "$f" ] || { echo "[!] missing $f -- run sh build.sh and sh tools/build.sh first"; exit 1; }
  cat "$f" | ssh "$HOST" "cat > /tmp/kit/$(basename "$f")"
done

# a second copy of the shim under a DIFFERENT name: the only way to UPGRADE the
# shim in an already-running carplay (a mapped .so cannot be replaced in place).
cat mib3shim.so | ssh "$HOST" 'cat > /tmp/kit/mib3shim2.so'

ssh "$HOST" 'chmod +x /tmp/kit/*.sh /tmp/kit/*.py /tmp/kit/mib3shim.so /tmp/kit/mib3shim2.so /tmp/kit/v4l2_seq /tmp/kit/mib3egl /tmp/kit/ausock /tmp/kit/socktest /tmp/kit/rgba_feed /tmp/kit/mib3clusterhelper /tmp/kit/mib3wheel /tmp/kit/mib3ctl 2>/dev/null; echo "--- /tmp/kit ---"; ls -la /tmp/kit'
