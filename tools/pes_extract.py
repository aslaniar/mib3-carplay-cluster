#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""pes_extract.py -- read a cluster-group capture and report the VIDEO stream's shape.

WHY THIS EXISTS (2026-10-27).  `pathb_report.py` and `pes_frames.py` walk the capture at a
fixed datagram stride (1140 B).  This group also carries the PT=98 idle beacon (256 B) and
frames can end in short datagrams, so the first short datagram desynchronises the walk and the
"verdict" becomes nonsense.  That cost a capture and produced a false conclusion.

This tool does not trust datagram boundaries at all: it scans for 188-byte MPEG-TS sync runs
(0x47 every 188 bytes, at least 6 deep), rebuilds the video PES from PID 17, and reports what
matters for the cluster:

  * PES structure -- the parameter sets must ride ONLY the access unit that carries an IDR.
    Both streams this ECU has ever decoded look like:  (5,7,8) for keyframes, (1,) otherwise.
  * PTS deltas / gaps -- the phone bursts at ~60 fps and then goes quiet for up to ~840 ms;
    the unit's own encoder never pauses.  Gaps here are the starvation evidence.
  * optionally writes the Annex-B ES so it can be decoded with ffmpeg and compared to the glass.

Usage:
    pes_extract.py CAPTURE.bin [--es out.264] [--max-gap 150] [--quiet]

Exit code 0 = video found, 3 = no video in the capture.
"""
import sys
from collections import Counter

TS = 188
PID_VIDEO = 17


def ts_packets(d):
    """All TS packets that sit in a verified 188-byte sync run (datagram-agnostic)."""
    pkts, o, L = [], 0, len(d)
    while o + TS * 6 <= L:
        if d[o] == 0x47 and all(d[o + TS * k] == 0x47 for k in range(1, 6)):
            while o + TS <= L and d[o] == 0x47:
                pkts.append(d[o:o + TS])
                o += TS
        else:
            o += 1
    return pkts


def pes_units(pkts):
    """Rebuild (pts, payload) per PES on the video PID."""
    out, cur, cur_pts = [], None, None
    for p in pkts:
        pid = ((p[1] & 0x1F) << 8) | p[2]
        if pid != PID_VIDEO:
            continue
        afc = (p[3] >> 4) & 3
        i = 4
        if afc & 2:                       # adaptation field
            i += 1 + p[4]
        if not (afc & 1):                 # no payload
            continue
        pl = p[i:]
        if p[1] & 0x40:                   # PUSI -> new PES
            if cur is not None:
                out.append((cur_pts, cur))
            hlen = pl[8] if len(pl) > 8 else 0
            cur_pts = None
            if len(pl) > 13 and (pl[7] & 0x80):
                b = pl[9:14]
                cur_pts = (((b[0] >> 1) & 7) << 30) | (b[1] << 22) | \
                          ((b[2] >> 1) << 15) | (b[3] << 7) | (b[4] >> 1)
            cur = pl[9 + hlen:]
        elif cur is not None:
            cur += pl
    if cur is not None:
        out.append((cur_pts, cur))
    return out


def nals(b):
    r, i = [], 0
    while True:
        j = b.find(b'\x00\x00\x01', i)
        if j < 0:
            return r
        r.append(b[j + 3])
        i = j + 3


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cap = argv[1]
    es_out = None
    max_gap = 150.0
    quiet = '--quiet' in argv
    brief = '--brief' in argv
    for k, a in enumerate(argv):
        if a == '--es' and k + 1 < len(argv):
            es_out = argv[k + 1]
        if a == '--max-gap' and k + 1 < len(argv):
            max_gap = float(argv[k + 1])

    # BYTE-ITERATION COMPATIBILITY (logging audit 2026-10-29).  The unit has only
    # Python 2.7; indexing a py2 str yields a 1-char string, so `d[o] == 0x47` is
    # always False and this tool printed "no video" for EVERY capture -- the false
    # verdict that sent a whole session chasing a non-existent sender bug.
    # bytearray indexes as int in BOTH 2.7 and 3.x.
    d = bytearray(open(cap, 'rb').read())
    pkts = ts_packets(d)
    units = pes_units(pkts)
    if not units:
        print("no video (TS sync runs=%d, video PES=0)" % len(pkts))
        return 3

    def has(p, types):
        return any((n & 0x1F) in types for n in nals(p))

    sets = Counter(tuple(sorted({n & 0x1F for n in nals(p)})) for _, p in units)
    param_only = sum(1 for _, p in units if not has(p, (1, 5)))
    idr = sum(1 for _, p in units if has(p, (5,)))
    with_params = sum(1 for _, p in units if has(p, (7, 8)))
    nulls = sum(1 for p in pkts if (((p[1] & 0x1F) << 8) | p[2]) == 0x1FFF)
    sizes = sorted(len(p) for _, p in units)

    # The invariant this tool exists to check: the parameter sets ride ONLY the
    # access unit that carries an IDR.  A stream with params on every frame makes
    # a hardware decoder re-initialise and discard its reference pictures; that was
    # the actual defect on 2026-10-27 and it was invisible in every other view.
    framing_ok = (with_params == idr)
    if brief:
        print("framing=%s idr=%d params=%d param_only=%d pes=%d nulls=%d frames_min=%d frames_max=%d"
              % ("ok" if framing_ok else "suspect", idr, with_params, param_only,
                 len(units), nulls, sizes[0], sizes[-1]))

    deltas = []
    prev = None
    for pts, _ in units:
        if pts is None:
            continue
        if prev is not None:
            deltas.append(pts - prev)
        prev = pts
    span = (units[-1][0] - units[0][0]) / 90000.0 if units[0][0] is not None else 0.0
    big = [round(x / 90.0) for x in deltas if x > max_gap * 90]

    if not brief:
        print("capture: %s (%d bytes, %d TS packets)" % (cap, len(d), len(pkts)))
        print("video PES: %d   span: %.1f s   avg: %.1f fps"
              % (len(units), span, len(units) / span if span > 0 else 0))
        print("AF on IDR: %d   AF on any frame: %d   AF with no slice: %d"
              % (idr, with_params, param_only))
        print("  ^^ the invariant: 'with params' should equal 'IDR count' "
              "(both reference streams: (5,7,8) on keyframes, (1,) otherwise)")
        print("TS null packets: %d   (multi-screen-server pads each AU's last "
              "datagram with these)" % nulls)
        print("NAL sets: %s" % dict(sets))
        print("frame bytes: min %d / median %d / max %d" % (sizes[0], sizes[len(sizes) // 2], sizes[-1]))
        if deltas:
            print("PTS deltas > %.0f ms: %d of %d   (largest %.0f ms)"
                  % (max_gap, len(big), len(deltas), max(deltas) / 90.0))
            if big and not quiet:
                print("  gaps (ms): %s" % big[:24])
    if es_out:
        # The PES payload already begins with a start code; prefixing another one
        # (the previous behaviour) left a stray NAL type 0 in front of every unit.
        with open(es_out, 'wb') as f:
            for _, p in units:
                if not p.startswith(b'\x00\x00\x01'):
                    f.write(b'\x00\x00\x00\x01')
                f.write(p)
        if not brief:
            print("wrote ES -> %s (decode it: ffmpeg -i %s -vf ... png)" % (es_out, es_out))
    return 0 if not brief or framing_ok else 4


if __name__ == '__main__':
    sys.exit(main(sys.argv))
