#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the harmless injection-proof library for the unit (aarch64).
# Mirrors shim/build.sh: unit libc when R8830L_ROOTFS is present, else stubs.
set -e
CC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

if [ -f "$REPO/R8830L_ROOTFS/lib64/libc.so.6" ]; then
  LIBC="$REPO/R8830L_ROOTFS/lib64/libc.so.6"
else
  sh "$REPO/tools/mk_linkstubs.sh"
  LIBC="$REPO/tests/build/linkstubs/libc.so.6"
fi

$CC --target=aarch64-linux-gnu -O2 -fPIC -fvisibility=default -nostdlib \
    -Wall -Wextra --ld-path="$LLD" -shared -o "$HERE/libinjectproof.so" \
    "$HERE/proof.c" "$LIBC"
echo "built libinjectproof.so ($(wc -c < "$HERE/libinjectproof.so") bytes)"

ABI_REPO="$REPO"; . "$REPO/tools/abi_check.sh"
abi_check "$HERE/libinjectproof.so" libc.so.6
