#!/bin/sh
# ============================================================================
# v4l2_run.sh -- run v4l2_seq ON THE UNIT under a hard wall-clock guard.
#
# WHY: the 2026-10-17 run reset the unit -- the documented vidc-driver wedge.
# The tool itself is bounded, but a wedged v4l2 ioctl (DQBUF/FLUSH) can block
# forever, and there is no `timeout` binary on the unit.  This wrapper kills the
# tool after GUARD seconds so a bad run can never hold the vehicle.
#
# Run it ON THE UNIT, from the kit dir:
#     sh /tmp/kit/v4l2_run.sh /tmp/kit/real.264 400
# Env passes through (V4L2_FDFIELD=3 is REQUIRED for the driver to accept a
# buffer: it wants the ION fd in v4l2_plane.reserved[0]).
# ============================================================================
GUARD="${GUARD:-20}"
TOOL="${TOOL:-/tmp/kit/v4l2_seq}"
LOG="${LOG:-/tmp/v4l2_run.log}"
: > "$LOG"
echo "[run] $TOOL $* (guard ${GUARD}s)" >> "$LOG"
"$TOOL" "$@" >> "$LOG" 2>&1 &
PID=$!
i=0
while [ "$i" -lt "$GUARD" ]; do
  kill -0 "$PID" 2>/dev/null || break
  sleep 1
  i=$((i+1))
done
if kill -0 "$PID" 2>/dev/null; then
  echo "[run] GUARD FIRED after ${i}s -- killing $PID (possible v4l2 wedge)" >> "$LOG"
  kill -9 "$PID" 2>/dev/null
  sleep 1
fi
echo "[run] exit after ${i}s" >> "$LOG"
cat "$LOG"
