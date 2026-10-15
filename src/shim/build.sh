#!/bin/sh
# Cross-build mib3shim.so for the LG MIB3 unit (aarch64).  Links against the
# UNIT's own libc/libdl (glibc 2.23) so the symbol versions are compatible and
# dlsym is guaranteed available.  Needs: brew llvm + brew lld.
set -e
CC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
READELF=/opt/homebrew/opt/llvm/bin/llvm-readelf
ROOTFS="$(cd ../../R8830L_ROOTFS && pwd)"
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
echo "--- exported seams ---"; $READELF --dyn-syms mib3shim.so | grep -E "FUNC.*GLOBAL.*(AirPlayInfoArrayAddScreenDisplay|AirPlayReceiverSessionSetup)"
