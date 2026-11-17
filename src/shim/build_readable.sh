#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-build the rewritten shim (instrument_cluster_shim.c) for the LG MIB3 unit.
#
# This is the readable rewrite. It builds the same ABI as build.sh and is
# checked the same way, so the two files can be swapped without touching the
# injector, the sender or the supervision scripts.
set -e
CC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
READELF=/opt/homebrew/opt/llvm/bin/llvm-readelf
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BIN="$REPO/bin"
mkdir -p "$BIN"
cd "$HERE" || exit 1

OUT="${1:-$BIN/mib3shim.so}"

if [ -f "$REPO/R8830L_ROOTFS/lib64/libc.so.6" ]; then
  LIBC="$REPO/R8830L_ROOTFS/lib64/libc.so.6"
  LIBDL="$(ls "$REPO"/R8830L_ROOTFS/lib64/libdl-*.so 2>/dev/null | head -n1)"
else
  sh "$REPO/tools/mk_linkstubs.sh"
  LIBC="$REPO/tests/build/linkstubs/libc.so.6"
  LIBDL="$REPO/tests/build/linkstubs/libdl.so.2"
fi

$CC --target=aarch64-linux-gnu -O2 -fPIC -fvisibility=default -nostdlib \
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    --ld-path="$LLD" -shared -o "$OUT" \
    instrument_cluster_shim.c mib3navtext.c "$LIBC" "$LIBDL"

echo "built $OUT ($(wc -c < "$OUT") bytes, $(wc -l < instrument_cluster_shim.c) lines)"

ABI_REPO="$REPO"; . "$REPO/tools/abi_check.sh"
abi_check "$OUT" libc.so.6 libdl.so.2

for _s in AirPlayInfoArrayAddScreenDisplay AirPlayReceiverSessionSetup AirPlayCopyServerInfo; do
  $READELF --dyn-syms "$OUT" | grep -q "GLOBAL.* $_s\$" || { echo "MISSING SEAM: $_s"; exit 1; }
done
echo "exported seams: ok"

for _s in mib3_nt_connect mib3_nt_send mib3_nt_close; do
  $READELF --dyn-syms "$OUT" | grep -q "GLOBAL.* $_s\$" || { echo "MISSING HOOK: $_s"; exit 1; }
done
echo "exported hooks: ok"

for _s in connect send close; do
  if $READELF --dyn-syms "$OUT" | grep -q "GLOBAL.* $_s\$"; then
    echo "REFUSING: the shim must NOT export $_s"; exit 1
  fi
done
echo "no bare libc exports: ok"
