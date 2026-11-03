#!/bin/sh
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
# The four binaries are md5-fenced against the sources in this tree, so a stale
# build cannot reach the car: rebuild with `sh shim/build.sh` and
# `sh shim/tools/build.sh` if the pre-flight complains.
# ============================================================================
set -e
HOST="${1:-mib3}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SHIM="$(cd "$HERE/.." && pwd)"
cd "$SHIM" || exit 1

md5of() { if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }

# Field-proven builds (2026-11-02).  A pin is a binary that has run on the car.
SHIM_WANT=dc723befb0cd4c636ca2aa2a8f2c3743
SENDER_WANT=a9ab16d0bb8f29ed0ceda3f3ad69472d
WHEEL_WANT=e689515fc20a701f0e84d62d9dcd6251

checkpin() { # checkpin <path> <want>
  _got=$(md5of "$1" 2>/dev/null || echo missing)
  [ "$_got" = "$2" ] || {
    echo "[!] PRE-FLIGHT: $1 is $_got, want $2"
    echo "    rebuild: sh shim/build.sh && sh shim/tools/build.sh"
    exit 1
  }
}
checkpin mib3shim.so               "$SHIM_WANT"
checkpin tools/mib3clusterstream   "$SENDER_WANT"
checkpin tools/mib3wheel           "$WHEEL_WANT"

FILES="mib3shim.so \
  inject/inject.py inject/inject_shim.py inject/target_env.py inject/run_inject.sh \
  live_kit/pathb_daemon.sh live_kit/pathbctl live_kit/deliver_ctl.sh \
  live_kit/wheel_zoom.sh live_kit/pathb/msport.py \
  tools/mib3clusterstream tools/mib3wheel"

for f in $FILES; do
  [ -f "$f" ] || { echo "[!] missing $f"; exit 1; }
done

ssh "$HOST" 'mkdir -p /tmp/kit'
for f in $FILES; do cat "$f" | ssh "$HOST" "cat > /tmp/kit/$(basename "$f")"; done
# the hot-swap copy of the shim, under a name glibc cannot confuse with the first
cat mib3shim.so | ssh "$HOST" 'cat > /tmp/kit/mib3shim2.so'

ssh "$HOST" 'chmod +x /tmp/kit/*.sh /tmp/kit/*.py /tmp/kit/mib3shim.so /tmp/kit/mib3shim2.so /tmp/kit/mib3clusterstream /tmp/kit/mib3wheel; echo "--- /tmp/kit ---"; ls -l /tmp/kit'

echo
echo "next: one boot, ONE injection."
echo "  ssh $HOST 'sh /tmp/kit/run_inject.sh'      # load the shim into live carplay"
echo "  ssh $HOST 'sh /tmp/kit/pathbctl start'     # start the supervisor"
echo "  ssh $HOST 'sh /tmp/kit/pathbctl status'"
