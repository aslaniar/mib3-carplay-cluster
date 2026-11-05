#!/bin/sh
# Build the harmless injection-proof library for the unit (aarch64), linking the
# UNIT's own libc so the symbol versions match. Mirrors shim/build.sh.
set -e
CC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
READELF=/opt/homebrew/opt/llvm/bin/llvm-readelf
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOTFS="$(cd "$HERE/../../R8830L_ROOTFS" && pwd)"
LIBC="$ROOTFS/lib64/libc.so.6"

echo "linking against $LIBC"
$CC --target=aarch64-linux-gnu -O2 -fPIC -fvisibility=default -nostdlib \
    -Wall -Wextra -fuse-ld=$LLD -shared -o "$HERE/libinjectproof.so" \
    "$HERE/proof.c" "$LIBC"
echo "built libinjectproof.so ($(wc -c < "$HERE/libinjectproof.so") bytes)"
$READELF -h "$HERE/libinjectproof.so" | grep -E "Machine|Type"
$READELF -d "$HERE/libinjectproof.so" | grep NEEDED
md5 -q "$HERE/libinjectproof.so" 2>/dev/null || md5sum "$HERE/libinjectproof.so"
