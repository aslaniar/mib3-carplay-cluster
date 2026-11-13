#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# ============================================================================
# run_pathb.sh -- PATH B BENCH: the muxer as a LIVE component.
#
# What it pins.  Each assertion is written so it FAILS on the old behaviour -- T4
# runs the old framing on purpose and must not satisfy T3:
#
#   T1  native build, -Wall -Wextra clean
#   T2  unit build (aarch64, unit glibc).  Build only, and only with R8830L_ROOTFS;
#       the run under the unit's own ld-2.23.so is the optional last leg of T12,
#       which also needs Apple's `container`
#   T3  FRAMING: shim-style input folds to one PES per frame, params in that PES
#   T4  ...and the assertion is not vacuous: --no-fold (old behaviour) fails it
#   T5  ES stays byte-exact through the fold (no byte is rewritten)
#   T6  RECONNECT: one process survives 2 connections; RTP seq continuous across
#       the boundary; PSI re-emitted for the new connection
#   T7  SPORT: --sport binds, and the datagrams really leave from that port
#   T8  SPORT taken -> loud failure (exit 3), never a silent shared group
#   T9  --sport auto refuses rather than guessing
#   T10 AU socket is 0777 (carplay is uid 986; the label alone is not enough)
#   T11 msport.py: single / absent / changed / ambiguous fake /proc fixtures
#   T12 FILE MODE unchanged (independent RTP/TS verification still passes)
#   T26 mib3wheel --selftest, built natively with -DHOST_TEST
#   T27 fake_shim.py retries a connect that fails with a Python 2 socket.error
#
# Desk only.  No car, no unit.  Usage: bash run_pathb.sh
# ============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
SHIMDIR="$REPO/src/shim"
TOOLS="$REPO/tools"
KIT="$REPO/src/control"
SRC="$REPO/src/sender/mib3clusterstream.c"
ROOT="$REPO/R8830L_ROOTFS"
FIX="$REPO/tests/fixtures/pathb_base.264"
OUT="${PATHB_OUT:-$REPO/tests/build}"
CC=${CC:-cc}
LLVMCC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
READELF=/opt/homebrew/opt/llvm/bin/llvm-readelf
PY=python3
mkdir -p "$OUT"
# AF_UNIX paths are capped at 104 bytes on macOS, so sockets never live under $OUT.
SOCKDIR="$(mktemp -d /tmp/mib3.XXXXXX)"

P=0; F=0; S=0
ok(){ echo "PATHB $1 exp=$2 got=$2 PASS"; P=$((P+1)); }
no(){ echo "PATHB $1 exp=$2 got=$3 FAIL"; F=$((F+1)); }
sk(){ echo "PATHB $1 SKIP ($2)"; S=$((S+1)); }
chk(){ if [ "$2" = "1" ]; then ok "$1" "$3"; else no "$1" "$3" "${4:-no}"; fi; }

