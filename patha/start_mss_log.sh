#!/bin/sh
# ============================================================================
# start_mss_log.sh -- start the ORACLE: multi-screen-server's own live log.
#
# WHY: the delivery control (deliver_ctl.sh) is a separate process from the encoder,
# so the ONLY objective read-out of "did the encoder take our surface, and at what
# cost" is MS-S's log on port 54321. On the unit it lands on the STICK (a fault can
# reset the unit and wipe /dev/shm, so the stick is the surviving record).
#
# This is a THIN WRAPPER -- the one implementation lives in deliver_ctl.sh
# (`oracle start|stop|tail`). Use whichever name the runbook/card gives you.
#
# USAGE (on the unit):
#   nohup sh start_mss_log.sh &        # start the capture (idempotent; safe to pre-start)
#   sh start_mss_log.sh stop           # stop it
#   sh start_mss_log.sh tail           # show the last share/stream decisions
#
# It makes NO display call and starts/stops nothing but its own curl. RAM-only.
# ============================================================================
set -u

# locate deliver_ctl.sh without external tools (busybox-safe)
case "$0" in
  */*) HERE="${0%/*}" ;;
  *)   HERE="." ;;
esac
CTL=""
for c in "$HERE/deliver_ctl.sh" /tmp/kit/deliver_ctl.sh /dev/shm/deliver_ctl.sh; do
  [ -f "$c" ] && { CTL="$c"; break; }
done
if [ -z "$CTL" ]; then
  echo "[msslog] ERROR: deliver_ctl.sh not found next to this script (looked in $HERE, /tmp/kit, /dev/shm)" >&2
  exit 3
fi

exec sh "$CTL" oracle "${1:-start}"
