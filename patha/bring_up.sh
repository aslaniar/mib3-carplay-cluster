#!/bin/sh
# ============================================================================
# bring_up.sh -- ONE command to bring the whole chain up, after push_kit+inject.
#
#   sh bring_up.sh            SUPERVISED (default): knobs, sysctl, SMACK, presenter,
#                             wheel tap, then hand the cluster to transition_ctl.sh
#   sh bring_up.sh chain      infrastructure only -- NO controller, NO delivery.
#                             For poking at things by hand; the cluster keeps
#                             showing the car's own content.
#   sh bring_up.sh manual     the OLD behaviour: start our decoder now and do the
#                             delivery handover inline, unsupervised.  Kept because
#                             it is the flow every earlier runbook documents.
#   sh bring_up.sh status     what is running, what the shim will declare, and the
#                             controller's state + timeline tail
#   sh bring_up.sh down       stop OUR processes only (controller, decoder,
#                             presenter, wheel tap).  Services and /vw untouched.
#
# WHAT CHANGED 2026-10-20 (HANDOFF_2026-10-20.md sections 1-3): the DELIVERY IS NO
# LONGER DONE HERE.  `transition_ctl.sh` owns switching, the decoder's lifetime and
# -- the reason this exists -- what the cluster shows WHEN THE PHONE LEAVES.
# Before this, bring-up armed the handover before the phone existed (a flat-green
# flash) and then nobody watched the stream, so a disconnect left the cluster
# showing a live stream of a stale CarPlay map, forever.
#
# WHAT IT REPLACES: 10-12 hand-typed steps, each of which has been got wrong at
# least once (the wmem ceiling forgotten, the presenter started with 3600,
# the socket labelled before it existed, the oracle started after the handover).
#
# PRE-CONDITIONS (deliberately NOT done here -- they are laptop-side):
#   sh MIB3_RE/shim/live_kit/push_kit.sh        # stage /tmp/kit, pre-flights pins
#   ssh mib3 'sh /tmp/kit/run_inject.sh'        # ONE injection per boot
#
# SAFETY: starts only OUR processes, writes only /dev/shm + sysctl ceilings +
# two SMACK rules.  Never restarts a service, never touches /vw.  A reboot is the
# full undo.  The delivery steps touch the live display RSI, so they run only in
# the supervised and `manual` modes -- `chain` never does.
# ============================================================================
KIT="${MIB3_KIT:-/tmp/kit}"
MODE="${1:-all}"
CARPLAY_PID="$(pidof carplay)"
URL="${MIB3_URL:-maps:/car/instrumentcluster?showETA=yes&maneuverLayout=topAligned&showSpeedLimit=no}"
# The presenter's dead-man switch: after this long with no decoded frame it shows
# the neutral fill instead of the last picture.  0 disables it (not advised).
IDLE_BLANK_MS="${MIB3_IDLE_BLANK_MS:-8000}"
say(){ echo "[up] $*"; }
die(){ echo "[up] ABORT: $*" >&2; exit 1; }
ctl() { sh "$KIT/transition_ctl.sh" "$@"; }

status(){
  _trp="$(cat /dev/shm/transition.lock/pid 2>/dev/null)"
  say "mode: supervised=${MODE}  (bring_up.sh chain|manual selects the others)"
  say "carplay=$(pidof carplay)  egl=$(pidof mib3egl)  dec=$(pidof v4l2_seq)  tap=$(pidof mib3wheel)  night=$(pidof mib3night)"
  say "controller: $( [ -n "$_trp" ] && kill -0 "$_trp" 2>/dev/null && echo "running (pid $_trp)" || echo 'NOT RUNNING -- an unhandled disconnect will only be caught by the presenter idle-blank' )"
  say "wmem_max=$(cat /proc/sys/net/core/wmem_max)  (want 8388608)"
  say "shim mapped: $(grep -c mib3shim /proc/$(pidof carplay)/maps 2>/dev/null)"
  say "SOCKETS: $(ls -l /dev/shm/mib3cluster.sock /dev/shm/mib3rgba.sock 2>/dev/null | tr '\n' ' ')"
  say "--- what the shim last declared ---"
  grep -E "cluster/va: (area index|class|  safeW|  safeH|initialViewArea)" /tmp/mib3shim.log 2>/dev/null | tail -n 14
  say "--- stream ---"
  grep -E "conn fd=|FIRST IDR" /tmp/mib3shim.log 2>/dev/null | tail -n 2
  grep STATUS /tmp/dec.log 2>/dev/null | tail -n 1
  say "--- presenter ---"
  grep -E "idle-blank|frames to commit|cadence" /tmp/egl.log 2>/dev/null | tail -n 4
  say "--- controller (transition_ctl.sh) ---"
  ctl status 2>/dev/null | tail -n 12
}

