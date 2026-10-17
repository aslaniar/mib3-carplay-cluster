#!/bin/bash
# =============================================================================
# run_clusterstream_bench.sh -- Path B desk bench (2026-10-16)
#
# Proves, on the desk, that mib3clusterstream emits EXACTLY the container the
# cluster ECU expects, and that the same source builds + runs for the unit.
#
#   T1  native build
#   T2  unit build (aarch64, -nostdlib, the unit's own glibc 2.23)
#   T3  a valid H.264 test stream is generated (H264 320x240, I_PCM IDR frames)
#   T4  file mode: mux to a datagram file
#   T5  independent parse (cluster_rtp_ts.py): RTP PT=33, ssid=100000, PAT/PMT,
#       tsid 100, PMT pid 1000, PCR/ES pid 17, stream_type 0x1b, PTS monotone,
#       continuity counters continuous, CRC32 correct
#   T6  byte-exact round trip: reassembled ES == input ES
#   T7  tshark independently dissects RTP/PT=33 + MPEG-2 PAT/PMT + AVC (if tshark)
#   T8  unix mode: the shim's helper protocol (16-byte header, 1 AU per message)
#   T9  the aarch64 binary runs under the unit's own ld-2.23.so in an Apple
#       `container` (if `container` is present) and its output re-verifies
#
# Desk-only.  Never touches a car or a unit.  R8830L_ROOTFS is read-only.
# =============================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
ROOT="$REPO/R8830L_ROOTFS"
OUT="$REPO/MIB3_RE/data/PATHB_20261016"
CC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
READELF=/opt/homebrew/opt/llvm/bin/llvm-readelf
LIBC="$ROOT/lib64/libc.so.6"
mkdir -p "$OUT"

P=0; F=0
ok(){ echo "PBENCH $1 exp=$2 got=$2 PASS"; P=$((P+1)); }
no(){ echo "PBENCH $1 exp=$2 got=$3 FAIL"; F=$((F+1)); }

echo "############ T1: native build ############"
if cc -O2 -Wall -Wextra -o "$OUT/mib3clusterstream.native" "$HERE/mib3clusterstream.c" 2>"$OUT/native.build.log"; then
  ok native_build OK
else
  no native_build OK "$(tail -1 "$OUT/native.build.log")"
fi
NAT="$OUT/mib3clusterstream.native"

echo; echo "############ T2: unit build (aarch64 + unit glibc) ############"
if "$CC" --target=aarch64-linux-gnu -DMIB3_UNIT_AARCH64 -O2 -nostdlib -fPIE \
        -fuse-ld="$LLD" -pie -Wl,-e,_start \
        -Wl,--dynamic-linker,/lib64/ld-linux-aarch64.so.1 \
        -o "$OUT/mib3clusterstream.unit" "$HERE/mib3clusterstream.c" "$LIBC" \
        2>"$OUT/unit.build.log"; then
  ARCH="$("$READELF" -h "$OUT/mib3clusterstream.unit" | awk '/Machine/{print $2}')"
  NEED="$("$READELF" -d "$OUT/mib3clusterstream.unit" | awk '/NEEDED/{print $NF}' | tr -d '[]' | head -1)"
  GVER="$("$READELF" -V "$OUT/mib3clusterstream.unit" | grep -oE 'GLIBC_[0-9.]+' | sort -u | tail -1)"
  [ "$ARCH" = AArch64 ] && ok unit_build_aarch64 AArch64 || no unit_build_aarch64 AArch64 "$ARCH"
  [ "$NEED" = "libc.so.6" ] && ok unit_build_needs_unit_libc libc.so.6 || no unit_build_needs_unit_libc libc.so.6 "$NEED"
  case "$GVER" in GLIBC_2.1[0-9]|GLIBC_2.2[0-3]) ok unit_build_glibc_compat "<=GLIBC_2.23" ;; *) no unit_build_glibc_compat "<=GLIBC_2.23" "$GVER" ;; esac
else
  no unit_build_aarch64 AArch64 "$(tail -1 "$OUT/unit.build.log")"
fi

echo; echo "############ T3: generate a VALID H.264 test stream ############"
python3 "$HERE/mk_h264.py" 6 320 240 > "$OUT/frames.264" || { no h264_gen OK "mk_h264 failed"; }
NAL=$(python3 - "$OUT/frames.264" <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
print(len(d) and d.count(b'\x00\x00\x00\x01'))
PY
)
[ "${NAL:-0}" -ge 3 ] && ok h264_gen_valid "NALs>=$NAL" || no h264_gen_valid "NALs>=3" "$NAL"
echo "  frames.264 = $(wc -c < "$OUT/frames.264") bytes, $NAL NAL units"

echo; echo "############ T4: file mode mux ############"
"$NAT" --in "$OUT/frames.264" --out "file:$OUT/stream.dgrams" --fps 30 2>"$OUT/mux.log"
[ -s "$OUT/stream.dgrams" ] && ok mux_produced_bytes nonempty || no mux_produced_bytes nonempty "empty"

echo; echo "############ T5+T6: independent parse + round trip ############"
if python3 "$HERE/cluster_rtp_ts.py" "$OUT/stream.dgrams" --extract "$OUT/roundtrip.264" \
        --pcap "$OUT/stream.pcap" --expect-frames 6 > "$OUT/verify.json" 2>&1; then
  grep -q "VERIFY OK" "$OUT/verify.json" && ok verify_structure "VERIFY OK" || no verify_structure "VERIFY OK" "$(tail -1 "$OUT/verify.json")"
  cat "$OUT/verify.json"
