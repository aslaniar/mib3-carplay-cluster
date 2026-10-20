#!/bin/sh
# ============================================================================
# bring_up.sh -- ONE command to bring the whole chain up, after push_kit+inject.
#
#   sh bring_up.sh            knobs, sysctl, SMACK, presenter, decoder, tap, delivery
#   sh bring_up.sh chain      everything EXCEPT the delivery handover
#   sh bring_up.sh status     what is running, and what the shim will declare
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
# full undo.  The delivery steps touch the live display RSI, so they run only
# when explicitly asked (the default does run them, `chain` mode does not).
# ============================================================================
KIT="${MIB3_KIT:-/tmp/kit}"
CARPLAY_PID="$(pidof carplay)"
URL="${MIB3_URL:-maps:/car/instrumentcluster?showETA=yes&maneuverLayout=topAligned&showSpeedLimit=no}"
say(){ echo "[up] $*"; }
die(){ echo "[up] ABORT: $*" >&2; exit 1; }

status(){
  say "carplay=$(pidof carplay)  egl=$(pidof mib3egl)  dec=$(pidof v4l2_seq)  tap=$(pidof mib3wheel)"
  say "wmem_max=$(cat /proc/sys/net/core/wmem_max)  (want 8388608)"
  say "shim mapped: $(grep -c mib3shim /proc/$(pidof carplay)/maps 2>/dev/null)"
  say "SOCKETS: $(ls -l /dev/shm/mib3cluster.sock /dev/shm/mib3rgba.sock 2>/dev/null | tr '\n' ' ')"
  say "--- what the shim last declared ---"
  grep -E "cluster/va: (area index|class|  safeW|  safeH|initialViewArea)" /tmp/mib3shim.log 2>/dev/null | tail -n 14
  say "--- stream ---"
  grep -E "conn fd=|FIRST IDR" /tmp/mib3shim.log 2>/dev/null | tail -n 2
  grep STATUS /tmp/dec.log 2>/dev/null | tail -n 1
}

[ -n "$CARPLAY_PID" ] || die "carplay is not running"
[ -f "$KIT/mib3shim.so" ] || die "no kit at $KIT -- run push_kit.sh first"
grep -q mib3shim /proc/$CARPLAY_PID/maps 2>/dev/null || die "shim NOT injected -- run run_inject.sh (one per boot)"

case "${1:-all}" in
  status) status; exit 0 ;;
  chain|all) : ;;
  *) echo "usage: sh bring_up.sh [all|chain|status]" >&2; exit 2 ;;
esac

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

# --- 4. presenter (PERSISTENT: the 4th arg is SECONDS, 0 = forever) --------
if [ -z "$(pidof mib3egl)" ]; then
  say "presenter (ivi 3000, persistent)"
  rm -f /dev/shm/mib3rgba.sock
  XDG_RUNTIME_DIR=/run/platform/weston WAYLAND_DISPLAY=wayland-0 \
    nohup "$KIT/mib3egl" 3000 1280 480 0 00ff00 --persist --fps 30 \
          --rgba-sock /dev/shm/mib3rgba.sock > /tmp/egl.log 2>&1 &
  sleep 4
else
  say "presenter already running (pid $(pidof mib3egl))"
fi
[ -n "$(pidof mib3egl)" ] || die "presenter did not start -- read /tmp/egl.log"
[ -S /dev/shm/mib3rgba.sock ] || die "presenter did not bind its rgba socket"

# --- 5. decoder ------------------------------------------------------------
if [ -z "$(pidof v4l2_seq)" ]; then
  say "decoder (OUTN=8)"
  rm -f /dev/shm/mib3cluster.sock
  V4L2_FDFIELD=3 V4L2_IN_SOCK=/dev/shm/mib3cluster.sock \
  V4L2_RGBA_SOCK=/dev/shm/mib3rgba.sock V4L2_OUTN=8 \
    nohup "$KIT/v4l2_seq" online 0 > /tmp/dec.log 2>&1 &
  sleep 8
else
  say "decoder already running (pid $(pidof v4l2_seq))"
fi
[ -n "$(pidof v4l2_seq)" ] || die "decoder did not start -- read /tmp/dec.log"
grep -q "listening on /dev/shm/mib3cluster.sock" /tmp/dec.log || die "decoder is not listening -- read /tmp/dec.log"
# the socket is created LATE (after the V4L2 setup), so label it here, not before
chsmack -a carplay::app /dev/shm/mib3cluster.sock 2>/dev/null
say "  AU socket: $(chsmack /dev/shm/mib3cluster.sock 2>/dev/null)"

# --- 6. steering-wheel zoom tap -------------------------------------------
sh "$KIT/wheel_zoom.sh" start >/dev/null 2>&1
sleep 2
[ -n "$(pidof mib3wheel)" ] && say "wheel tap: pid $(pidof mib3wheel)" || say "wheel tap FAILED (zoom will not work) -- run: sh $KIT/wheel_zoom.sh start"

# --- 7. delivery ----------------------------------------------------------
if [ "${1:-all}" = "chain" ]; then
  say "chain up; delivery NOT touched (use: sh bring_up.sh all)"
  status
  exit 0
fi
export MIB3_DLV_STICK=/dev/shm           # the stick mounts read-only
say "delivery: oracle -> park -> attach -> arm"
sh "$KIT/deliver_ctl.sh" oracle start 2>&1 | tail -n 1
sh "$KIT/deliver_ctl.sh" handover park  2>&1 | grep -E "PARK OK|DEGRADED|FAIL" | tail -n 1
sh "$KIT/deliver_ctl.sh" handover attach 2>&1 | grep -E "ATTACH OK|DEGRADED|FAIL" | tail -n 1
sh "$KIT/deliver_ctl.sh" handover arm   2>&1 | grep -E "ARM OK|DEGRADED|FAIL" | tail -n 1

say "the cluster should now show the presenter's flat colour -- delivery proven"
say "NOW connect the phone.  Want: conn fd= -> FIRST IDR -> STATUS frames climbing."
say "the oracle log (/dev/shm/mss.log) grows ~8 MB/min; stop it when not reading it:"
say "  sh $KIT/deliver_ctl.sh oracle stop"
status
