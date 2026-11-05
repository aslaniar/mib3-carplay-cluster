#!/bin/sh
# ============================================================================
# tools/build.sh -- cross-build the two on-unit tools (aarch64).
#
#   mib3wheel            the steering-wheel zoom watcher (read-only RSI tap)
#   mib3clusterstream    the Path B sender: re-mux the phone's H.264 onto the
#                        cluster's RTP/MPEG-TS multicast
#
# Both link the UNIT's own libs from R8830L_ROOTFS, not a sysroot, so the NEEDED
# sonames and glibc symbol versions (<= GLIBC_2.23) match the car.
#
# The recipe is freestanding on purpose: this image ships no crt, so every tool
# has its own _start and is linked -nostdlib with -Wl,-e,_start.  A build that
# silently skips a tool is how a stale binary reaches the car, so a missing
# source is an error here, not a skip.
#
# NEEDS: brew llvm (clang) + brew lld.
# ============================================================================
set -e
CC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
READELF=/opt/homebrew/opt/llvm/bin/llvm-readelf
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
ROOTFS="$(cd "$REPO/R8830L_ROOTFS" && pwd)"
BIN="$REPO/bin"
mkdir -p "$BIN"
cd "$HERE" || exit 1

MD5SUM() { if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }
LIBC="$ROOTFS/lib64/libc.so.6"
LIBDL="$ROOTFS/lib64/libdl-2.23.so"
for f in "$LIBC" "$LIBDL"; do
  [ -f "$f" ] || { echo "MISSING: $f (is R8830L_ROOTFS present at the repo root?)"; exit 1; }
done

echo "cross-compiling on-unit tools for aarch64 (unit libs from $ROOTFS)"

# libc-only freestanding executable: -nostdlib, -fPIE, our own _start.
# The link line is part of the pin: mib3wheel needs libdl, the sender does not,
# and matching each tool's original flags is what makes the md5 reproducible.
build() { # build <source> <output> [extra-libs...]
  _src=$1 _out=$2; shift 2
  [ -f "$_src" ] || { echo "  !! $_src missing -- refusing to skip $_out"; exit 1; }
  $CC --target=aarch64-linux-gnu -DMIB3_UNIT_AARCH64 -O2 -nostdlib -fPIE \
      -fuse-ld=$LLD -pie -Wl,-e,_start -o "$_out" "$_src" "$LIBC" "$@"
  echo "  -> $_out ($(wc -c < "$_out") bytes)"
}

build "$REPO/src/wheel/mib3wheel.c"          "$BIN/mib3wheel"         "$LIBDL"
build "$REPO/src/sender/mib3clusterstream.c" "$BIN/mib3clusterstream"

echo
echo "--- sanity: machine + NEEDED (want AArch64, unit sonames only) ---"
for b in "$BIN/mib3wheel" "$BIN/mib3clusterstream"; do
  printf '%s: ' "$(basename "$b")"
  $READELF -h "$b" | grep -o 'AArch64'
  $READELF -d "$b" | grep NEEDED | sed 's/^/    /'
done

echo
echo "--- pins (md5) ---"
for b in "$BIN/mib3wheel" "$BIN/mib3clusterstream"; do
  printf '  %-20s %s\n' "$(basename "$b")" "$(MD5SUM "$b")"
done
