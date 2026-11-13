#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Write bin/PINS from the three binaries now in bin/.
#
# up.sh, src/inject/run_inject.sh and src/control/push_kit.sh refuse any binary
# whose md5 is not the one in bin/PINS.  Run this only after a build you mean to
# deploy, and commit bin/PINS together with the binaries it names.
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/bin"
MD5SUM() { if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }
for f in mib3shim.so mib3clusterstream mib3wheel; do
  [ -f "$BIN/$f" ] || { echo "pin.sh: missing $BIN/$f" >&2; exit 1; }
done
{
  echo "# md5 of each build in bin/. Written by tools/pin.sh."
  echo "SHIM_PIN=$(MD5SUM "$BIN/mib3shim.so")"
  echo "SENDER_PIN=$(MD5SUM "$BIN/mib3clusterstream")"
  echo "WHEEL_PIN=$(MD5SUM "$BIN/mib3wheel")"
} >"$BIN/PINS.tmp"
mv "$BIN/PINS.tmp" "$BIN/PINS"
cat "$BIN/PINS"
