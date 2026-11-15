#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# ============================================================================
# run_navtext.sh -- the bench for the cluster nav-text gate.
#
# The gate: carplay posts the phone's navigation records to simplenav on
# /tmp/Simple_Socket.  simplenav turns them into the RSI "simplenavigation"
# service, navsd turns that into BAP, and the cluster ECU prints the address
# line and the distance/arrival-time line around our map.  The gate drops those
# records at the socket, so the head unit has nothing to send
# (src/shim/mib3navtext.c).
#
# Each assertion fails if the mechanism breaks or becomes vacuous:
#
#   N1  the proof (needs a firmware dump): simplenav's parser dispatches on the
#       event id at offset 0 and the technology at offset 4, with 100/101/102,
#       and carplay has exactly one connect/send/close call slot.  Skipped, and
#       reported as skipped, when no dump is present.
#   N2  the host build of the shipping source compiles -Wall -Wextra clean
#   N3  hook behaviour against a real AF_UNIX socket: connect unchanged, fd
#       recorded, one record per send, filter drops exactly the listed ids
#   N4  the above is not vacuous: an unlisted id, a short buffer, a bogus
#       technology and an unknown id all still reach the listener (fail-open)
#   N5  live knob: MIB3_SHIM_NAVTEXT is read per record, so off restores
#       delivery on the next record
#   N6  fd safety: close() forgets the fd, and a reused fd number is not dropped
#   N7  mid-session adoption: a socket connected before injection is found
#   N8  the aarch64 shim builds, exports the three hooks, and does not export
#       connect/send/close.  An exported connect would be found by RTLD_DEFAULT
#       and the shim's own helper socket would recurse into the gate.
#   N9  nothing newer than GLIBC_2.23 is required (the unit's libc)
#   N10 the build is reproducible (same md5 twice)
#   N11 the aarch64 glue, by disassembly: delegates to the gate, calls the real
#       libc function through the resolved pointer, never its own PLT slot
#   N12 the kit carries the knob and pins this build (up.sh, run_inject.sh,
#       push_kit.sh, pathb_daemon.sh)
#
# Bench only.  No car and no unit.  Usage: bash tests/run_navtext.sh
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
SHIMDIR="$REPO/src/shim"
KIT="$REPO/src/control"
BIN="$REPO/bin"
ROOT="$REPO/R8830L_ROOTFS"
OUT="${NAVTEXT_OUT:-$REPO/tests/build}"
CC=${CC:-cc}
READELF=/opt/homebrew/opt/llvm/bin/llvm-readelf
PY=python3
mkdir -p "$OUT"

P=0; F=0
ok(){ echo "NAVTEXT $1 exp=$2 got=$2 PASS"; P=$((P+1)); }
no(){ echo "NAVTEXT $1 exp=$2 got=$3 FAIL"; F=$((F+1)); }
chk(){ if [ "$2" = "1" ]; then ok "$1" "$3"; else no "$1" "$3" "${4:-no}"; fi; }

echo "############ N1: the proof (both shipped binaries) ############"
if [ ! -d "$ROOT" ]; then
  echo "NAVTEXT N1_proof SKIPPED -- no R8830L_ROOTFS beside the repo."
  echo "        (This check reads simplenav's parser and carplay's call slots out of"
  echo "        the firmware. It is optional: maintainers who have the dump get it.)"
elif $PY "$REPO/tools/navtext_proof.py" "$ROOT" >"$OUT/proof.log" 2>&1; then
  ok N1_proof "event ids + 3 call slots"
  sed -n '1,12p' "$OUT/proof.log" | sed 's/^/    /'
else
  no N1_proof "event ids + 3 call slots" "see $OUT/proof.log"
  sed -n '1,40p' "$OUT/proof.log" | sed 's/^/    /'
fi

echo
echo "############ N2: host build of the shipping source ############"
HOSTBIN="$OUT/test_navtext"
if $CC -O1 -g -Wall -Wextra -Wno-unused-parameter -o "$HOSTBIN" "$HERE/test_navtext.c" \
      2>"$OUT/build.log"; then
  if [ -s "$OUT/build.log" ]; then
    no N2_host_build_clean "no warnings" "$(head -3 "$OUT/build.log" | tr '\n' ' ')"
  else ok N2_host_build_clean "no warnings"; fi
else
  no N2_host_build_clean "builds" "$(head -5 "$OUT/build.log" | tr '\n' ' ')"
fi

echo
echo "############ N3-N7: the gate against a real socket ############"
if [ -x "$HOSTBIN" ]; then
  if "$HOSTBIN" >"$OUT/hook.log" 2>&1; then
    chk N3_hook_behaviour "$(grep -q '  ok ' "$OUT/hook.log" && echo 1 || echo 0)" "$(grep -c '  ok ' "$OUT/hook.log") checks"
    chk N4_fail_open "$(grep -q 'fail-open' "$OUT/hook.log" && echo 1 || echo 0)" "malformed input forwarded"
    chk N5_live_knob "$(grep -q 'live knob' "$OUT/hook.log" && echo 1 || echo 0)" "off takes effect per record"
    chk N6_fd_safety "$(grep -q 'fd reuse: the unrelated socket' "$OUT/hook.log" && echo 1 || echo 0)" "reused fd not swallowed"
    echo "    $(tail -1 "$OUT/hook.log")"
  else
    no N3_hook_behaviour "0 failures" "$(grep -c 'FAIL' "$OUT/hook.log") failures"
    grep 'FAIL' "$OUT/hook.log" | sed 's/^/    /'
  fi
  if "$HOSTBIN" --adopt >"$OUT/adopt.log" 2>&1; then
    ok N7_midsession_adopt "pre-existing socket adopted"
  else
    no N7_midsession_adopt "pre-existing socket adopted" "$(grep 'FAIL' "$OUT/adopt.log" | tr '\n' ' ')"
  fi