CLEANUP_PIDS=""
cleanup(){
  for p in $CLEANUP_PIDS; do kill "$p" 2>/dev/null; done
  rm -f "$SOCKDIR"/* 2>/dev/null
}
trap 'cleanup; rm -rf "$SOCKDIR"' EXIT INT TERM
track(){ CLEANUP_PIDS="$CLEANUP_PIDS $1"; }

NAT="$OUT/mib3clusterstream.native"
UNIT="$OUT/mib3clusterstream.unit"

echo "############ T1: native build (-Wall -Wextra) ############"
if $CC -O2 -Wall -Wextra -o "$NAT" "$SRC" 2>"$OUT/build.log"; then
  if [ -s "$OUT/build.log" ]; then
    no native_build_clean "no warnings" "$(head -2 "$OUT/build.log" | tr '\n' ' ')"
  else ok native_build_clean "no warnings"; fi
else
  no native_build_clean "no warnings" "$(tail -2 "$OUT/build.log" | tr '\n' ' ')"
fi

echo; echo "############ T2: unit build (aarch64 + unit glibc) ############"
if [ ! -d "$ROOT" ]; then
  sk unit_build_glibc "R8830L_ROOTFS not present: the aarch64/unit-glibc leg needs the firmware's lib64, which is not distributed with this repository"
elif "$LLVMCC" --target=aarch64-linux-gnu -DMIB3_UNIT_AARCH64 -O2 -nostdlib -fPIE \
        -fuse-ld="$LLD" -pie -Wl,-e,_start \
        -Wl,--dynamic-linker,/lib64/ld-linux-aarch64.so.1 \
        -o "$UNIT" "$SRC" "$ROOT/lib64/libc.so.6" 2>"$OUT/unitbuild.log"; then
  GVER="$("$READELF" -V "$UNIT" | grep -oE 'GLIBC_[0-9.]+' | sort -u | tail -1)"
  case "$GVER" in GLIBC_2.1[0-9]|GLIBC_2.2[0-3]) ok unit_build_glibc "<=2.23" ;; *) no unit_build_glibc "<=2.23" "$GVER" ;; esac
else
  no unit_build_glibc "<=2.23" "$(tail -1 "$OUT/unitbuild.log")"
fi

# ---------------------------------------------------------------- framing ---
# Run the muxer in unix mode against a shim-faithful feeder, then look at PES.
frame_case(){   # $1=tag  $2=extra muxer args
  local tag="$1"; shift
  local sock="$SOCKDIR/$tag" dg="$OUT/$tag.dgrams"
  rm -f "$sock" "$dg"
  "$NAT" --in "unix:$sock" --out "file:$dg" "$@" 2>"$OUT/$tag.mux.log" &
  local mp=$!; track $mp
  for _ in $(seq 1 40); do [ -S "$sock" ] && break; sleep 0.05; done
  $PY "$KIT/pathb/fake_shim.py" "$FIX" 40 "$sock" --es-out "$OUT/$tag.fed.264" \
      >"$OUT/$tag.feed.log" 2>&1
  sleep 0.5
  kill $mp 2>/dev/null; wait $mp 2>/dev/null
  echo "$dg"
}

echo; echo "############ T3: FRAMING -- params ride ONLY the IDR access units ############"
# THE CONTRACT (2026-10-27, measured on the car from BOTH streams this ECU has
# ever decoded):
#     the car's own nav      -> (5,7,8) x17, (1,) x64     params on IDRs only
#     our known-good pattern -> (5,7,8) x5,  (1,) x41     params on IDRs only
# A stream with SPS+PPS in front of EVERY frame does not render: the decoder takes
# the opening IDR, then re-initialises on every following frame and discards its
# reference pictures, so the picture never updates.  That was the 2026-10-27 field
# defect, and the assertion below USED to demand the broken behaviour ("50 of 50
# PES carry params") -- the bench enshrined the bug instead of catching it.
DG=$(frame_case fold)
$PY "$TOOLS/pes_frames.py" "$DG" --json >"$OUT/fold.json" 2>"$OUT/fold.err"
J(){ $PY -c "import json,sys;print(json.load(open('$OUT/fold.json')).get('$1'))" 2>/dev/null; }
PES=$(J pes); VCL=$(J vcl_pes); PARAM=$(J param_pes); BOTH=$(J vcl_with_sps_pps)
[ "$PES" = "50" ] && ok framing_pes_per_frame 50 || no framing_pes_per_frame 50 "$PES"
[ "$VCL" = "50" ] && ok framing_all_vcl 50 || no framing_all_vcl 50 "$VCL"
[ "$PARAM" = "0" ] && ok framing_no_param_only_pes 0 || no framing_no_param_only_pes 0 "$PARAM"

# The invariant, stated positively: params appear EXACTLY on the IDR-bearing PES.
# `pes_extract.py` computes it independently of the fold, straight off the wire.
$PY "$TOOLS/pes_extract.py" "$DG" --brief >"$OUT/fold.brief" 2>/dev/null
BR=$(cat "$OUT/fold.brief" 2>/dev/null)
case "$BR" in
  *framing=ok*) ok framing_params_only_on_idr "framing=ok" ;;
  *)            no framing_params_only_on_idr "framing=ok" "${BR:-no output}" ;;
esac
IDRN=$($PY -c "import re,sys;print((re.search(r'idr=(\d+)',sys.argv[1]) or [0,'0'])[1])" "$BR" 2>/dev/null)
[ "${IDRN:-0}" -ge 1 ] && ok framing_idr_present ">=1" || no framing_idr_present ">=1" "${IDRN:-0}"
[ "${IDRN:-0}" -lt 50 ] && ok framing_most_frames_are_bare "<idr count" || no framing_most_frames_are_bare "<idr count" "${IDRN:-0}"
echo "  $BR"
echo "  ($($PY "$TOOLS/pes_frames.py" "$DG" --quiet 2>/dev/null || true))"

echo; echo "############ T4: the assertion is not vacuous (old framing must fail) ############"
DG0=$(frame_case nofold --no-fold)
$PY "$TOOLS/pes_extract.py" "$DG0" --brief >"$OUT/nofold.brief" 2>"$OUT/nofold.err"
BR0=$(cat "$OUT/nofold.brief" 2>/dev/null)
case "$BR0" in
  *framing=suspect*) ok old_framing_detected "framing=suspect" ;;
  *)                 no old_framing_detected "framing=suspect" "${BR0:-no output}" ;;
esac
OLD_PARAM=$($PY "$TOOLS/pes_frames.py" "$DG0" --json 2>/dev/null | \
            $PY -c "import json,sys;print(json.load(sys.stdin)['param_pes'])" 2>/dev/null)
[ "${OLD_PARAM:-0}" -gt 0 ] && ok old_framing_has_param_only_pes ">0" || no old_framing_has_param_only_pes ">0" "${OLD_PARAM:-0}"
echo "  old behaviour: $BR0"

echo; echo "############ T5: ES survives the fold byte-exact ############"
# The old assertion was cmp(fed, wire).  That is no longer the contract and never
# was: the fold deliberately DROPS the repeated sequence headers the shim stamps
# before every AU, so the wire is shorter than the feed by exactly those copies.
# The honest statement is: strip every parameter-set NAL from both sides and the
# remaining bytes must be identical -- nothing is rewritten, nothing is lost, and
# the only thing removed is redundancy.  (A 183-byte TS tail used to lose one
# payload byte here; that is covered by T5b.)
$PY "$TOOLS/pathb_report.py" "$DG" --extract "$OUT/fold.es.264" >"$OUT/fold.report" 2>&1
# State the contract in the only form that is unambiguous: compare the sequence of
# (NAL type, payload) with the parameter sets removed, using a normalised start
# code.  Comparing raw bytes instead makes the result depend on whether a start
# code happened to be 3 or 4 bytes long -- which is exactly the kind of accident
# that made the first version of this assertion fail on identical payloads.
strip_ps(){ $PY - "$1" "$2" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
n = len(d)
marks, i = [], 0
while i + 3 <= n:
    if d[i] == 0 and d[i+1] == 0 and d[i+2] == 1:
        marks.append(i)
        i += 3
    else:
        i += 1
out = bytearray()
for k, s in enumerate(marks):
    h = s + 3
    if h >= n:
        continue
    t = d[h] & 0x1F
    if t in (7, 8):                       # parameter sets: the fold's only removal
        continue
    e = marks[k+1] if k + 1 < len(marks) else n
    body = d[h:e]
    while body.endswith(b'\x00'):         # trailing zero pad is not payload
        body = body[:-1]
    out += b'\x00\x00\x00\x01' + bytes([d[h]]) + body[1:]
open(sys.argv[2], 'wb').write(bytes(out))
PY
}
strip_ps "$OUT/fold.fed.264" "$OUT/fed.nops"
strip_ps "$OUT/fold.es.264"  "$OUT/wire.nops"
if cmp -s "$OUT/fed.nops" "$OUT/wire.nops"; then
  ok es_byte_exact_after_ps_removal "$(wc -c < "$OUT/wire.nops" | tr -d ' ')B"
else
  no es_byte_exact_after_ps_removal "identical" \
     "differs ($(wc -c < "$OUT/fed.nops" | tr -d ' ') vs $(wc -c < "$OUT/wire.nops" | tr -d ' ') B)"
fi
# and the header must NOT have been silently dropped altogether
HDR=$($PY "$TOOLS/pes_extract.py" "$DG" --brief 2>/dev/null)
case "$HDR" in
  *params=[1-9]*) ok framing_header_survived "params>0" ;;
  *)              no framing_header_survived "params>0" "${HDR:-no output}" ;;
esac
grep -q "0 discontinuities" "$OUT/fold.report" && ok rtp_seq_continuous 0 || no rtp_seq_continuous 0 "$(grep 'discontin' "$OUT/fold.report")"

echo; echo "############ T5b: every AU ends on a datagram boundary (MS-S format) ############"
# Measured on the car's own stream: 16 of 1586 datagrams straddle an AU boundary
# (1%, the PSI insertions) and 76 are padded with TS null packets.  Ours straddled
# in 1669 of 8948 (19%) and padded none, which meant the last AU of every burst
# sat on the wire incomplete until the next AU arrived.  This pins the fix.
$PY "$TOOLS/au_boundary.py" "$DG" >"$OUT/boundary.txt" 2>&1
BS=$(cat "$OUT/boundary.txt" 2>/dev/null | tr '\n' ' ')
STRAD=$(echo "$BS" | sed -n 's/.*straddle=\([0-9]*\).*/\1/p')
DGS=$(echo "$BS" | sed -n 's/.*datagrams=\([0-9]*\).*/\1/p')
NUL=$(echo "$BS" | sed -n 's/.*nulls=\([0-9]*\).*/\1/p')
# The threshold is a RATE, not a count: straddling can only come from the 500 ms
# PSI insertion, so the absolute number scales with how long the bench fed for.
# MS-S measures 1%; 5% is a generous ceiling that still fails the old 19%.
if [ -n "${STRAD:-}" ] && [ -n "${DGS:-}" ] && [ "$DGS" -gt 0 ] \
   && [ $((STRAD * 100)) -le $((DGS * 5)) ]; then
  ok au_ends_on_datagram_boundary "<=5%"
else
  no au_ends_on_datagram_boundary "<=5%" "${BS:-no output}"
fi
[ -n "${NUL:-}" ] && [ "${NUL:-0}" -gt 0 ] \
  && ok au_null_padding_present ">0" || no au_null_padding_present ">0" "${NUL:-0}"
[ -n "${DGS:-}" ] && [ "${DGS:-0}" -gt 0 ] && echo "$BS" | grep -q 'sizes=\[[0-9]*x[0-9]*\]' \
  && ok au_datagram_size_fixed "one size" || no au_datagram_size_fixed "one size" "$(echo "$BS" | sed -n 's/.*\(sizes=\[[^]]*\]\).*/\1/p')"
echo "  $BS"

echo; echo "############ T5c: a 183-byte TS tail must not lose a byte ############"
# A payload of exactly 183 bytes has no legal TS encoding (afc=01 needs 184; an
# adaptation field leaves 182).  The old pack_ts computed afb=0, forced it to 1,
# and copied 183 bytes at offset 6 -- one byte past a 188-byte stack array, with
# the last payload byte dropped.  AUs whose length leaves a 183-byte tail are
# L = 162 + 184k + 183, so build exactly those and round-trip them.
$PY - >"$OUT/tail183.264" <<'PY'
out = bytearray()
for k in range(6):
    L = 345 + 184 * k
    hdr = b'\x00\x00\x00\x01\x61'          # start code + non-IDR slice
    out += hdr + bytes([0xAA]) * (L - len(hdr))
import sys
sys.stdout.buffer.write(bytes(out))
PY
"$NAT" --in "$OUT/tail183.264" --out "file:$OUT/tail183.dgrams" 2>"$OUT/tail183.log"
$PY "$TOOLS/cluster_rtp_ts.py" "$OUT/tail183.dgrams" --extract "$OUT/tail183.rt.264" \
    --expect-frames 6 >"$OUT/tail183.verify" 2>&1
if cmp -s "$OUT/tail183.264" "$OUT/tail183.rt.264"; then
  ok ts_183_byte_tail_lossless "6 AUs exact"
else
  no ts_183_byte_tail_lossless "6 AUs exact" \
     "differs at $(cmp "$OUT/tail183.264" "$OUT/tail183.rt.264" 2>&1 | sed 's/.*char //;s/,.*//')"
fi


SOCK="$SOCKDIR/recon"; DG2="$OUT/recon.dgrams"; rm -f "$SOCK" "$DG2"
"$NAT" --in "unix:$SOCK" --out "file:$DG2" 2>"$OUT/recon.mux.log" &
MP=$!; track $MP
for _ in $(seq 1 40); do [ -S "$SOCK" ] && break; sleep 0.05; done
$PY "$KIT/pathb/fake_shim.py" "$FIX" 40 "$SOCK" >/dev/null 2>&1
$PY "$KIT/pathb/fake_shim.py" "$FIX" 40 "$SOCK" >/dev/null 2>&1
sleep 0.5
kill -0 $MP 2>/dev/null && chk survived_two_connections 1 alive || chk survived_two_connections 0 alive dead
kill $MP 2>/dev/null; wait $MP 2>/dev/null
$PY "$TOOLS/pathb_report.py" "$DG2" >"$OUT/recon.report" 2>&1
grep -q "0 discontinuities" "$OUT/recon.report" && ok seq_continuous_across_reconnect 0 || no seq_continuous_across_reconnect 0 "$(grep 'discontin' "$OUT/recon.report")"
# PSI must be re-emitted for each NEW connection so a receiver which joined late
# re-acquires the program map.  The old assertion was a bare ">= 8 PATs", which was
# a function of how long the bench happened to feed for and drifted as soon as the
# sender's flush behaviour changed.  The contract is: at least one PAT per
# connection, and PSI present at the head of the stream.
CONNS=$(grep -c 'conn open' "$OUT/recon.mux.log" 2>/dev/null)
PAT=$(grep -o 'TS PIDs: {0: [0-9]*' "$OUT/recon.report" | grep -o '[0-9]*$')
[ "${PAT:-0}" -ge "${CONNS:-1}" ] \
  && ok psi_reemitted_per_connection ">= conns(${CONNS:-1})" \
  || no psi_reemitted_per_connection ">= conns(${CONNS:-1})" "${PAT:-0}"
$PY "$TOOLS/au_boundary.py" "$DG2" --quiet 2>/dev/null | head -1 >"$OUT/recon.psi"
FIRST=$($PY - "$DG2" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
o = 12 if (d[0] >> 6) == 2 else 0            # skip the RTP header if present
pid = ((d[o+1] & 0x1F) << 8) | d[o+2]
print(pid)
PY
)
[ "${FIRST:-x}" = "0" ] && ok psi_at_stream_head "PID 0 first" || no psi_at_stream_head "PID 0 first" "first PID ${FIRST:-?}"
FR=$($PY "$TOOLS/pes_frames.py" "$DG2" --json | $PY -c "import json,sys;print(json.load(sys.stdin)['vcl_pes'])" 2>/dev/null)
[ "${FR:-0}" -ge 100 ] && ok frames_across_reconnect ">=100" || no frames_across_reconnect ">=100" "$FR"
echo "  $(grep -E 'conn open|conn close' "$OUT/recon.mux.log" | tr '\n' ' ')"

echo; echo "############ T6b: a feed FASTER than the pacer must not lose its tail ############"
# The pacer holds up to 8 access units.  Closing the connection with them queued
# used to discard them silently -- measured on a real phone capture: 7 of 2077 AUs
# never reached the wire and nothing said so.  The reconnect test above cannot see
# that, because at 40 fps the feed is slower than the pacer and the queue never
# fills.  So feed as fast as the socket allows and require EVERY AU to arrive.
SOCKF="$SOCKDIR/fast"; DGF="$OUT/fast.dgrams"; rm -f "$SOCKF" "$DGF"
"$NAT" --in "unix:$SOCKF" --out "file:$DGF" --pace-ms 40 2>"$OUT/fast.mux.log" &
FP=$!; track $FP
for _ in $(seq 1 40); do [ -S "$SOCKF" ] && break; sleep 0.05; done
$PY "$KIT/pathb/fake_shim.py" "$FIX" 5000 "$SOCKF" >"$OUT/fast.feed.log" 2>&1
FEDN=$(sed -n 's/.*frames \([0-9]*\)$/\1/p' "$OUT/fast.feed.log" | head -1)
sleep 2.0
kill $FP 2>/dev/null; wait $FP 2>/dev/null
WIREN=$($PY "$TOOLS/pes_extract.py" "$DGF" --brief 2>/dev/null | sed -n 's/.*pes=\([0-9]*\).*/\1/p')
[ -n "${WIREN:-}" ] && [ "${WIREN:-0}" = "${FEDN:-x}" ] \
  && ok pacer_drains_on_close "${FEDN:-?} AUs" || no pacer_drains_on_close "${FEDN:-?} AUs" "${WIREN:-0}"
grep -q "drained-at-close=" "$OUT/fast.mux.log" && ok pacer_reports_drain yes || no pacer_reports_drain yes no
echo "  fed=$FEDN  on-wire=$WIREN  $(grep -o 'drained-at-close=[0-9]*' "$OUT/fast.mux.log" | head -1)"

cat > "$OUT/mcaster.py" <<'PY'
import socket, struct, sys
port = int(sys.argv[1]); out = sys.argv[2]; cap = sys.argv[3] if len(sys.argv) > 3 else None
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
s.bind(("::", port))
mreq = socket.inet_pton(socket.AF_INET6, "ff14::2:4") + struct.pack("@I", socket.if_nametoindex("lo0"))
s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, mreq)
s.settimeout(8); seen = {}; cf = open(cap, "wb") if cap else None
try:
    while True:
        d, a = s.recvfrom(4096); seen[a[1]] = seen.get(a[1], 0) + 1
        if cf: cf.write(struct.pack(">I", len(d)) + d); cf.flush()