down(){
  say "stopping OUR processes only (no service is touched)"
  ctl stop 2>/dev/null
  [ -n "$(pidof v4l2_seq)" ] && { kill "$(pidof v4l2_seq)" 2>/dev/null; say "  decoder stopped"; }
  [ -n "$(pidof mib3wheel)" ] && { kill "$(pidof mib3wheel)" 2>/dev/null; say "  wheel tap stopped"; }
  [ -n "$(pidof mib3night)" ] && { kill "$(pidof mib3night)" 2>/dev/null; say "  night watcher stopped"; }
  # *** THE ARG BUG, FIXED (2026-10-26): `sh bring_up.sh down all` used to pass
  # "all" as down()'s FIRST positional while the function tested its SECOND --
  # so the presenter was never stopped and the doc's "down all" was a lie.
  # Tested by matching the FIRST positional (what every caller actually passes).
  if [ "${1:-}" = "all" ]; then
    [ -n "$(pidof mib3egl)" ] && { kill "$(pidof mib3egl)" 2>/dev/null; say "  presenter stopped"; }
  fi
  say "done.  The cluster keeps whatever is on it; a reboot restores stock."
}

case "$MODE" in
  status) status; exit 0 ;;
  down)   down "${2:-}"; exit 0 ;;  all|chain|manual) : ;;
  *) echo "usage: sh bring_up.sh [all|chain|manual|status|down [all]]" >&2; exit 2 ;;
esac

[ -n "$CARPLAY_PID" ] || die "carplay is not running"
[ -f "$KIT/mib3shim.so" ] || die "no kit at $KIT -- run push_kit.sh first"
grep -q mib3shim /proc/$CARPLAY_PID/maps 2>/dev/null || die "shim NOT injected -- run run_inject.sh (one per boot)"

# --- 1. knobs (idempotent; read back from inside the target) ----------------
say "knobs"
for kv in \
  MIB3_SHIM_GATE=1 MIB3_SHIM_ADD=1 MIB3_SHIM_SINK=helper \
  MIB3_SHIM_HELPER=/dev/shm/mib3cluster.sock MIB3_SHIM_PORT=5000 \
  MIB3_SHIM_CMDS=1 MIB3_SHIM_CMDACK=1 MIB3_SHIM_VIEWAREAS=1 \
  MIB3_SHIM_ZOOM=1 MIB3_SHIM_APPEARANCE=1 MIB3_SHIM_INFO=1 MIB3_SHIM_ENABLEDFEAT=1 ; do
  /usr/bin/python "$KIT/target_env.py" "$CARPLAY_PID" "$kv" >/dev/null 2>&1
done
/usr/bin/python "$KIT/target_env.py" "$CARPLAY_PID" "MIB3_SHIM_URL=$URL" >/dev/null 2>&1
say "  URL=$URL"

# --- 2. the sysctl CEILING (the run-killer if forgotten) --------------------
# A whole 2.4 MB frame must fit in one send(); below this the kernel clamps
# SO_SNDBUF to 2*wmem_max and every frame becomes a short write, the decoder
# closes the socket, and the cluster freezes on the presenter's flat colour.
echo 8388608 > /proc/sys/net/core/wmem_max
echo 8388608 > /proc/sys/net/core/rmem_max
say "  wmem_max=$(cat /proc/sys/net/core/wmem_max) rmem_max=$(cat /proc/sys/net/core/rmem_max) (defaults left at $(cat /proc/sys/net/core/wmem_default))"

