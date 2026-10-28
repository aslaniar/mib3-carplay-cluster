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

# PRE-FLIGHT: the generation pins (2026-10-26, LANE_GEN_FIX_2026-10-26.md --
# the ONE coherent generation: shim + decoder + presenter + watchers + kit).
# Abort BEFORE staging if a tool does not match, so a stale decoder/presenter
# cannot reach the car (the same discipline run_inject.sh applies to the shim).
md5of(){ if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }
SHIM_WANT=13e0456e9e5f9ceb6d07e9cd9978cb4f
V4L2_WANT=250466e1d00421b7136c192cc3878d47
EGL_WANT=bd20b5910507e3876471cb922c9c6556
NIGHT_WANT=acaf614dbb7211963dcbca1bea7013d2
# Path B sender (2026-10-27, second session).  CORRECTED: this used to pin
# data/PATHB_20261026/mib3clusterstream.unit at 6b668509 -- which is the *run 1*
# build of the field session, the one that put the sequence header on EVERY frame
# and left the cluster on its pre-render grid.  The comment above it forbade
# exactly that binary while the pin deployed it.  The truth is now the fresh
# build in tools/, pinned here, and `pathb_ctl.sh` finds it at /tmp/kit/<name>.
PATHB_WANT=fc923b446505c83ae23ad2bccd5a0513
PATHB_BIN=tools/mib3clusterstream
# the synthetic rate-test file (field plan step 0): the 10 fps pattern that is
# known to render, played back at a chosen rate with `pathb_ctl.sh play`.
PATHB_FIX=../data/PATHB_20261026/pathb_base.264
PATHB_FIX_WANT=a2a78e2ede7d5d9b8d3392625926c75a
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
checkpin tools/mib3night    "$NIGHT_WANT"
checkpin "$PATHB_BIN"       "$PATHB_WANT"
echo "[i] NOTE: this generation was rebuilt by the 2026-10-26 cleanup and has NOT run on the car yet."
echo "[i]       Validate once (bring_up + one guidance session) before treating it as field-proven."
echo "[i] pre-flight pins OK: shim $SHIM_WANT / v4l2_seq $V4L2_WANT / mib3egl $EGL_WANT / mib3night $NIGHT_WANT / pathb $PATHB_WANT"

ssh "$HOST" 'mkdir -p /tmp/kit'

FILES="mib3shim.so mib3clusterhelper \
  inject/inject.py inject/inject_shim.py inject/target_env.py inject/run_inject.sh \
  live_kit/deliver_ctl.sh live_kit/chain_up.sh live_kit/chain_down.sh \
  live_kit/transition_ctl.sh \
  live_kit/v4l2_run.sh live_kit/v4l2_sweep.sh \
  live_kit/start_mss_log.sh live_kit/patha_deliver.sh live_kit/patha_auto.sh \
  live_kit/start_cluster_helper.sh live_kit/probe.sh \
  live_kit/wheel_zoom.sh live_kit/night_watch.sh live_kit/usb_diag.sh live_kit/viewarea.sh \
  live_kit/calib.sh tools/calib_grid.py live_kit/bring_up.sh \
  live_kit/fps_probe.sh live_kit/res_probe.sh live_kit/stream_fps.sh \
  tools/v4l2_seq tools/mib3egl tools/ausock tools/socktest tools/rgba_feed \
  tools/mib3wheel tools/mib3night tools/mib3ctl \
  live_kit/pathb_ctl.sh live_kit/pathb/msport.py live_kit/pathb/fake_shim.py \
  live_kit/pathb/sniff.py tools/pathb_report.py tools/pes_frames.py \
  tools/pes_extract.py tools/au_boundary.py tools/sps_dump.py tools/wire_diff.py \
  $PATHB_FIX"

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
# never freezes on a stale CarPlay map.
# FROM 2026-10-20 `bring_up.sh` STARTS IT FOR YOU (supervised mode is the default),
# so the operator never types the line below by hand:
#   sh /tmp/kit/bring_up.sh          -> knobs, sysctl, SMACK, presenter, tap, controller
#   sh /tmp/kit/bring_up.sh status   -> processes, view areas, stream, controller state
#   sh /tmp/kit/bring_up.sh manual   -> the old unsupervised flow (delivery inline)
#   sh /tmp/kit/bring_up.sh down     -> stop OUR processes only
# The three measurement tools shipped alongside it (HANDOFF_2026-10-20 S2):
#   fps_probe.sh   which of the three frame-rate limits is binding (20 s, read-only)
#   res_probe.sh   the 2x resolution experiment (show | two)
#   stream_fps.sh  the car's OWN encoder rate (show | set N | restore) -- RSI write, read its header

for f in $FILES; do
  [ -f "$f" ] || { echo "[!] missing $f -- run sh build.sh and sh tools/build.sh first"; exit 1; }
  cat "$f" | ssh "$HOST" "cat > /tmp/kit/$(basename "$f")"
done

# a second copy of the shim under a DIFFERENT name: the only way to UPGRADE the
# shim in an already-running carplay (a mapped .so cannot be replaced in place).
cat mib3shim.so | ssh "$HOST" 'cat > /tmp/kit/mib3shim2.so'

# THE SENDER MUST LAND UNDER THE NAME pathb_ctl.sh LOOKS FOR.  `for f in $FILES`
# stages each file under its BASENAME, so a sender carried as
# data/PATHB_20261026/mib3clusterstream.unit arrived as
# /tmp/kit/mib3clusterstream.unit while the controller defaults its MUXER to
# /tmp/kit/mib3clusterstream -- i.e. "sender not executable" on the car, for a
# file that was sitting right there.  Stage it explicitly under the right name.
cat tools/mib3clusterstream | ssh "$HOST" 'cat > /tmp/kit/mib3clusterstream'

ssh "$HOST" 'chmod +x /tmp/kit/*.sh /tmp/kit/*.py /tmp/kit/mib3shim.so /tmp/kit/mib3shim2.so /tmp/kit/v4l2_seq /tmp/kit/mib3egl /tmp/kit/ausock /tmp/kit/socktest /tmp/kit/rgba_feed /tmp/kit/mib3clusterhelper /tmp/kit/mib3wheel /tmp/kit/mib3ctl /tmp/kit/mib3night /tmp/kit/mib3clusterstream 2>/dev/null; echo "--- /tmp/kit ---"; ls -la /tmp/kit'
