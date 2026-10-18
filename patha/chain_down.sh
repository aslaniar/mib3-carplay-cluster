#!/bin/sh
# chain_down.sh -- stop the decode+present chain we started (RAM-only).
kill $(pidof v4l2_seq) 2>/dev/null
kill $(pidof mib3egl)  2>/dev/null
sleep 1
rm -f /dev/shm/mib3cluster.sock /dev/shm/mib3rgba.sock
echo "[chain] stopped. egl=$(pidof mib3egl) dec=$(pidof v4l2_seq)"
