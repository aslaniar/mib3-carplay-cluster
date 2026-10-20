#!/bin/sh
# ============================================================================
# calib.sh -- put the CALIBRATION FRAME on the cluster, then restore the chain.
#
#   sh calib.sh make     draw the frame on the unit      (python calib_grid.py)
#   sh calib.sh on       stop our decoder, feed the frame to the presenter
#   sh calib.sh off      stop the feed, restart the decoder (chain back to normal)
#   sh calib.sh status   what is running / is the frame up
#
# WHY.  The view-area safe boxes were guesses.  A photo of the cluster showing a
# MAP cannot be measured; a photo showing this grid can: it says which part of
# our 1280x480 frame is visible in a given layout, where the centre lands, and
# the coordinates of the clear window between the gauges.
#
# HOW TO USE (one layout at a time):
#   1. sh calib.sh make && sh calib.sh on
#   2. put the cluster in layout A, photograph it; switch to B, photograph; C.
#   3. sh calib.sh off        (the phone picture and guidance come back)
#   4. read the boxes off the photos and set them:
#        MIB3_SHIM_VA0="0,0,1280,480,<sx>,<sy>,<sw>,<sh>"   (and VA1/VA2)
#      then reconnect the phone so the advert is rebuilt.
#
# SAFETY: this touches only OUR processes (v4l2_seq / rgba_feed).  No service is
# restarted, `carplay` is not touched, nothing is written outside /dev/shm.
# ============================================================================
KIT="${MIB3_KIT:-/tmp/kit}"
W=1280
H=480
RGBA_SOCK=/dev/shm/mib3rgba.sock
IN_SOCK=/dev/shm/mib3cluster.sock
RGBA_OUT=/dev/shm/calib.rgba
DEC_LOG=/tmp/dec.log

say(){ echo "[calib] $*"; }

case "${1:-status}" in
  make)
    /usr/bin/python "$KIT/calib_grid.py" "$W" "$H" "$RGBA_OUT" || { say "generator failed"; exit 1; }
    ls -l "$RGBA_OUT"
    ;;

  on)
    [ -f "$RGBA_OUT" ] || { say "no frame yet -- run: sh calib.sh make"; exit 1; }
    say "stopping our decoder (it owns the presenter's rgba socket)"
    kill $(pidof v4l2_seq) 2>/dev/null
    sleep 1
    # rgba_feed takes a dummy first argument (documented argv off-by-one)
    nohup "$KIT/rgba_feed" X "$RGBA_SOCK" "$RGBA_OUT" "$W" "$H" >/tmp/calib.log 2>&1 &
    sleep 3
    say "feeder=$(pidof rgba_feed)  presenter=$(pidof mib3egl)"
    tail -n 5 /tmp/calib.log 2>/dev/null
    say "photograph the cluster in each layout now, then: sh calib.sh off"
    ;;

  off)
    say "stopping the feeder"
    kill $(pidof rgba_feed) 2>/dev/null
    sleep 1
    rm -f "$IN_SOCK"
    V4L2_FDFIELD=3 V4L2_IN_SOCK="$IN_SOCK" V4L2_RGBA_SOCK="$RGBA_SOCK" V4L2_OUTN=8 \
      nohup "$KIT/v4l2_seq" online 0 > "$DEC_LOG" 2>&1 &
    sleep 6
    chsmack -a carplay::app "$IN_SOCK" 2>/dev/null
    say "decoder=$(pidof v4l2_seq)"
    grep -E "listening|STREAMON output" "$DEC_LOG" 2>/dev/null | tail -n 2
    say "the card needs a keyframe after a decoder restart: if the picture stays"
    say "artifacty, reconnect the phone (new session = new IDR)."
    ;;

  status)
    say "presenter=$(pidof mib3egl)  decoder=$(pidof v4l2_seq)  feeder=$(pidof rgba_feed)"
    [ -f "$RGBA_OUT" ] && ls -l "$RGBA_OUT" || say "no calibration frame drawn yet"
    [ -S "$RGBA_SOCK" ] && say "presenter socket present: $RGBA_SOCK" || say "NO presenter socket"
    ;;

  *)
    echo "usage: sh calib.sh {make|on|off|status}" >&2
    exit 2 ;;
esac
