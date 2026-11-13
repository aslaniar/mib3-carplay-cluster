#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-build the two on-unit tools (aarch64).
#
#   mib3wheel            the steering-wheel zoom watch (read-only RSI tap)
#   mib3clusterstream    the Path B sender: re-mux the phone's H.264 onto the
#                        cluster's RTP/MPEG-TS multicast
#
# The image ships no crt, so each tool has its own _start and links -nostdlib.
# Linking needs only the unit libs' SONAME and version nodes, so generated stubs
# do the job when R8830L_ROOTFS is absent; both paths give the same bytes.
#
# NEEDS: brew llvm (clang) + brew lld.
set -e
CC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
BIN="$REPO/bin"
mkdir -p "$BIN"
cd "$HERE" || exit 1

MD5SUM() { if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }
PIN() { sed -n "s/^$1=//p" "$REPO/bin/PINS" | head -n1; }

if [ -f "$REPO/R8830L_ROOTFS/lib64/libc.so.6" ]; then
  LIBC="$REPO/R8830L_ROOTFS/lib64/libc.so.6"
  LIBDL="$(ls "$REPO"/R8830L_ROOTFS/lib64/libdl-*.so 2>/dev/null | head -n1)"
  echo "cross-compiling for aarch64 (unit libs)"
else
  sh "$REPO/tools/mk_linkstubs.sh"
  LIBC="$REPO/tests/build/linkstubs/libc.so.6"
  LIBDL="$REPO/tests/build/linkstubs/libdl.so.2"
  echo "cross-compiling for aarch64 (link stubs, no firmware image)"
fi
ABI_REPO="$REPO"; . "$REPO/tools/abi_check.sh"

# The link line is part of the pin: mib3wheel needs libdl, the sender does not.
build() { # build <source> <output> [extra-libs...]
  _src=$1 _out=$2; shift 2
  [ -f "$_src" ] || { echo "  !! $_src missing -- refusing to skip $_out"; exit 1; }
  $CC --target=aarch64-linux-gnu -DMIB3_UNIT_AARCH64 -O2 -nostdlib -fPIE \
      --ld-path="$LLD" -pie -Wl,-e,_start -o "$_out" "$_src" "$LIBC" "$@"
  echo "  -> $_out ($(wc -c < "$_out") bytes)"
}

build "$REPO/src/wheel/mib3wheel.c"          "$BIN/mib3wheel"         "$LIBDL"
build "$REPO/src/sender/mib3clusterstream.c" "$BIN/mib3clusterstream"

echo
echo "--- ABI the car must satisfy ---"
abi_check "$BIN/mib3wheel"         libc.so.6 libdl.so.2
abi_check "$BIN/mib3clusterstream" libc.so.6

echo
echo "--- pins (md5) ---"
chk_pin() { # chk_pin <file> <pin-from-bin/PINS>
  _g=$(MD5SUM "$1")
  if [ "$_g" = "$2" ]; then printf '  %-20s %s  matches bin/PINS\n' "$(basename "$1")" "$_g"
  else printf '  %-20s %s  bin/PINS has %s\n' "$(basename "$1")" "$_g" "$2"; fi
}
chk_pin "$BIN/mib3wheel"         "$(PIN WHEEL_PIN)"
chk_pin "$BIN/mib3clusterstream" "$(PIN SENDER_PIN)"
