#!/bin/sh
# ============================================================================
# chain_up.sh -- bring up the cluster DECODE+PRESENT chain (RAM-only).
#
#   shim (in carplay) --AUs over AF_UNIX-->  v4l2_seq (hw H.264 decode)
#        --RGBA over AF_UNIX-->  mib3egl (owns ivi surface 3000)
#        --> multi-screen-server --> cluster
#
# Run ON THE UNIT.  Safe: it starts only OUR OWN processes; no service is touched.
#   sh chain_up.sh          start (idempotent: kills ours first)
#   sh chain_down.sh        stop ours
# ============================================================================
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/platform/weston}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
SOCK=/dev/shm/mib3cluster.sock
RGBA=/dev/shm/mib3rgba.sock
FPS="${FPS:-30}"

kill $(pidof v4l2_seq) 2>/dev/null
kill $(pidof mib3egl)  2>/dev/null
sleep 1
rm -f "$SOCK" "$RGBA" /tmp/egl.log /tmp/dec.log

# 1) the presenter FIRST: it must own ivi 3000 with a committed buffer before
#    deliver_ctl.sh registers the element (the order is load-bearing).
nohup /tmp/kit/mib3egl 3000 1280 480 3600 00ff00 --persist --fps "$FPS" \
      --rgba-sock "$RGBA" > /tmp/egl.log 2>&1 &
sleep 3

# 2) the decoder/presenter bridge (online: shim AUs -> hw decode -> RGBA).
V4L2_FDFIELD=3 V4L2_IN_SOCK="$SOCK" V4L2_RGBA_SOCK="$RGBA" \
  nohup /tmp/kit/v4l2_seq online 0 > /tmp/dec.log 2>&1 &
sleep 4

# The shim runs as carplay::app and SMACK needs the socket's LABEL to match it
# (chmod 777 is not enough -- a common::lib socket is refused). Label it here.
chsmack -a carplay::app "$SOCK" >/dev/null 2>&1
# AND: AF_UNIX connect is checked BOTH ways.  The base image ships
# `common::lib carplay::app rx` (no write) and nothing from carplay::app to
# common::lib -- either side missing makes connect() fail EACCES.  RAM-only
# (a reboot is the full undo).
echo "common::lib carplay::app rwx" > /sys/fs/smackfs/load2 2>/dev/null
echo "carplay::app common::lib rwx" > /sys/fs/smackfs/load2 2>/dev/null
echo "[chain] egl=$(pidof mib3egl) dec=$(pidof v4l2_seq)"
echo "[chain] presenter:"
tail -n 4 /tmp/egl.log 2>/dev/null | sed 's/^/    /'
echo "[chain] decoder:"
tail -n 4 /tmp/dec.log 2>/dev/null | sed 's/^/    /'
[ -S "$SOCK" ] && echo "[chain] AU socket present: $SOCK" || echo "[chain] !! AU socket MISSING"
