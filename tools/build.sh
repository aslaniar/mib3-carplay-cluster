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
# run from the script's own directory so the relative source paths resolve.
# Resolve both paths to ABSOLUTE first -- same relative-$0 bug as shim/build.sh:
# resolving $0 after the cd made the documented invocation from MIB3_RE fail.
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOTFS="$(cd "$HERE/../../../R8830L_ROOTFS" && pwd)"
cd "$HERE" || exit 1
MD5SUM() { if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }
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
# v4l2_probe: what IS each /dev/video* node (decoder vs encoder)?
[ -f v4l2_probe.c ] && build_libc v4l2_probe.c v4l2_probe
# v4l2_dec: drive /dev/video32 (the H.264 decoder) directly, V4L2 M2M
[ -f v4l2_dec.c ] && build_libc v4l2_dec.c v4l2_dec
# v4l2_seq: the VENDOR-EXACT /dev/video32 sequence (2-plane capture + PREPARE_BUF
# + private S_CTRLs) captured by strace on the car 2026-10-17. This is the tool
# that now DECODES the phone's cluster stream into buffers we own.
[ -f v4l2_seq.c ] && build_libc v4l2_seq.c v4l2_seq
# ausock: feed Annex-B AUs from a file into the helper's AF_UNIX socket
# (AUSOCK_UID/AUSOCK_GID drop privileges so the carplay-credential path can be
# exercised without a phone).
[ -f ausock.c ] && build_libc ausock.c ausock
# socktest: reproduce the shim's helper connect as a given uid/gid and print errno
[ -f socktest.c ] && build_libc socktest.c socktest
# omx_probe: is the HW decoder drivable via the OpenMAX component API?
[ -f omx_probe.c ] && build_libc omx_probe.c omx_probe
[ -f omx_ports.c ] && build_libc omx_ports.c omx_ports
[ -f omx_dec.c ] && build_libc omx_dec.c omx_dec
[ -f omx_idx.c ] && build_libc omx_idx.c omx_idx
[ -f omx_vt.c ] && build_libc omx_vt.c omx_vt
[ -f omx_run.c ] && build_libc omx_run.c omx_run
[ -f omx_fix.c ] && build_libc omx_fix.c omx_fix
[ -f omx_full.c ] && build_libc omx_full.c omx_full
[ -f omx_decode.c ] && build_libc omx_decode.c omx_decode   # REDTEAM_DECODE_2026-10-17: the corrected driver (supersedes omx_full on the unit)
# mib3wheel: the STEERING-WHEEL zoom watcher (2026-10-18).  A read-only RSI
# client: it subscribes to the unit's own maprenderer (port 49500) and forwards
# wheel zoom steps to the shim's inbox (/dev/shm/mib3zoom.sock), where the shim
# turns them into changeMapZoomLevel for the CarPlay cluster map.  libc only.
[ -f mib3wheel.c ] && build_libc mib3wheel.c mib3wheel
# mib3night: the DAY/NIGHT watcher (2026-10-26, LANE_GEN_FIX_2026-10-26).  A
# read-only RSI client: it polls the unit's own hmiintegration dayNight element
# (port 49570) and, on every day/night change, writes 'a map <mode> <setting>'
# to the SAME inbox, which the shim turns into mapAppearanceUpdate -- so the
# cluster map follows the car's theme like the centre screen does (the rule is
# read from the STOCK carplay binary: night <=> the collection has an element
# named "nightmode").  libc only.
[ -f mib3night.c ] && build_libc mib3night.c mib3night
# mib3ctl: the ONE-SHOT control-inbox button (2026-10-19).  The wheel watcher is
# a long-lived RSI subscriber; this is the thing you press to flip a cluster
# view area (view/next/prev), mirror a wheel zoom, or set an appearance, by
# writing ONE line to the shim's inbox.  libc only.
[ -f mib3ctl.c ] && build_libc mib3ctl.c mib3ctl
# wl_trace.so: the DRAW-CALL TRACER -- a shared library (not an executable: no crt,
# no _start).  Loaded into OUR OWN helper only (LD_PRELOAD, or first in
# MIB3_HELPER_LIBS) to log the EGL/Wayland calls the vendor video device makes.
if [ -f wl_trace.c ]; then
  $CC --target=aarch64-linux-gnu -O2 -fPIC -fvisibility=default -nostdlib \
      -Wall -Wextra -fuse-ld=$LLD -shared -o wl_trace.so wl_trace.c "$LIBC" "$LIBDL"
  echo "  -> wl_trace.so ($(wc -c < wl_trace.so) bytes)"
fi

# Route-B RTP/MPEG-TS sender.
#
# HISTORY (corrected 2026-10-27): this comment used to say mib3clusterstream.c is a
# "normal HOSTED program" needing an aarch64 libc sysroot, and skipped the build
# without AARCH64_SYSROOT set.  That is wrong.  The file has a freestanding branch
# (-DMIB3_UNIT_AARCH64) that declares the handful of libc prototypes it uses and
# links the unit's own libc.so.6, so it needs NO headers and NO sysroot.  The
# field session rebuilt it exactly this way.  A build that silently skips the one
# binary the takeover runs is how a stale sender gets pushed to the car.
if [ -f mib3clusterstream.c ]; then
  $CC --target=aarch64-linux-gnu -DMIB3_UNIT_AARCH64 -O2 -nostdlib -fPIE \
      -fuse-ld=$LLD -pie -Wl,-e,_start -o mib3clusterstream mib3clusterstream.c "$LIBC"
  echo "  -> mib3clusterstream ($(wc -c < mib3clusterstream) bytes)"
else
  echo "  !! mib3clusterstream.c missing -- the Path B sender was NOT built"
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
echo "--- pins (md5) ---"
echo "  HISTORY, do not quote as current: the 2026-10-16 pins"
echo "  (mib3surface d09cd0da..., mib3egl2 ef73fdd5...) NO LONGER reproduce --"
echo "  their sources changed since.  The truth is the list this script just"
echo "  measured; verified 2026-10-18: mib3egl 7612f74a (the upright fix),"
echo "  v4l2_seq f6abd36c (BLOCKING -- superseded 2026-10-19 by bff15e2a, the"
echo "  non-blocking whole-frame drop), mib3surface aaff6555, mib3egl2 beabf11a,"
echo "  rgba_feed a056eabc.  (actual, this build:)"
for b in mib3surface mib3egl mib3egl2 rgba_feed v4l2_seq ausock socktest omx_decode mib3wheel mib3night mib3ctl mib3clusterstream; do
  [ -f "$b" ] && printf '  %-20s %s\n' "$b" "$(MD5SUM "$b")"
done
exit 0
