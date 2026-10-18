#!/bin/sh
# ============================================================================
# probe.sh -- hands-free: wait for the phone AU capture, then run the decisive
#             file-mode decodes on it and leave a report for the operator.
#
# WHY: the phone tethers the laptop, so the agent's session dies the moment the
# phone moves to the car.  Nothing can be watched live.  So the unit collects
# the evidence by itself while it is unattended.
#
# RAM-ONLY.  No service is touched.  Every decoder run is wall-clock guarded
# (v4l2_run.sh) because the vidc driver can wedge.
#
# Writes:  /dev/shm/report.txt      <- the thing to read first
#          /dev/shm/aus_final.264   <- the captured phone stream
#          /dev/shm/p0.log p1.log p2.log
# ============================================================================
AUS=/dev/shm/aus.264
REP=/dev/shm/report.txt
KEEP=/dev/shm/aus_final.264

echo "probe: armed at $(date 2>/dev/null || cat /proc/uptime)" > "$REP"

# ---- 1) wait for a non-empty capture (up to ~2 h at 3 s) -------------------
i=0
while [ "$i" -lt 2400 ]; do
  [ -s "$AUS" ] && break
  # the phone may never arrive; note why we are still waiting
  sleep 3
  i=$((i+1))
done
if [ ! -s "$AUS" ]; then
  echo "NO CAPTURE: aus.264 never became non-empty." >> "$REP"
  echo "=> the phone never streamed to us (check /tmp/mib3shim.log)." >> "$REP"
  exit 0
fi
echo "capture appeared after $((i*3)) s" >> "$REP"

# ---- 2) wait for the stream to stop growing (10 x 3 s of no change) --------
last=-1; stable=0
while [ "$stable" -lt 10 ]; do
  cur=$(wc -c < "$AUS")
  if [ "$cur" = "$last" ]; then stable=$((stable+1)); else stable=0; last=$cur; fi
  sleep 3
done
echo "capture settled: $last bytes" >> "$REP"

# ---- 3) stop the online decoder: file mode needs /dev/video32 exclusively --
cp -f "$AUS" "$KEEP"
kill $(pidof v4l2_seq) 2>/dev/null
sleep 2

# ---- 4) the probes --------------------------------------------------------
# p0 = today's behaviour (reproduce the blocker)
# p1 = the vendor's second setup pass (the fix candidate)
# p2 = p1 with a deeper output queue (starvation check)
echo "" >> "$REP"
echo "=== p0: baseline (V4L2_RESTART=0) ===" >> "$REP"
LOG=/dev/shm/p0.log GUARD=30 V4L2_FDFIELD=3 V4L2_NV12=/tmp/f1.nv12 \
  sh /tmp/kit/v4l2_run.sh "$KEEP" 400 >> "$REP" 2>&1
grep -E "result|consumed|returns|REAL FRAMES|PRIMARY|no real frames|HARDWARE" /dev/shm/p0.log >> "$REP" 2>/dev/null
sleep 2

echo "" >> "$REP"
echo "=== p1: vendor second pass (V4L2_RESTART=1) ===" >> "$REP"
LOG=/dev/shm/p1.log GUARD=30 V4L2_RESTART=1 V4L2_FDFIELD=3 V4L2_NV12=/tmp/g1.nv12 \
  sh /tmp/kit/v4l2_run.sh "$KEEP" 400 >> "$REP" 2>&1
grep -E "restart|renegotiate|granted|consumed|returns|REAL FRAMES|HARDWARE|no real frames" /dev/shm/p1.log >> "$REP" 2>/dev/null
sleep 2

echo "" >> "$REP"
echo "=== p2: second pass + deeper output queue (OUTN=8) ===" >> "$REP"
LOG=/dev/shm/p2.log GUARD=30 V4L2_RESTART=1 V4L2_OUTN=8 V4L2_FDFIELD=3 \
  sh /tmp/kit/v4l2_run.sh "$KEEP" 400 >> "$REP" 2>&1
grep -E "consumed|returns|REAL FRAMES|HARDWARE|no real frames" /dev/shm/p2.log >> "$REP" 2>/dev/null

# ---- 5) driver state, cheap and read-only --------------------------------
echo "" >> "$REP"
echo "=== msm_vidc instances ===" >> "$REP"
ls -la /sys/kernel/debug/msm_vidc/ >> "$REP" 2>&1
cat /sys/kernel/debug/msm_vidc/instances >> "$REP" 2>&1

# ---- 6) restart the online decoder so the chain is back up ----------------
V4L2_FDFIELD=3 V4L2_IN_SOCK=/dev/shm/mib3cluster.sock \
  V4L2_RGBA_SOCK=/dev/shm/mib3rgba.sock V4L2_DUMPAUS=/dev/shm/aus2.264 \
  nohup /tmp/kit/v4l2_seq online 0 > /tmp/dec2.log 2>&1 &
sleep 3
chsmack -a carplay::app /dev/shm/mib3cluster.sock >/dev/null 2>&1

echo "" >> "$REP"
echo "=== shim log tail ===" >> "$REP"
tail -n 30 /tmp/mib3shim.log >> "$REP" 2>&1
echo "" >> "$REP"
echo "=== decode log tail ===" >> "$REP"
tail -n 25 /tmp/dec.log >> "$REP" 2>&1
echo "PROBE DONE" >> "$REP"
