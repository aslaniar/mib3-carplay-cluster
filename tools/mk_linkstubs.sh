#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build link-time stand-ins for the unit's libc/libdl into tests/build/linkstubs.
#
# A linker reads only a library's SONAME and version nodes, never its code, so
# these produce the identical binary as the real firmware libs. The symbol list
# doubles as the allowlist: anything not named here fails to link.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
OUT="$REPO/tests/build/linkstubs"
CC=${CC:-/opt/homebrew/opt/llvm/bin/clang}
LLD=${LLD:-/opt/homebrew/opt/lld/bin/ld.lld}
mkdir -p "$OUT"

# Every libc function the on-unit tools may call. Kept at GLIBC_2.17 -- nothing
# in src/ needs newer, and the unit runs 2.23.
LIBC_SYMS="__errno_location _exit accept access atoi bcmp bind chmod clock_gettime
close connect free fsync getenv getpid getsockname getsockopt htons if_nametoindex
inet_pton ioctl listen malloc memcpy memset open poll ppoll read realloc rename
send setenv setsockopt socket strcmp strlen strncmp time unlink usleep write"

LIBDL_SYMS="dlsym"

printf 'GLIBC_2.17 { global: *; };\n' > "$OUT/v.map"

stub() { # stub <soname> <symbols...>
  _so=$1; shift
  : > "$OUT/$_so.c"
  for _s in "$@"; do printf 'void %s(void){}\n' "$_s" >> "$OUT/$_so.c"; done
  $CC --target=aarch64-linux-gnu -O1 -fPIC -nostdlib -shared -w --ld-path="$LLD" \
      -Wl,-soname,"$_so" -Wl,--version-script="$OUT/v.map" \
      -o "$OUT/$_so" "$OUT/$_so.c"
}

stub libc.so.6  $LIBC_SYMS
stub libdl.so.2 $LIBDL_SYMS
