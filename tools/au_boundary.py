#!/usr/bin/env python3
"""au_boundary.py -- does every access unit END on a datagram boundary?

WHY (2026-10-27).  This is a wire-format property nobody had measured, and getting
it wrong starves the receiver in a way that looks like a decode failure.

multi-screen-server's own stream, measured from the car:
    1586 datagrams, all 6 x 188 B
     16 of them straddle an AU boundary  (1% -- the 500 ms PSI insertions)
     76 of them carry TS null packets    (the padding that fills an AU's last one)
So: an AU always finishes inside the datagram that ends it, and the tail of one
AU is never mixed with the head of the next.

Our sender used to pack greedily and flush only at six, so 1669 of 8948 datagrams
straddled (19%) and the final packets of an AU stayed in the buffer until the NEXT
AU was emitted.  When the phone goes quiet -- which for a static map is most of the
time -- the last picture sat on the wire incomplete for the length of the silence
(600-840 ms measured).  A receiver cannot finish that PES until the next AU starts.

Reported:
    datagrams=N straddle=N nulls=N aus=N sizes=[...]
One line, so a shell can parse it with sed; the table above it is for a human.

Usage: au_boundary.py CAPTURE.bin [--quiet]
Exit 0 = boundary discipline holds (straddle <= 3% of datagrams), 1 = it does not.
"""
import sys
from collections import Counter

TS = 188
SYNC_DEPTH = 6
SYNC = 0x47
PID_VIDEO = 17
PID_NULL = 0x1FFF


def datagrams(d):
    """Each maximal run of contiguous 188-byte TS packets is one datagram's worth."""
    out, o, L = [], 0, len(d)
    while o + TS * SYNC_DEPTH <= L:
        if d[o] == SYNC and all(d[o + TS * k] == SYNC for k in range(1, SYNC_DEPTH)):
            run = []
            while o + TS <= L and d[o] == SYNC:
                run.append(d[o:o + TS])
                o += TS
            out.append(run)
        else:
            o += 1
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    quiet = '--quiet' in argv
    path = argv[1]
    d = open(path, 'rb').read()
    dgs = datagrams(d)
    if not dgs:
        print("datagrams=0 straddle=0 nulls=0 aus=0 sizes=[]")
        return 1

    straddle = 0
    nulls = 0
    aus = 0
    sizes = Counter()
    for run in dgs:
        sizes[len(run)] += 1
        pids = [((p[1] & 0x1F) << 8) | p[2] for p in run]
        pusi = [bool(p[1] & 0x40) for p in run]
        nulls += sum(1 for x in pids if x == PID_NULL)
        vid_pusi = [i for i, (x, u) in enumerate(zip(pids, pusi)) if x == PID_VIDEO and u]
        aus += len(vid_pusi)
        # a PUSI at a non-zero index means this datagram also carries the tail of
        # the previous access unit => the two are mixed
        if vid_pusi and vid_pusi[0] != 0:
            straddle += 1

    limits = ", ".join("%dx%d" % (n, k) for k, n in sizes.most_common(4))
    print("datagrams=%d straddle=%d nulls=%d aus=%d sizes=[%s]"
          % (len(dgs), straddle, nulls, aus, limits))
    if not quiet:
        pct = 100.0 * straddle / len(dgs)
        print("  access units            %d" % aus)
        print("  datagrams straddling    %d of %d (%.1f%%) -- MS-S measures 1%%" % (straddle, len(dgs), pct))
        print("  TS null packets         %d (padding; MS-S: one padded datagram per AU)" % nulls)
        print("  datagram sizes          %s  (all one size = fixed datagram)" % limits)
    return 0 if straddle * 100 <= len(dgs) * 3 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
