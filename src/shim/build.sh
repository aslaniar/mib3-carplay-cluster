#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-build mib3shim.so for the LG MIB3 unit (aarch64).
#
# Links the unit's own libc/libdl when R8830L_ROOTFS is present, else generated
# stubs -- linking reads only SONAME and version nodes, so both give the same
# .so. dlsym is guaranteed available either way.
set -e
CC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
READELF=/opt/homebrew/opt/llvm/bin/llvm-readelf
# Resolve both paths to ABSOLUTE before cd: $0 is relative.
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BIN="$REPO/bin"
mkdir -p "$BIN"
cd "$HERE" || exit 1

MD5SUM() { if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }

if [ -f "$REPO/R8830L_ROOTFS/lib64/libc.so.6" ]; then
  LIBC="$REPO/R8830L_ROOTFS/lib64/libc.so.6"
  LIBDL="$(ls "$REPO"/R8830L_ROOTFS/lib64/libdl-*.so 2>/dev/null | head -n1)"
else
  sh "$REPO/tools/mk_linkstubs.sh"
  LIBC="$REPO/tests/build/linkstubs/libc.so.6"
  LIBDL="$REPO/tests/build/linkstubs/libdl.so.2"
fi
echo "linking against: $LIBC  $LIBDL"

$CC --target=aarch64-linux-gnu -O2 -fPIC -fvisibility=default -nostdlib \
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable \
    --ld-path="$LLD" -shared -o "$BIN/mib3shim.so" mib3shim.c "$LIBC" "$LIBDL"
echo "built $BIN/mib3shim.so ($(wc -c < "$BIN/mib3shim.so") bytes)"

ABI_REPO="$REPO"; . "$REPO/tools/abi_check.sh"
abi_check "$BIN/mib3shim.so" libc.so.6 libdl.so.2

for _s in AirPlayInfoArrayAddScreenDisplay AirPlayReceiverSessionSetup AirPlayCopyServerInfo; do
  $READELF --dyn-syms "$BIN/mib3shim.so" | grep -q "GLOBAL.* $_s\$" || { echo "MISSING SEAM: $_s"; exit 1; }
done
echo "exported seams: ok"

_g=$(MD5SUM "$BIN/mib3shim.so"); _p="$(sed -n 's/^SHIM_PIN=//p' "$REPO/up.sh" | head -n1)"
if [ "$_g" = "$_p" ]; then echo "pin: $_g  matches up.sh"; else echo "pin: $_g  up.sh pins $_p"; fi

# The out-of-process decode host belonged to Path A and is not built.
