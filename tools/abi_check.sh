#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by the build scripts. Refuses a binary whose dynamic interface the car
# cannot satisfy.
#
#   ABI_REPO=<repo root>; . "$REPO/tools/abi_check.sh"
#   abi_check <binary> <expected-needed-lib> [...]
#
# Checks: AArch64; DT_NEEDED exactly as given; no GLIBC newer than the unit's
# 2.23; every undefined symbol defined by the generated stubs (the allowlist).
set -u
ABI_REPO="${ABI_REPO:-$PWD}"
ABI_STUBS="${ABI_STUBS:-$ABI_REPO/tests/build/linkstubs}"
READELF="${READELF:-/opt/homebrew/opt/llvm/bin/llvm-readelf}"
command -v "$READELF" >/dev/null 2>&1 || READELF=readelf
MAX_GLIBC="${MAX_GLIBC:-2.23}"

abi_die() { echo "abi_check: FAIL: $*" >&2; exit 1; }

# _ver_cmp <a> <b> -> gt|eq|lt
_ver_cmp() {
  awk -v a="$1" -v b="$2" 'BEGIN{
    n=split(a,x,"."); m=split(b,y,".");
    for(i=1;i<=3;i++){
      xv=(i<=n)?x[i]+0:0; yv=(i<=m)?y[i]+0:0;
      if(xv>yv){print "gt"; exit} if(xv<yv){print "lt"; exit}
    }
    print "eq"
  }'
}

abi_check() {
  _bin=$1; shift
  [ -f "$_bin" ] || abi_die "$_bin: no such file"
  _name=$(basename "$_bin")

  case "$("$READELF" -h "$_bin" | awk -F: '/Machine/{print $2}' | tr -d ' ')" in
    AArch64) ;;
    *) abi_die "$_name: not an AArch64 ELF" ;;
  esac

  _got=$("$READELF" -d "$_bin" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort | tr '\n' ' ')
  _want=$(printf '%s\n' "$@" | sort | tr '\n' ' ')
  [ "${_got% }" = "${_want% }" ] || \
    abi_die "$_name: DT_NEEDED [${_got% }] != expected [${_want% }]"

  _max=0
  for _v in $("$READELF" -V "$_bin" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -u); do
    [ "$(_ver_cmp "$_v" "$_max")" = gt ] && _max=$_v
  done
  [ "$_max" = 0 ] || [ "$(_ver_cmp "$_max" "$MAX_GLIBC")" != gt ] || \
    abi_die "$_name: needs GLIBC_$_max, unit has $MAX_GLIBC"

  # The stubs only define allowlisted names, so this catches a binary built
  # elsewhere (a real sysroot, another branch) rather than an edit in src/.
  if [ -f "$ABI_STUBS/libc.so.6" ]; then
    _allow=$(mktemp "${TMPDIR:-/tmp}/abi.XXXXXX")
    for _l in libc.so.6 libdl.so.2; do
      [ -f "$ABI_STUBS/$_l" ] && "$READELF" --dyn-syms "$ABI_STUBS/$_l" |
        awk '$4=="FUNC"{print $8}' | sed 's/@.*//'
    done | sort -u > "$_allow"
    _stray=$("$READELF" --dyn-syms "$_bin" | awk '$7=="UND"{print $8}' |
             sed 's/@.*//' | sed '/^$/d' | sort -u | comm -23 - "$_allow")
    rm -f "$_allow"
    [ -z "$_stray" ] || abi_die "$_name: symbols not on the allowlist: $(echo $_stray)"
  fi

  echo "abi: $_name ok  (needed: ${_want% }; glibc floor: GLIBC_$_max)"
}
