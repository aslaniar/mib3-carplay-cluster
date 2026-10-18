#!/bin/sh
# Cross-build mib3shim.so for the LG MIB3 unit (aarch64).  Links against the
# UNIT's own libc/libdl (glibc 2.23) so the symbol versions are compatible and
# dlsym is guaranteed available.  Needs: brew llvm + brew lld.
set -e
CC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
READELF=/opt/homebrew/opt/llvm/bin/llvm-readelf
# run from the script's own directory so the relative source paths resolve.
# Resolve both paths to ABSOLUTE first: $0 is relative, so resolving it AFTER
# the cd below silently produced $PWD/../.. and made the documented
# `sh shim/build.sh` (from MIB3_RE) fail with "cd: ... No such file or
# directory".  Works from any cwd now.
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOTFS="$(cd "$HERE/../../R8830L_ROOTFS" && pwd)"
cd "$HERE" || exit 1
LIBC="$ROOTFS/lib64/libc.so.6"
LIBDL="$(ls "$ROOTFS"/lib64/libdl-*.so 2>/dev/null | head -n1)"
echo "linking against: $LIBC  $LIBDL"
$CC --target=aarch64-linux-gnu -O2 -fPIC -fvisibility=default -nostdlib \
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable \
    -fuse-ld=$LLD -shared -o mib3shim.so mib3shim.c "$LIBC" "$LIBDL"
echo "built mib3shim.so ($(wc -c < mib3shim.so) bytes)"
$READELF -h mib3shim.so | grep -E "Class|Machine|Type"
echo "--- NEEDED ---"; $READELF -d mib3shim.so | grep NEEDED
echo "--- GLIBC version refs (want <= 2.23) ---"; $READELF -V mib3shim.so | grep -oE "GLIBC_[0-9.]+" | sort -u
echo "--- exported seams ---"; $READELF --dyn-syms mib3shim.so | grep -E "FUNC.*GLOBAL.*(AirPlayInfoArrayAddScreenDisplay|AirPlayReceiverSessionSetup|AirPlayCopyServerInfo)"
for s in AirPlayInfoArrayAddScreenDisplay AirPlayReceiverSessionSetup AirPlayCopyServerInfo; do
  $READELF --dyn-syms mib3shim.so | grep -q "GLOBAL.* $s\$" || { echo "MISSING SEAM: $s"; exit 1; }
done

# ---------------------------------------------------------------------------
# The OUT-OF-PROCESS delivery helper (mib3clusterhelper).  A standalone aarch64
# program (entry _start, no crt) linked against the unit's own libc/libdl; it
# dlopen()s mib3shim.so and drives its sink exports.  See mib3clusterhelper.c.
# ---------------------------------------------------------------------------
echo
echo "building mib3clusterhelper (out-of-process delivery helper)"
$CC --target=aarch64-linux-gnu -O2 -nostdlib -fPIE -fuse-ld=$LLD -pie \
    -Wall -Wextra -Wno-unused-parameter -o mib3clusterhelper \
    mib3clusterhelper.c "$LIBC" "$LIBDL" -Wl,-e,_start
echo "built mib3clusterhelper ($(wc -c < mib3clusterhelper) bytes)"
$READELF -h mib3clusterhelper | grep -E "Class|Machine|Type"