else
  no N3_hook_behaviour "runs" "no host binary"
  no N7_midsession_adopt "runs" "no host binary"
fi

echo
echo "############ N8-N10: the aarch64 shim ############"
if sh "$SHIMDIR/build.sh" >"$OUT/shim_build.log" 2>&1; then
  chk N8_shim_builds 1 "bin/mib3shim.so built"
  MD5A="$(md5 -q "$BIN/mib3shim.so" 2>/dev/null || md5sum "$BIN/mib3shim.so" | cut -d' ' -f1)"
  EXP_OK=1
  for s in mib3_nt_connect mib3_nt_send mib3_nt_close; do
    $READELF --dyn-syms "$BIN/mib3shim.so" | grep -q "GLOBAL.* $s\$" || EXP_OK=0
  done
  chk N8b_hooks_exported "$EXP_OK" "mib3_nt_connect/send/close"
  LEAF_OK=1
  for s in connect send close; do
    if $READELF --dyn-syms "$BIN/mib3shim.so" | grep -q "GLOBAL.* $s\$"; then LEAF_OK=0; fi
  done
  chk N8c_no_bare_libc_exports "$LEAF_OK" "no exported connect/send/close"
  { $READELF -V "$BIN/mib3shim.so" | grep -oE "GLIBC_[0-9.]+" | sort -u >"$OUT/glibc.txt"; } 2>/dev/null
  if [ -s "$OUT/glibc.txt" ] && [ "$(grep -vc 'GLIBC_2\.\(1[0-9]\|2[0-3]\)$' "$OUT/glibc.txt")" = "0" ]; then
    ok N9_glibc_ceiling "<= 2.23 ($(tr '\n' ' ' <"$OUT/glibc.txt"))"
  else
    no N9_glibc_ceiling "<= 2.23" "$(tr '\n' ' ' <"$OUT/glibc.txt")"
  fi
  sh "$SHIMDIR/build.sh" >"$OUT/shim_build2.log" 2>&1
  MD5B="$(md5 -q "$BIN/mib3shim.so" 2>/dev/null || md5sum "$BIN/mib3shim.so" | cut -d' ' -f1)"
  if [ -n "${MD5A:-}" ] && [ "$MD5A" = "$MD5B" ]; then ok N10_reproducible "$MD5A"; else no N10_reproducible "$MD5A" "$MD5B"; fi
  echo "SHIM_MD5=$MD5A" >"$OUT/pin.env"
else
  no N8_shim_builds "builds" "$(tail -3 "$OUT/shim_build.log" | tr '\n' ' ')"
fi

echo
echo "############ N11: the aarch64 glue, by disassembly ############"
if $PY "$HERE/test_glue_aarch64.py" >"$OUT/glue.log" 2>&1; then
  ok N11_glue_shape "$(grep -c '  ok ' "$OUT/glue.log") checks"
else
  no N11_glue_shape "0 failures" "$(grep -c 'FAIL' "$OUT/glue.log") failures (see $OUT/glue.log)"
  grep 'FAIL' "$OUT/glue.log" | sed 's/^/    /'
fi

echo
echo "############ N12: the kit carries the knob and this pin ############"
chk N12a_kit_sets_knob "$(grep -q 'MIB3_SHIM_NAVTEXT=filter' "$KIT/pathb_daemon.sh" && echo 1 || echo 0)" \
    "pathb_daemon.sh sets the gate"
chk N12b_safe_area "$(grep -q 'MIB3_SHIM_VA_INITIAL=1' "$KIT/pathb_daemon.sh" && echo 1 || echo 0)" \
    "pathb_daemon.sh sets the tuned safe area"
chk N12c_up_sh_pin "$(grep -q "${MD5A:-zzz}" "$REPO/up.sh" && echo 1 || echo 0)" "up.sh pins this build"
chk N12d_inject_pin "$(grep -q "${MD5A:-zzz}" "$REPO/src/inject/run_inject.sh" && echo 1 || echo 0)" "run_inject.sh pins this build"
chk N12e_push_pin "$(grep -q "${MD5A:-zzz}" "$KIT/push_kit.sh" && echo 1 || echo 0)" "push_kit.sh pins this build"

echo
echo "===================================================================="
echo "NAVTEXT BENCH: $P passed, $F failed"
echo "  artifacts: $OUT"
[ "$F" = "0" ] || echo "  [!] $F assertion(s) failed -- do not ship this build"
echo "===================================================================="
exit $([ "$F" = "0" ] && echo 0 || echo 1)