except Exception:
    pass
if cf: cf.close()
open(out, "w").write(repr(seen))
PY
cat > "$OUT/holder.py" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.bind(("::", int(sys.argv[1])))
time.sleep(10)
PY

echo; echo "############ T7: SPORT -- the datagrams really leave from that port ############"
SPORT=42989; GPORT=45999
rm -f "$OUT/sport.seen" "$OUT/sport.cap"
$PY "$OUT/mcaster.py" "$GPORT" "$OUT/sport.seen" "$OUT/sport.cap" >/dev/null 2>&1 &
LS=$!; track $LS
sleep 1.0
"$NAT" --in "$FIX" --dst "ff14::2:4%lo0" --port "$GPORT" --sport "$SPORT" --fps 10 --realtime \
       >"$OUT/sport.log" 2>&1
# the listener writes its summary on exit (8 s socket timeout), so wait for the file
for _ in $(seq 1 30); do [ -s "$OUT/sport.seen" ] && break; sleep 0.5; done
SEEN=$(cat "$OUT/sport.seen" 2>/dev/null || echo "{}")
echo "$SEEN" | grep -q "$SPORT" && ok sport_bind_and_send "$SPORT" || no sport_bind_and_send "$SPORT" "$SEEN"
echo "  source ports observed: $SEEN"