# --- 3. SMACK both ways ----------------------------------------------------
# An AF_UNIX connect is checked BOTH ways: the base image ships common::lib ->
# carplay::app rx (no write) and nothing in the other direction.
echo "common::lib carplay::app rwx" > /sys/fs/smackfs/load2 2>/dev/null
echo "carplay::app common::lib rwx" > /sys/fs/smackfs/load2 2>/dev/null

# --- 3b. the cluster encoder rate: the ONE knob that raises the ECU's fps ----
# MS-S sizes its encode period as period_ms = 1000/framerate (startShare -> the
# shared-surface context -> omx_encoder_create -> omx_encoder_adjust_framerate),
# with 30 as its internal default when the field is unset.  The car ships 10.
#
# ORDER MATTERS AND IT IS NOT COSMETIC.  MS-S identifies a stream by
# (surfaceId, ip, framerate), so changing this field on a LIVE stream makes it
# treat the share as a new stream while the old one is still running -- and on
# 2026-10-20 that left the ECU fed by two encoders at once: "insanely artifacted"
# until a reboot.  Setting it HERE, before any of our elements exist, means
# startShare reads the new value and the stream is BORN at that rate.
#   MIB3_STREAM_FPS_DEFAULT=15   the default (send ~16.5 fps, so 15 is just under)
#   MIB3_STREAM_FPS_DEFAULT=0    leave the element alone
FPSD="${MIB3_STREAM_FPS_DEFAULT:-15}"
case "$FPSD" in
  ''|0) say "stream fps: left as-is (MIB3_STREAM_FPS_DEFAULT=0)" ;;
  *)  if [ -x "$KIT/stream_fps.sh" ]; then
        say "stream fps: ensuring framerate=$FPSD BEFORE any share exists (period $(($FPSD>0?1000/FPSD:0)) ms)"
        sh "$KIT/stream_fps.sh" set "$FPSD" 2>&1 | grep -E "VERIFIED|already|NOT VERIFIED" | sed 's/^/  /'
      else
        say "stream fps: stream_fps.sh missing -- leaving the element at its shipped value"
      fi ;;
esac

# --- 4. presenter (PERSISTENT: the 4th arg is SECONDS, 0 = forever) --------
if [ -z "$(pidof mib3egl)" ]; then
  say "presenter (ivi 3000, persistent, idle-blank ${IDLE_BLANK_MS} ms)"
  rm -f /dev/shm/mib3rgba.sock
  XDG_RUNTIME_DIR=/run/platform/weston WAYLAND_DISPLAY=wayland-0 \
    nohup "$KIT/mib3egl" 3000 1280 480 0 00ff00 --persist --fps 30 \
          --rgba-sock /dev/shm/mib3rgba.sock --idle-blank-ms "$IDLE_BLANK_MS" \
          > /tmp/egl.log 2>&1 &
  sleep 4
else
  say "presenter already running (pid $(pidof mib3egl))"
fi
[ -n "$(pidof mib3egl)" ] || die "presenter did not start -- read /tmp/egl.log"
[ -S /dev/shm/mib3rgba.sock ] || die "presenter did not bind its rgba socket"
grep -q "idle-blank (ms" /tmp/egl.log || say "  WARNING: the running presenter predates --idle-blank-ms (restart it to arm the dead-man switch)"

# HOW MANY FRAMES MAY BE IN FLIGHT to the presenter (V4L2_RGBA_QLIM, 1..4).
# The decoder skips a frame whenever the presenter still has one queued
# (SIOCOUTQ check) -- that skip is our own ~10% loss, and it is the same gap the
# phone punishes: it drops frames when ITS queue backs up past 67 ms.
# Bench (run_present.sh 34/0): qlim=1 -> 30/60 presented (7.0 fps), qlim=2 ->
# 60/60 (13.9 fps), zero partial frames in both.  Depth 2 costs up to one frame
# of latency (~60 ms at 16 fps).  Revert with MIB3_RGBA_QLIM=1.
# --- 5. decoder ------------------------------------------------------------
# PRE-ARMED, in every mode except `chain`.  This is not belt-and-braces, it is the
# difference between a clean picture and a permanently artifacted one:
#   * the phone sends roughly ONE keyframe per stream, in the first second;
#   * the shim drops frames while no helper is connected (`sink: helper absent`);
#   * starting the decoder later costs 6-8 s of V4L2 setup, so the only IDR is
#     long gone by the time it listens -> `keyframe wait timed out -> feeding
#     best-effort` -> mid-GOP decode -> artifacts that never heal.
# Measured on the car 2026-10-20 (the supervised flow started the decoder at
# PENDING, ~7 s after the stream was already up).  The controller ADOPTS this
# process (decoder_up: adopt when it has no client yet), so nothing kills it.
if [ "$MODE" = "chain" ]; then
  say "decoder: not started (chain mode; the controller starts one per session)"