else
  no verify_structure "VERIFY OK" "$(tail -2 "$OUT/verify.json")"
fi
if cmp -s "$OUT/frames.264" "$OUT/roundtrip.264"; then ok es_roundtrip_byte_exact exact
else no es_roundtrip_byte_exact exact "differ"; fi

echo; echo "############ T7: tshark independent dissection ############"
TSHARK=/Applications/Wireshark.app/Contents/MacOS/tshark
if [ -x "$TSHARK" ]; then
  T=$("$TSHARK" -r "$OUT/stream.pcap" -d udp.port==42800,rtp -c 2 -V 2>/dev/null)
  echo "$T" | grep -q "MPEG-II transport streams (33)" && ok tshark_rtp_pt33 "PT 33" || no tshark_rtp_pt33 "PT 33" "absent"
  echo "$T" | grep -q "Transport Stream ID: 0x0064" && ok tshark_pat_tsid100 0x0064 || no tshark_pat_tsid100 0x0064 "absent"
  echo "$T" | grep -q "AVC video stream" && ok tshark_pmt_avc "AVC 0x1b" || no tshark_pmt_avc "AVC 0x1b" "absent"
  "$TSHARK" -r "$OUT/stream.pcap" -d udp.port==42800,rtp -c 1 -T fields -e rtp.ssrc 2>/dev/null \
    | grep -q "0x000186a0" && ok tshark_ssrc_100000 0x000186a0 || no tshark_ssrc_100000 0x000186a0 "absent"
else
  echo "  (tshark not found -- skipping; not a failure)"
fi

echo; echo "############ T8: unix (helper-protocol) input mode ############"
python3 - "$OUT/frames.264" "$OUT/fake_shim.sock" <<'PY' &
import sys, socket, struct, time, os
frames=open(sys.argv[1],'rb').read()
# re-split into AUs the same way (4-byte start codes, SPS+PPS+IDR per AU)
parts=frames.split(b'\x00\x00\x00\x01')
aus=[]
cur=b''
for p in parts:
    if not p: continue
    cur += b'\x00\x00\x00\x01'+p
    if p[0] == 0x65:   # IDR closes the AU
        aus.append(cur); cur=b''
s=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(50):
    try: s.connect(sys.argv[2]); break
    except OSError: time.sleep(0.1)
for i,au in enumerate(aus):
    s.sendall(struct.pack('<IIQ', 0x4d334331, len(au), i)+au)
s.close()
PY
PY_PEER=$!
"$NAT" --in "unix:$OUT/fake_shim.sock" --out "file:$OUT/stream_unix.dgrams" >/dev/null 2>"$OUT/unix.log"
wait $PY_PEER 2>/dev/null
if python3 "$HERE/cluster_rtp_ts.py" "$OUT/stream_unix.dgrams" --quiet >/dev/null 2>&1; then
  ok unix_helper_protocol verified
else
  no unix_helper_protocol verified "$(tail -1 "$OUT/unix.log")"
fi

echo; echo "############ T9: the aarch64 build under the unit's own ld-2.23.so ############"
if command -v container >/dev/null 2>&1 && [ -x "$OUT/mib3clusterstream.unit" ]; then
  container run --rm \
    --mount type=bind,source="$ROOT",target=/mnt/rootfs,readonly \
    --mount type=bind,source="$OUT",target=/out \
    ubuntu:24.04 /bin/bash -c '
      LP="/mnt/rootfs/lib64:/mnt/rootfs/usr/lib64:/mnt/rootfs/lib:/mnt/rootfs/usr/lib"
      LD=/mnt/rootfs/lib64/ld-2.23.so
      $LD --library-path "$LP" /out/mib3clusterstream.unit --in /out/frames.264 --out file:/out/stream_unit.dgrams --fps 30
      echo "rc=$?"
    ' > "$OUT/container.log" 2>&1
  if python3 "$HERE/cluster_rtp_ts.py" "$OUT/stream_unit.dgrams" --quiet >/dev/null 2>&1; then
    ok unit_binary_runs_under_unit_loader verified
  else
    no unit_binary_runs_under_unit_loader verified "$(tr -d '\r' < "$OUT/container.log" | tail -2 | tr '\n' ' ')"
  fi
  tr -d '\r' < "$OUT/container.log" | grep -E 'access units|datagrams|rc=' | sed 's/^/  /'
else
  echo "  (container CLI or unit binary unavailable -- skipping T9)"
fi

echo; echo "############ T10: --probe mode (blind capture of an existing sender) ############"
"$NAT" --in "$OUT/frames.264" --dst "ff14::2:4%lo0" --port 45888 --fps 60 --loop >/dev/null 2>&1 &
SP=$!
sleep 0.6
"$NAT" --probe --dst "ff14::2:4%lo0" --port 45888 --probe-count 12 --out "file:$OUT/probe.bin" \
      >/dev/null 2>"$OUT/probe.log"
kill $SP 2>/dev/null; wait $SP 2>/dev/null
if python3 "$HERE/cluster_rtp_ts.py" "$OUT/probe.bin" --quiet --no-psi >/dev/null 2>&1; then
  ok probe_capture verified
else
  no probe_capture verified "$(tail -1 "$OUT/probe.log")"
fi
sed 's/^/  /' "$OUT/probe.log"

echo
echo "============================================================"
echo "Path B bench: PASS=$P FAIL=$F   (artifacts in $OUT)"
echo "============================================================"
[ "$F" -eq 0 ]