# WIRE ROUND-TRIP: what left our socket, re-parsed on the far side, must be the
# elementary stream we were given -- byte for byte.
$PY "$TOOLS/pathb_report.py" "$OUT/sport.cap" --extract "$OUT/sport.es.264" \
    >"$OUT/sport.report" 2>&1
if cmp -s "$FIX" "$OUT/sport.es.264"; then
  ok onwire_es_roundtrip exact
else
  no onwire_es_roundtrip exact "$(grep -c . "$OUT/sport.es.264" 2>/dev/null)/$(wc -c < "$FIX" | tr -d ' ')"
fi
grep -q "0 discontinuities" "$OUT/sport.report" \
  && ok onwire_seq_continuous 0 || no onwire_seq_continuous 0 "$(grep 'discontin' "$OUT/sport.report")"

echo; echo "############ T8: SPORT taken -> loud failure ############"
BUSY=43007
$PY "$OUT/holder.py" "$BUSY" >/dev/null 2>&1 &
HOLD=$!; track $HOLD
sleep 1.0
"$NAT" --in "$FIX" --dst "ff14::2:4%lo0" --port "$GPORT" --sport "$BUSY" --fps 10 \
       >"$OUT/busy.log" 2>&1; RC=$?
kill $HOLD 2>/dev/null
[ "$RC" = "3" ] && ok sport_busy_exit 3 || no sport_busy_exit 3 "$RC"
grep -q "SPORT_BUSY" "$OUT/busy.log" && ok sport_busy_message SPORT_BUSY || no sport_busy_message SPORT_BUSY "$(head -1 "$OUT/busy.log")"