else
  if [ -z "$(pidof v4l2_seq)" ]; then
    say "decoder (pre-armed BEFORE the phone connects, OUTN=8)"
    # MIB3_NV12=1 (+ MIB3_NV12_AT=N): arm the REAL-NV12 dump window for the
    # artifact measurement (works in ONLINE mode since 2026-10-26; the AT
    # offset aims the 12-frame window at guidance start, hundreds of frames
    # in -- the old "first 12" only ever covered the clean connect).
    # V4L2_NV12 must be OMITTED entirely when the dump is not wanted: the
    # decoder's env check is "is the string present", so =0 would enable it.
    if [ -n "${MIB3_NV12:-}" ]; then
      say "  NV12 dump ARMED: frames $(( ${MIB3_NV12_AT:-0} + 1 ))..$(( ${MIB3_NV12_AT:-0} + 12 )) -> /tmp/fr<N>.nv12"
      rm -f /dev/shm/mib3cluster.sock
      V4L2_FDFIELD=3 V4L2_IN_SOCK=/dev/shm/mib3cluster.sock \
      V4L2_RGBA_SOCK=/dev/shm/mib3rgba.sock V4L2_OUTN=8 \
      V4L2_RGBA_QLIM="${MIB3_RGBA_QLIM:-4}" \
      V4L2_NV12=1 V4L2_NV12_AT="${MIB3_NV12_AT:-0}" \
        nohup "$KIT/v4l2_seq" online 0 > /tmp/dec.log 2>&1 &
    else
      rm -f /dev/shm/mib3cluster.sock
      V4L2_FDFIELD=3 V4L2_IN_SOCK=/dev/shm/mib3cluster.sock \
      V4L2_RGBA_SOCK=/dev/shm/mib3rgba.sock V4L2_OUTN=8 \
      V4L2_RGBA_QLIM="${MIB3_RGBA_QLIM:-4}" \
        nohup "$KIT/v4l2_seq" online 0 > /tmp/dec.log 2>&1 &
    fi
    _i=0
    while [ "$_i" -lt 20 ]; do
      grep -q "listening on /dev/shm/mib3cluster.sock" /tmp/dec.log 2>/dev/null && break
      sleep 1; _i=$((_i+1))
    done
  else
    say "decoder already running (pid $(pidof v4l2_seq)) -- will be adopted"
  fi
  [ -n "$(pidof v4l2_seq)" ] || die "decoder did not start -- read /tmp/dec.log"
  [ -S /dev/shm/mib3cluster.sock ] || say "  NOTE: AU socket not bound yet (still in V4L2 setup) -- the shim drops frames until it is"
  # the socket is created LATE (after the V4L2 setup), so label it here, not before
  chsmack -a carplay::app /dev/shm/mib3cluster.sock 2>/dev/null
  say "  AU socket: $(chsmack /dev/shm/mib3cluster.sock 2>/dev/null)"
  grep -q "listening on /dev/shm/mib3cluster.sock" /tmp/dec.log 2>/dev/null \
    && say "  decoder is LISTENING before the phone: the first keyframe will be captured"
fi

# --- 6. steering-wheel zoom tap -------------------------------------------
sh "$KIT/wheel_zoom.sh" start >/dev/null 2>&1
sleep 2
[ -n "$(pidof mib3wheel)" ] && say "wheel tap: pid $(pidof mib3wheel)" || say "wheel tap FAILED (zoom will not work) -- run: sh $KIT/wheel_zoom.sh start"

