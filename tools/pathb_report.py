#!/usr/bin/env python3
"""pathb_report.py -- classify a Path B datagram capture on the laptop.

Reads either:
  * a LENGTH-PREFIXED capture from pathb_listen.py --out   (u32 BE length + datagram)
  * a fixed-stride capture from mib3clusterstream --out file:... (1140-byte datagrams)

and prints what actually arrived: RTP PT histogram, SSRC per PT, datagram sizes,
TS PIDs inside PT=33, sequence continuity, and it extracts the Annex-B video ES
so it can be compared with the file we sent.

usage:
  pathb_report.py <capture.bin> [--stride 1140] [--extract out.264] [--quiet]

exit 0 = PT=33 present, 3 = no PT=33.
"""
import struct
import sys
import collections


def load(path):
    d = open(path, "rb").read()
    dgs = []
    # Try length-prefixed first: a 4-byte BE length that lands exactly on the end
    # of a chain of datagrams is almost certainly ours.
    i = 0
    ok = True
    while i + 4 <= len(d):
        n = struct.unpack(">I", d[i:i + 4])[0]
        if n < 12 or n > 9000 or i + 4 + n > len(d):
            ok = False
            break
        dgs.append(d[i + 4:i + 4 + n])
        i += 4 + n
    if ok and i == len(d) and dgs:
        return dgs, "length-prefixed"
    # Fixed stride (our own sender): 12 + 6*188
    stride = 1140
    for k in range(len(d) // stride):
        dgs.append(d[k * stride:(k + 1) * stride])
    if len(d) % stride:
        dgs.append(d[len(d) // stride * stride:])
    return dgs, "fixed %d" % stride


def ts_pids(dg):
    out = collections.Counter()
    for off in range(12, len(dg) - 187, 188):
        p = dg[off:off + 188]
        if p[0] == 0x47:
            out[((p[1] & 0x1F) << 8) | p[2]] += 1
    return out


def es_from(dgs):
    """Concatenate PT=33 video PID 17 payloads into the Annex-B elementary stream.

    Strips the PES header on every PUSI, so the result is byte-comparable with the
    file that was handed to the muxer (that round-trip is the point: sender ->
    datagrams -> parse -> identical ES)."""
    out = bytearray()
    for dg in dgs:
        if len(dg) < 12 or (dg[1] & 0x7F) != 33:
            continue
        for off in range(12, len(dg) - 187, 188):
            p = dg[off:off + 188]
            if p[0] != 0x47:
                continue
            pid = ((p[1] & 0x1F) << 8) | p[2]
            if pid != 17:
                continue
            pusi = (p[1] & 0x40) != 0
            afc = (p[3] >> 4) & 3
            o = 4
            if afc & 2:  # adaptation field present
                o += 1 + p[4]
            if not (afc & 1) or o >= 188:
                continue
            pay = p[o:]
            if pusi and len(pay) > 8 and pay[:3] == b"\x00\x00\x01":
                pay = pay[9 + (pay[8] & 0xFF):]
            out += pay
    return bytes(out)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    extract = None
    quiet = False
    a = sys.argv[2:]
    for i, v in enumerate(a):
        if v == "--extract" and i + 1 < len(a):
            extract = a[i + 1]
        elif v == "--quiet":
            quiet = True

    dgs, how = load(path)
    pt = collections.Counter()
    ssrc = collections.Counter()
    sizes = collections.Counter()
    pids = collections.Counter()
    seqs = []
    for dg in dgs:
        if len(dg) < 12:
            continue
        p = dg[1] & 0x7F
        pt[p] += 1
        ssrc[(p, struct.unpack(">I", dg[8:12])[0])] += 1
        sizes[len(dg)] += 1
        if p == 33:
            pids.update(ts_pids(dg))
            seqs.append((dg[2] << 8) | dg[3])

    gaps = 0
    for x, y in zip(seqs, seqs[1:]):
        if ((x + 1) & 0xFFFF) != y:
            gaps += 1

    print("capture: %s (%s), %d datagrams" % (path, how, len(dgs)))
    print("  PT:    %s" % dict(sorted(pt.items())))
    print("  SSRC:  %s" % ", ".join("%d/0x%x:%d" % (p, s, c)
                                    for (p, s), c in sorted(ssrc.items())))
    print("  sizes: %s" % dict(sizes.most_common(6)))
    if pids:
        print("  TS PIDs: %s" % dict(sorted(pids.items())))
        print("  RTP seq: %d packets, %d discontinuities" % (len(seqs), gaps))
    if extract:
        es = es_from(dgs)
        open(extract, "wb").write(es)
        print("  extracted %d bytes of Annex-B -> %s" % (len(es), extract))
    ok = pt.get(33, 0) > 0
    print("VERDICT %s (pt33=%d pt98=%d)" %
          ("video present" if ok else "NO video", pt.get(33, 0), pt.get(98, 0)))
    return 0 if ok else 3


if __name__ == "__main__":
    sys.exit(main())