echo; echo "############ T9: --sport auto refuses to guess ############"
"$NAT" --in /dev/null --sport auto >"$OUT/auto.log" 2>&1; RC=$?
[ "$RC" = "2" ] && ok sport_auto_refused 2 || no sport_auto_refused 2 "$RC"
grep -q "msport.py" "$OUT/auto.log" && ok sport_auto_points_at_msport yes || no sport_auto_points_at_msport yes no

echo; echo "############ T9b: --probe on a DARK group must RETURN (wall-clock bound) ############"
# `pathb_ctl verify` runs the probe in the FOREGROUND now.  A blocking read() on a
# silent group never returns, so a wall-clock bound that is only checked AFTER a
# read is not a bound at all -- verify would hang for ever on a dark group instead
# of saying "no video".  The wait therefore has a timeout.
T0=$(date +%s)
"$NAT" --probe --dst "ff14::2:4%lo0" --port 46998 --probe-secs 1 --probe-video-only \
       --out "file:$OUT/dark.bin" >"$OUT/dark.log" 2>&1
RC=$?; T1=$(date +%s); EL=$((T1 - T0))
[ "$EL" -le 5 ] && ok probe_returns_on_dark_group "<=5s (rc=$RC)" \
  || no probe_returns_on_dark_group "<=5s" "${EL}s (it hung)"