# --- 6b. the day/night watcher (2026-10-26, the B3 fix) --------------------
# The cluster map must follow the car's theme the way the CENTRE screen already
# does.  The stock carplay binary keeps the centre screen right by subscribing
# /hmiintegration/dayNight/ (port 49570); mib3night polls the same element and
# mirrors each day/night edge into the shim's inbox as 'a map <mode> 0', which
# the shim ships to the phone as mapAppearanceUpdate.  Read-only + one inbox
# line; no shim change, no service touched; kill it any time.
#   probe first (evidence the service answers):  sh $KIT/night_watch.sh probe
#   one-shot fallback test of the whole chain:    /tmp/kit/mib3ctl a map 1 0
if [ -x "$KIT/mib3night" ]; then
  if [ -n "$(pidof mib3night)" ]; then
    say "night watcher: already running (pid $(pidof mib3night))"
  else
    sh "$KIT/night_watch.sh" start 2>/dev/null
    sleep 1
    if [ -n "$(pidof mib3night)" ]; then
      say "night watcher: pid $(pidof mib3night) (day/night -> 'a map <mode> 0' on the shim inbox)"
    else
      say "night watcher FAILED (cluster theme mirroring OFF) -- read /dev/shm/night.log"
      head -n 6 /dev/shm/night.log 2>/dev/null | sed 's/^/    /'
    fi
  fi
else
  say "night watcher: mib3night missing from the kit (cluster theme mirroring OFF)"
fi

export MIB3_DLV_STICK=/dev/shm           # the stick mounts read-only

# --- 7. the cluster source -------------------------------------------------
if [ "$MODE" = "chain" ]; then
  say "chain up; the controller and delivery are NOT running (use: sh bring_up.sh all)"
  status
  exit 0
fi

if [ "$MODE" = "manual" ]; then
  say "delivery (MANUAL, unsupervised): oracle -> park -> attach -> arm"
  sh "$KIT/deliver_ctl.sh" oracle start 2>&1 | tail -n 1
  sh "$KIT/deliver_ctl.sh" handover park  2>&1 | grep -E "PARK OK|DEGRADED|FAIL" | tail -n 1
  sh "$KIT/deliver_ctl.sh" handover attach 2>&1 | grep -E "ATTACH OK|DEGRADED|FAIL" | tail -n 1
  sh "$KIT/deliver_ctl.sh" handover arm   2>&1 | grep -E "ARM OK|DEGRADED|FAIL" | tail -n 1
  say "the cluster should now show the presenter's neutral fill"
  say "NOW connect the phone.  Want: conn fd= -> FIRST IDR -> STATUS frames climbing."
  say "NOTE: NOTHING WATCHES THIS.  On a disconnect the cluster keeps a stale map."
  say "      Use the supervised mode (sh bring_up.sh) for anything but a quick poke."
  say "the oracle log (/dev/shm/mss.log) grows ~8 MB/min; stop it when not reading it:"
  say "  sh $KIT/deliver_ctl.sh oracle stop"
  status
  exit 0
fi

# SUPERVISED (default).  The controller keeps the car's own cluster up until real
# frames exist, switches once, and on disconnect/completion stops the decoder,
# shows one neutral frame and hands the cluster back.
say "controller: starting transition_ctl.sh run (supervised)"
ctl stop >/dev/null 2>&1                 # idempotent; clears a stale lock/state
nohup sh "$KIT/transition_ctl.sh" run > /dev/shm/transition.out 2>&1 &
sleep 3
if ctl status >/dev/null 2>&1; then
  say "controller is up.  It will NOT touch the cluster until the phone sends real frames."
else
  die "controller did not start -- read /dev/shm/transition.out"
fi
say ""
say "NOW connect the phone.  What you should see, in order:"
say "  1. the car's own cluster stays on screen (the controller calls this PENDING)"
say "  2. on the first real frames it switches to Apple Maps (CARPLAY)"
say "  3. on disconnect: ~6 s hold, one neutral frame, then the car's cluster returns"
say "     (if the car's display service refuses the hand-back it stays neutral and"
say "      says so in the timeline -- never a stale map)"
say ""
say "watch:  sh /tmp/kit/transition_ctl.sh status   |   tail -n 20 /dev/shm/transition.log"
say "oracle: /dev/shm/mss.log grows ~8 MB/min -- sh /tmp/kit/deliver_ctl.sh oracle stop"
status
