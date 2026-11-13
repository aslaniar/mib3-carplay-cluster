#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# ============================================================================
# push_kit.sh -- stage the Path B kit into the unit's /tmp/kit.
#
#   sh push_kit.sh [ssh-host]        (default host: mib3)
#
# Everything here is RAM-only: /tmp is wiped by every reboot, so re-run this
# after each one.  Nothing staged by this script survives a reboot.
#
# What is staged, and why nothing else:
#   mib3shim.so        the interposer that advertises the cluster and owns the
#                      phone's cluster stream
#   mib3shim2.so       the same build under a second name -- the only way to
#                      swap the shim inside a running carplay (a mapped .so
#                      cannot be replaced in place)
#   inject/            ptrace + dlopen + GOT-slot patching; the shim is loaded
#                      into the LIVE carplay, so there is no restart and no
#                      reboot
#   pathb_daemon.sh    the resident supervisor (takes and returns the cluster)
#   pathbctl           the read-only operator CLI for it
#   deliver_ctl.sh     the RSI handover the supervisor drives (park / hand back)
#   pathb/msport.py    reads multi-screen-server's current cluster source port
#   mib3clusterstream  the sender: the phone's H.264 straight onto the cluster
#   wheel_zoom.sh      steering-wheel zoom tap (optional, started if present)
#   mib3wheel          its watcher binary
#
# The three binaries are md5-fenced against bin/PINS, so a stale build cannot
# reach the car: rebuild with `sh src/shim/build.sh` and `sh tools/build.sh`,
# then `sh tools/pin.sh`, if the pre-flight complains.
# ============================================================================
set -e
HOST="${1:-mib3}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT" || exit 1

md5of() { if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }

pin() { sed -n "s/^$1=//p" bin/PINS 2>/dev/null | head -n1; }
SHIM_WANT=$(pin SHIM_PIN); SENDER_WANT=$(pin SENDER_PIN); WHEEL_WANT=$(pin WHEEL_PIN)
[ -n "$SHIM_WANT" ] && [ -n "$SENDER_WANT" ] && [ -n "$WHEEL_WANT" ] \
  || { echo "[!] bin/PINS is missing or incomplete -- run sh tools/pin.sh"; exit 1; }

checkpin() { # checkpin <path> <want>
  _got=$(md5of "$1" 2>/dev/null || echo missing)
  [ "$_got" = "$2" ] || {
    echo "[!] PRE-FLIGHT: $1 is $_got, want $2"
    echo "    rebuild: sh src/shim/build.sh && sh tools/build.sh"
    exit 1
  }
}
checkpin bin/mib3shim.so             "$SHIM_WANT"
checkpin bin/mib3clusterstream       "$SENDER_WANT"
checkpin bin/mib3wheel               "$WHEEL_WANT"

FILES="bin/PINS bin/mib3shim.so \
  src/inject/inject.py src/inject/inject_shim.py src/inject/target_env.py src/inject/run_inject.sh \
  src/control/pathb_daemon.sh src/control/pathbctl src/control/deliver_ctl.sh \
  src/control/wheel_zoom.sh src/control/pathb/msport.py \
  bin/mib3clusterstream bin/mib3wheel"

for f in $FILES; do
  [ -f "$f" ] || { echo "[!] missing $f"; exit 1; }
done

ssh "$HOST" 'mkdir -p /tmp/kit'
for f in $FILES; do cat "$f" | ssh "$HOST" "cat > /tmp/kit/$(basename "$f")"; done
# the hot-swap copy of the shim, under a name glibc cannot confuse with the first
cat bin/mib3shim.so | ssh "$HOST" 'cat > /tmp/kit/mib3shim2.so'

ssh "$HOST" 'chmod +x /tmp/kit/*.sh /tmp/kit/*.py /tmp/kit/mib3shim.so /tmp/kit/mib3shim2.so /tmp/kit/mib3clusterstream /tmp/kit/mib3wheel; echo "--- /tmp/kit ---"; ls -l /tmp/kit'

echo
echo "next: one boot, ONE injection."
echo "  ssh $HOST 'sh /tmp/kit/run_inject.sh'      # load the shim into live carplay"
echo "  ssh $HOST 'sh /tmp/kit/pathbctl start'     # start the supervisor"
echo "  ssh $HOST 'sh /tmp/kit/pathbctl status'"
