#!/bin/sh
# ============================================================================
# v4l2_sweep.sh -- try the decode variants back-to-back, each under a hard
# wall-clock guard, and print only the lines that decide the outcome.
#
# Run ON THE UNIT (after staging /tmp/kit/v4l2_seq):
#     sh /tmp/kit/v4l2_sweep.sh
#
# The one non-negotiable: V4L2_FDFIELD=3 (the driver wants the ION fd in
# v4l2_plane.reserved[0]) -- without it every QBUF is EINVAL.
# ============================================================================
GUARD="${GUARD:-18}"
TOOL=/tmp/kit/v4l2_seq
F=/tmp/kit/real.264
LOG=/tmp/v4l2_sweep.log
: > "$LOG"

one(){
  name="$1"; shift
  echo "================ $name ================" | tee -a "$LOG"
  rm -f /tmp/sweep_run.log
  env "$@" MIB3_FDFIELD_X=1 "$TOOL" "$F" 400 >> /tmp/sweep_run.log 2>&1 &
  PID=$!
  i=0
  while [ "$i" -lt "$GUARD" ]; do kill -0 "$PID" 2>/dev/null || break; sleep 1; i=$((i+1)); done
  if kill -0 "$PID" 2>/dev/null; then echo "  !! GUARD FIRED (${i}s) -- killing" | tee -a "$LOG"; kill -9 "$PID" 2>/dev/null; fi
  grep -E "AUs|primed output|QBUF cap ok|QBUF cap fail idx=0|STREAMON|DQEVENT|FLUSH|CAP n=|REAL FRAMES|capture returns|output consumed|STAGE1|wrote REAL" /tmp/sweep_run.log | tail -n 14 | tee -a "$LOG"
  echo "" | tee -a "$LOG"
  sleep 2
}

# 1) the recipe as rebuilt: one AU per buffer + TIMESTAMP_COPY (the vendor's flags)
one "A: AU-per-buffer + TS (default)"            V4L2_FDFIELD=3
# 2) whole file in one buffer (the older feeding style)
one "B: whole-file-in-one-buffer"                V4L2_FDFIELD=3 V4L2_WHOLEFILE=1
# 3) no timestamp / no TIMESTAMP_COPY flag
one "C: no TS flag"                              V4L2_FDFIELD=3 V4L2_TS=0
# 4) capture format Q128 (the UBWC form the vendor also negotiated)
one "D: AU + TS + Q128 capture"                  V4L2_FDFIELD=3 V4L2_CAPFMT=1
# 5) force the early flush even without a source-change event
one "E: AU + forced early flush"                 V4L2_FDFIELD=3 V4L2_DECCMD_ALWAYS=1

echo "===== summary =====" | tee -a "$LOG"
grep -E "================|REAL FRAMES|STAGE1" "$LOG"
echo "(full log: $LOG)"