grep -q "window elapsed" "$OUT/dark.log" && ok probe_reports_window yes || no probe_reports_window yes no
echo "  elapsed ${EL}s, rc=$RC"

echo; echo "############ T10: AU socket is 0777 ############"
SOCK3="$SOCKDIR/hyg"; rm -f "$SOCK3"
"$NAT" --in "unix:$SOCK3" --out "file:$OUT/hyg.dgrams" 2>/dev/null &
HP=$!; track $HP
for _ in $(seq 1 40); do [ -S "$SOCK3" ] && break; sleep 0.05; done
MODE=$(perl -e 'printf "%04o", (stat($ARGV[0]))[2] & 07777' "$SOCK3" 2>/dev/null)
[ "$MODE" = "0777" ] && ok au_socket_mode 0777 || no au_socket_mode 0777 "$MODE"
kill $HP 2>/dev/null; wait $HP 2>/dev/null

echo; echo "############ T11: msport.py against fake /proc trees ############"
MS="$KIT/pathb/msport.py"
mkfix(){   # $1=dir  $2="inode:porthex" ...   ; env MK_PID=yes|no
  local d="$1"; shift
  rm -rf "$d"; mkdir -p "$d/net"
  if [ "${MK_PID:-yes}" = "yes" ]; then
    mkdir -p "$d/4242/fd"
    printf '/usr/bin/multi-screen-server\x00--cfg\x00' > "$d/4242/cmdline"
  fi
  {
    echo "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode"
    for pair in "$@"; do
      ino=${pair%%:*}; phex=${pair##*:}
      echo "   0: 00000000000000000000000000000000:$phex 00000000000000000000000000000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 $ino 2 0000"
    done
  } > "$d/net/udp6"
}
# fd sockets are created separately so a fixture can hold a socket the table does not list
mkfd(){ ln -sf "socket:[$2]" "$1/4242/fd/$2"; }

MK_PID=yes mkfix "$OUT/proc.ok"       55555:A7ED
MK_PID=yes mkfd   "$OUT/proc.ok" 55555

MK_PID=yes mkfix "$OUT/proc.moved"    55555:9C40
MK_PID=yes mkfd   "$OUT/proc.moved" 55555

MK_PID=yes mkfix "$OUT/proc.amb"      55555:A7ED 55556:9C40
MK_PID=yes mkfd   "$OUT/proc.amb" 55555
MK_PID=yes mkfd   "$OUT/proc.amb" 55556

MK_PID=no  mkfix "$OUT/proc.none"     55555:A7ED
mkdir -p "$OUT/proc.none"            # no pid dir at all

# process exists, holds a socket, but its inode is absent from the table
MK_PID=yes mkfix "$OUT/proc.unrelated" 55555:A7ED
MK_PID=yes mkfd   "$OUT/proc.unrelated" 99999

GOT=$($PY "$MS" --proc "$OUT/proc.ok" 2>/dev/null)
[ "$GOT" = "42989" ] && ok msport_single 42989 || no msport_single 42989 "$GOT"
GOT=$($PY "$MS" --proc "$OUT/proc.moved" 2>/dev/null)
[ "$GOT" = "40000" ] && ok msport_changed_port 40000 || no msport_changed_port 40000 "$GOT"
$PY "$MS" --proc "$OUT/proc.none" >/dev/null 2>&1; RC=$?
[ "$RC" = "2" ] && ok msport_no_process 2 || no msport_no_process 2 "$RC"
$PY "$MS" --proc "$OUT/proc.amb" >/dev/null 2>&1; RC=$?
[ "$RC" = "3" ] && ok msport_ambiguous 3 || no msport_ambiguous 3 "$RC"
$PY "$MS" --proc "$OUT/proc.unrelated" >/dev/null 2>&1; RC=$?
[ "$RC" = "2" ] && ok msport_no_matching_socket 2 || no msport_no_matching_socket 2 "$RC"
$PY "$MS" --proc "$OUT/proc.ok" --json 2>/dev/null | grep -q '"port": 42989' \
  && ok msport_json yes || no msport_json yes no

echo; echo "############ T12: FILE MODE still verified (no regression) ############"
cleanup
$PY "$TOOLS/mk_h264.py" 6 320 240 >"$OUT/frames.264" 2>/dev/null
"$NAT" --in "$OUT/frames.264" --out "file:$OUT/file.dgrams" --fps 30 2>/dev/null
if $PY "$TOOLS/cluster_rtp_ts.py" "$OUT/file.dgrams" --extract "$OUT/file.rt.264" \
      --expect-frames 6 >"$OUT/file.verify" 2>&1; then
  ok file_mode_verified "VERIFY OK"
else
  no file_mode_verified "VERIFY OK" "$(tail -1 "$OUT/file.verify")"
fi
cmp -s "$OUT/frames.264" "$OUT/file.rt.264" && ok file_mode_roundtrip exact || no file_mode_roundtrip exact differs

if command -v container >/dev/null 2>&1 && [ -x "$UNIT" ]; then
  container run --rm --mount type=bind,source="$ROOT",target=/mnt/rootfs,readonly \
    --mount type=bind,source="$OUT",target=/out ubuntu:24.04 /bin/bash -c '
      LP="/mnt/rootfs/lib64:/mnt/rootfs/usr/lib64:/mnt/rootfs/lib:/mnt/rootfs/usr/lib"
      /mnt/rootfs/lib64/ld-2.23.so --library-path "$LP" /out/mib3clusterstream.unit \
         --in /out/frames.264 --out file:/out/unit.dgrams --fps 30 >/dev/null 2>&1; echo rc=$?' \
    >"$OUT/unitrun.log" 2>&1
  if $PY "$TOOLS/cluster_rtp_ts.py" "$OUT/unit.dgrams" --quiet >/dev/null 2>&1; then
    ok unit_binary_under_unit_loader verified
  else
    no unit_binary_under_unit_loader verified "$(tail -1 "$OUT/unitrun.log")"
  fi
else
  sk unit_binary_under_unit_loader "needs the T2 unit build and Apple's container CLI"
fi

# T13-T16 tested the retired shell controller (live_kit/pathb_ctl.sh).  That
# controller is superseded by pathb_daemon.sh, whose own bench (run_pathb_daemon.sh,
# D1-D15) asserts the same ordering and failure invariants.  Archived 2026-11-02.

echo; echo "############ T17-T18: START GATE -- hold until a decodable IDR ############"
# a mid-GOP fixture: SPS/PPS kept, the first IDR removed, so the first VCL is a P-slice
$PY - "$FIX" "$OUT/midgop.264" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
nals = []; i = 0
while i + 3 < len(d):
    if d[i] == 0 and d[i+1] == 0 and d[i+2] == 1:
        h = i + 3
        if h < len(d): nals.append((i, d[h] & 0x1f))
        i = h
    else: i += 1
idrs = [k for k, (o, t) in enumerate(nals) if t == 5]
a = idrs[0]
open(sys.argv[2], "wb").write(d[nals[a-2][0]:nals[a][0]] + d[nals[a+1][0]:])
PY
gate_case(){   # $1=tag  $2..=extra muxer args
  local tag="$1"; shift
  local sock="$SOCKDIR/$tag" dg="$OUT/$tag.dgrams"
  rm -f "$sock" "$dg"
  "$NAT" --in "unix:$sock" --out "file:$dg" "$@" 2>"$OUT/$tag.mux.log" &
  local mp=$!; track $mp
  for _ in $(seq 1 40); do [ -S "$sock" ] && break; sleep 0.05; done
  $PY "$KIT/pathb/fake_shim.py" "$OUT/midgop.264" 60 "$sock" >/dev/null 2>&1
  sleep 0.4; kill $mp 2>/dev/null; wait $mp 2>/dev/null
  echo "$dg"
}
DG_GATE=$(gate_case gate)
$PY "$TOOLS/pes_frames.py" "$DG_GATE" --json >"$OUT/gate.json" 2>/dev/null
FIRST_NAL=$($PY -c "import json;print(json.load(open('$OUT/gate.json'))['frames'][0]['nal'])" 2>/dev/null)
echo "$FIRST_NAL" | grep -q "5" && ok gate_starts_on_idr "first PES carries an IDR" || no gate_starts_on_idr "IDR first" "$FIRST_NAL"
grep -q "gate: waiting for a decodable start" "$OUT/gate.mux.log" 2>/dev/null \
  && ok gate_held_p_slices yes || no gate_held_p_slices yes "no hold logged"
grep -q "IDR in hand" "$OUT/gate.mux.log" 2>/dev/null && ok gate_announces_start yes || no gate_announces_start yes no
echo "  gate ON: first PES nal types $FIRST_NAL, log: $(grep -c 'gate:' "$OUT/gate.mux.log" 2>/dev/null) gate lines"

DG_NOGATE=$(gate_case nogate --no-idr-gate)
$PY "$TOOLS/pes_frames.py" "$DG_NOGATE" --json >"$OUT/nogate.json" 2>/dev/null
NG_FIRST=$($PY -c "import json;print(json.load(open('$OUT/nogate.json'))['frames'][0]['nal'])" 2>/dev/null)
if echo "$NG_FIRST" | grep -q "5"; then
  no gate_is_not_vacuous "P-slice first with gate off" "still IDR-first, so T17 proves nothing"
else
  ok gate_is_not_vacuous "P-slice first with gate off"
fi
echo "  gate OFF: first PES nal types $NG_FIRST"

# T19-T22 tested the retired controller's pre-flight refusals.  The supervisor
# owns that behaviour now (single instance, pause marker, port ownership) and
# run_pathb_daemon.sh asserts it: D8 pause, D9 single instance, D15 busy port.

echo "############ T23-T25: MONITOR mode -- the port is taken on command ############"
# The resident supervisor's whole contract: own the AU socket from boot, take the
# cluster's UDP port only when told, and give it back on command.  Nothing here
# needs a phone: the control file is the interface.
# The sender caps --ctl and --conn-state at 111 bytes, so they share the short dir.
MON="$SOCKDIR/mon"; MONLOG="$OUT/mon.log"
rm -f "$MON".state "$MON".ctl "$MON".sock "$MONLOG"
"$NAT" --in "unix:$MON.sock" --dst "ff14::2:4%lo0" --port "$GPORT" --sport "$SPORT" \
       --monitor --conn-state "$MON.state" --ctl "$MON.ctl" >"$MONLOG" 2>&1 &
MONPID=$!; track $MONPID
sleep 0.6
chk mon_starts_unbound "$(grep -q 'monitor: AU socket only' "$MONLOG" && echo 1 || echo 0)" "no bind at start"
chk mon_does_not_grab "$(grep -q 'bound source port' "$MONLOG" && echo 0 || echo 1)" "port untouched"
chk mon_state_unbound "$(sed -n 's/.*bound=\([0-9]*\).*/\1/p' "$MON.state" 2>/dev/null | grep -qx 0 && echo 1 || echo 0)" "bound=0"

printf 'bind %s\n' "$SPORT" >"$MON.ctl"
_i=0; while [ "$_i" -lt 40 ]; do
  grep -q "PORT OURS" "$MONLOG" 2>/dev/null && break; sleep 0.1; _i=$((_i+1)); done
chk mon_bind_on_command "$(grep -q 'PORT OURS' "$MONLOG" && echo 1 || echo 0)" "bound after 'bind'"
chk mon_state_bound "$(sed -n 's/.*bound=\([0-9]*\).*/\1/p' "$MON.state" 2>/dev/null | grep -qx "$SPORT" && echo 1 || echo 0)" "bound=$SPORT"

printf 'unbind\n' >"$MON.ctl"
_i=0; while [ "$_i" -lt 40 ]; do
  grep -q "port released" "$MONLOG" 2>/dev/null && break; sleep 0.1; _i=$((_i+1)); done
chk mon_unbind_on_command "$(grep -q 'port released' "$MONLOG" && echo 1 || echo 0)" "released after 'unbind'"
chk mon_state_unbound_again "$(sed -n 's/.*bound=\([0-9]*\).*/\1/p' "$MON.state" 2>/dev/null | grep -qx 0 && echo 1 || echo 0)" "bound=0"
# PROOF, not a log line: the port is actually free for somebody else.
$PY - "$SPORT" <<'PYBIND' >/dev/null 2>&1 && ok mon_port_really_free yes || no mon_port_really_free yes "still bound"
import socket, sys
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.bind(("::", int(sys.argv[1])))
s.close()
PYBIND
kill "$MONPID" 2>/dev/null

echo; echo "############ T26: mib3wheel --selftest (HOST_TEST build) ############"
if "$CC" -DHOST_TEST -O2 -Wall -Wextra -o "$OUT/mib3wheel.host" "$REPO/src/wheel/mib3wheel.c" \
       2>"$OUT/wheel.build.log"; then
  "$OUT/mib3wheel.host" --selftest >"$OUT/wheel.selftest.log" 2>&1
  WRC=$?
  sed 's/^/  /' "$OUT/wheel.selftest.log" | grep -E 'FAIL|TOTAL'
  [ "$WRC" -eq 0 ] && ok wheel_selftest "rc=0" || no wheel_selftest "rc=0" "rc=$WRC"
else
  no wheel_selftest "rc=0" "$(tail -1 "$OUT/wheel.build.log")"
fi

echo; echo "############ T27: fake_shim.py retries a Python 2 socket.error ############"
# The unit runs fake_shim.py on Python 2.7, where a refused connect raises
# socket.error, which is not an OSError.  Python 3 cannot raise one, so model it:
# the first two connects raise a socket.error outside the OSError tree.
SOCKP="$SOCKDIR/py2err"; DGP="$OUT/py2err.dgrams"; rm -f "$SOCKP" "$DGP"
"$NAT" --in "unix:$SOCKP" --out "file:$DGP" 2>"$OUT/py2err.mux.log" &
PP=$!; track $PP
for _ in $(seq 1 40); do [ -S "$SOCKP" ] && break; sleep 0.05; done
$PY - "$KIT/pathb/fake_shim.py" "$FIX" 200 "$SOCKP" >"$OUT/py2err.feed.log" 2>&1 <<'PY'
import runpy, socket, sys
class Py2SocketError(Exception):
    pass
real = socket.socket
class Refusing(real):
    fails = 2
    def connect(self, addr):
        if Refusing.fails:
            Refusing.fails -= 1
            raise Py2SocketError(111, "Connection refused")
        return real.connect(self, addr)
socket.error = Py2SocketError
socket.socket = Refusing
sys.argv = sys.argv[1:]
runpy.run_path(sys.argv[0], run_name="__main__")
PY
RC=$?
kill $PP 2>/dev/null; wait $PP 2>/dev/null
grep -q '^sent 50 frames' "$OUT/py2err.feed.log" && [ "$RC" = "0" ] \
  && ok fake_shim_retries_socket_error "rc=0, sent 50 frames" \
  || no fake_shim_retries_socket_error "rc=0, sent 50 frames" "rc=$RC $(tail -1 "$OUT/py2err.feed.log")"

echo
echo "============================================================"
echo "Path B bench: PASS=$P FAIL=$F SKIP=$S   (artifacts in $OUT)"
echo "============================================================"
[ "$F" -eq 0 ]
