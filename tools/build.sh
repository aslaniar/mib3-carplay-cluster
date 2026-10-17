#!/bin/sh
# ============================================================================
# tools/build.sh -- cross-build the LIVE-ONLY on-unit tools (aarch64).
#
# These are the small self-contained Wayland/EGL clients that own ivi surface
# 3000 (mib3egl, mib3surface) and the Route-B RTP/MPEG-TS sender
# (mib3clusterstream).  There was NO build script for these before -- the recipe
# was reconstructed from the shipped binaries' NEEDED lists and verified to
# reproduce them BYTE-IDENTICAL (md5 pinned below).  See
# LANE_CLUSTER_DELIVERY_IMPLEMENT_2026-10-16.md S"Reproducible build".
#
# KEY RECIPE POINTS (each was a bug found the hard way):
#   * --target=aarch64-linux-gnu, brew llvm + brew lld.
#   * -nostdlib  -- there is no crt for this image; every tool has its own _start
#                   (reads argv from /proc/self/cmdline) and -Wl,-e,_start.
#   * link the UNIT's OWN libs from R8830L_ROOTFS so the NEEDED sonames and the
#     glibc symbol versions (<= GLIBC_2.17/2.23) match the unit.
#   * libwayland does NOT export the ivi interface structs on this image -- the
#     tools declare them locally (see mib3egl.c).  Do not try to link them.
#
# NEEDS: brew llvm (clang), brew lld.
# ============================================================================
set -e
CC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
READELF=/opt/homebrew/opt/llvm/bin/llvm-readelf
# run from the script's own directory so the relative source paths resolve
cd "$(dirname "$0")" || exit 1
MD5SUM() { if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }

ROOTFS="$(cd "$(dirname "$0")/../../../R8830L_ROOTFS" && pwd)"
LIBC="$ROOTFS/lib64/libc.so.6"
LIBDL="$ROOTFS/lib64/libdl-2.23.so"
WLCLIENT="$ROOTFS/usr/lib64/libwayland-client.so.0.3.0"
WLEGL="$ROOTFS/usr/lib64/libwaylandegl.so"
LEGL="$ROOTFS/usr/lib64/libEGL_adreno.so"
LGLES="$ROOTFS/usr/lib64/libGLESv2_adreno.so"

for f in "$LIBC" "$LIBDL" "$WLCLIENT" "$WLEGL" "$LEGL" "$LGLES"; do
  [ -f "$f" ] || { echo "MISSING: $f  (is R8830L_ROOTFS present beside MIB3_RE?)"; exit 1; }
done

echo "cross-compiling on-unit tools for aarch64 (unit libs from $ROOTFS)"

# waitland-client only (SHM presenter)
build_wl() {
  $CC --target=aarch64-linux-gnu -O2 -nostdlib -fPIE -fuse-ld=$LLD \
      -Wl,-e,_start -o "$2" "$1" "$LIBC" "$LIBDL" "$WLCLIENT"
  echo "  -> $2 ($(wc -c < "$2") bytes)"
}
# libc only (no Wayland/EGL) -- the RGBA feeder
build_libc() {
  $CC --target=aarch64-linux-gnu -O2 -nostdlib -fPIE -fuse-ld=$LLD \
      -Wl,-e,_start -o "$2" "$1" "$LIBC" "$LIBDL"
  echo "  -> $2 ($(wc -c < "$2") bytes)"
}
# wayland + EGL/GLES (dmabuf presenter)
build_egl() {
  $CC --target=aarch64-linux-gnu -O2 -nostdlib -fPIE -fuse-ld=$LLD \
      -Wl,-e,_start -o "$2" "$1" "$LIBC" "$LIBDL" "$WLCLIENT" "$WLEGL" "$LEGL" "$LGLES"
  echo "  -> $2 ($(wc -c < "$2") bytes)"
}

build_wl  mib3surface.c mib3surface
build_egl mib3egl.c     mib3egl
# mib3egl2: the explicit-EGL-config diagnostic presenter (same lib set as mib3egl).
[ -f mib3egl2.c ] && build_egl mib3egl2.c mib3egl2
# rgba_feed: push a known RGBA frame into a presenter's --rgba-sock (libc only).
[ -f rgba_feed.c ] && build_libc rgba_feed.c rgba_feed

# Route-B RTP/MPEG-TS sender.  NOTE: mib3clusterstream.c is a normal HOSTED program
# (it #includes <stdio.h>/<sys/socket.h>), so it needs an aarch64 *libc sysroot*
# (-isysroot / --sysroot) which is NOT in this repo.  We build it only if one is
# supplied; otherwise reuse the committed binary (data/DELIVERY_20261016/
# mib3clusterstream.unit == the aarch64 build that ran on the unit).
if [ -n "${AARCH64_SYSROOT:-}" ] && [ -f mib3clusterstream.c ]; then
  $CC --target=aarch64-linux-gnu -O2 --sysroot="$AARCH64_SYSROOT" -fuse-ld=$LLD \
      -Wl,-e,_start -o mib3clusterstream mib3clusterstream.c "$LIBC" "$LIBDL"
  echo "  -> mib3clusterstream ($(wc -c < mib3clusterstream) bytes)"
else
  echo "  -- mib3clusterstream: SKIPPED (hosted source; needs AARCH64_SYSROOT)."
  echo "     reuse the committed data/DELIVERY_20261016/mib3clusterstream.unit"
fi

echo
echo "--- sanity: machine + NEEDED (want AArch64, only unit sonames) ---"
for b in mib3surface mib3egl mib3egl2 rgba_feed mib3clusterstream; do
  [ -f "$b" ] || continue
  printf '%s: ' "$b"
  $READELF -h "$b" | grep -o 'AArch64'
  $READELF -d "$b" | grep NEEDED | sed 's/^/    /'
done

echo
echo "--- known-good md5s (reproduce byte-identical with this script) ---"
echo "  mib3surface       d09cd0dad7045fac206f400e04ae0188"
echo "  mib3egl2          ef73fdd517afff801f77606ccf700791"
echo "  (mib3egl/rgba_feed are pinned in harness/run_delivery.sh -- they change when their source does)"
echo "  (actual, this build:)"
for b in mib3surface mib3egl mib3egl2 rgba_feed mib3clusterstream; do
  [ -f "$b" ] && printf '  %-16s %s\n' "$b" "$(MD5SUM "$b")"
done
exit 0
