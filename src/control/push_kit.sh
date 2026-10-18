#!/bin/sh
# ============================================================================
# push_kit.sh -- stage the whole live kit into the unit's /tmp/kit in one shot.
#
#   sh push_kit.sh [ssh-host]        (default host: mib3)
#
# Run from anywhere; it resolves the repo from its own location.  /tmp is wiped
# by every reboot, so re-run this after each one.  Nothing here is persistent.
#
# NOTE: real.264 / test.264 are NOT in the repo.  Generate a smoke-test stream
# with  python3 shim/tools/mk_h264.py 4 1280 480 > test.264  (I_PCM only), or
# bring a stick capture.
# ============================================================================
set -e
HOST="${1:-mib3}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SHIM="$(cd "$HERE/.." && pwd)"
cd "$SHIM" || exit 1

ssh "$HOST" 'mkdir -p /tmp/kit'

FILES="mib3shim.so mib3clusterhelper \
  inject/inject.py inject/inject_shim.py inject/target_env.py inject/run_inject.sh \
  live_kit/deliver_ctl.sh live_kit/chain_up.sh live_kit/chain_down.sh \
  live_kit/v4l2_run.sh live_kit/v4l2_sweep.sh \
  tools/v4l2_seq tools/mib3egl tools/ausock tools/socktest tools/rgba_feed"

for f in $FILES; do
  [ -f "$f" ] || { echo "[!] missing $f -- run sh build.sh and sh tools/build.sh first"; exit 1; }
  cat "$f" | ssh "$HOST" "cat > /tmp/kit/$(basename "$f")"
done

# a second copy of the shim under a DIFFERENT name: the only way to UPGRADE the
# shim in an already-running carplay (a mapped .so cannot be replaced in place).
cat mib3shim.so | ssh "$HOST" 'cat > /tmp/kit/mib3shim2.so'

ssh "$HOST" 'chmod +x /tmp/kit/*.sh /tmp/kit/*.py /tmp/kit/mib3shim.so /tmp/kit/mib3shim2.so /tmp/kit/v4l2_seq /tmp/kit/mib3egl /tmp/kit/ausock /tmp/kit/socktest /tmp/kit/rgba_feed /tmp/kit/mib3clusterhelper 2>/dev/null; echo "--- /tmp/kit ---"; ls -la /tmp/kit'
