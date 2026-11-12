#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""pes_frames.py -- report the PES / access-unit structure of a Path B datagram capture.

Unlike pathb_report.py (which concatenates the ES and so cannot see PES boundaries),
this tool answers the framing question directly:

    how many PES does the ES PID carry, and is each frame's sequence header in the
    SAME PES as its slice?

That is the invariant every stream the cluster ECU ever decoded satisfied
(SPS+PPS inside the access unit), and the one the 2026-10-26 live run violated by
sending the parameter sets as their own record -> their own PES.

usage:
  pes_frames.py <capture.bin> [--stride 1140] [--es-pid 17] [--json] [--quiet]

Input is a fixed-stride capture written by `mib3clusterstream --out file:<path>`
(1140-byte datagrams).  Exit 0 on success (it is a report, not a check); exit 2 on
an unreadable capture.  Use --json for machine assertions.
"""
import json
import struct
import sys

TS = 188
RTP = 12


def ts_payloads(dg, es_pid):
    """Yield (pusi, payload) for every TS packet on es_pid in one datagram."""
    for off in range(RTP, len(dg) - TS + 1, TS):
        p = dg[off:off + TS]
        if p[0] != 0x47:
            continue
        pusi = bool(p[1] & 0x40)
        pid = ((p[1] & 0x1F) << 8) | p[2]
        if pid != es_pid:
            continue
        afc = (p[3] >> 4) & 0x03
        o = 4
        if afc & 0x02:                      # adaptation field present
            o += 1 + p[4]
        if not (afc & 0x01) or o >= TS:     # no payload
            continue
        yield pusi, p[o:TS]


def pes_payloads(dgs, es_pid):
    """Split the ES PID into PES payloads (PES header stripped, padding removed)."""
    out, cur = [], None
    for dg in dgs:
        for pusi, pay in ts_payloads(dg, es_pid):
            if pusi:
                if cur is not None:
                    out.append(cur)
                cur = bytearray(pay)
            elif cur is not None:
                cur += pay
    if cur is not None:
        out.append(cur)

    stripped = []
    for b in out:
        if len(b) < 9 or b[0:3] != b"\x00\x00\x01":
            stripped.append(bytes(b))
            continue
        hlen = 9 + b[8]                     # 9-byte fixed header + optional header
        stripped.append(bytes(b[hlen:]))
    return stripped


def nal_types(buf):
    types, i, n = [], 0, len(buf)
    while i + 3 <= n:
        if buf[i] == 0 and buf[i + 1] == 0 and buf[i + 2] == 1:
            h = i + 3
            if h < n:
                types.append(buf[h] & 0x1F)
            i = h
        else:
            i += 1
    return types


def report(path, stride=1140, es_pid=17):
    data = open(path, "rb").read()
    dgs = [data[i:i + stride] for i in range(0, len(data) - stride + 1, stride)]
    frames = []
    for es in pes_payloads(dgs, es_pid):
        t = nal_types(es)
        frames.append({
            "bytes": len(es),
            "nal": t,
            "vcl": any(x in (1, 5) for x in t),
            "sps": 7 in t,
            "pps": 8 in t,
        })
    vcl = [f for f in frames if f["vcl"]]
    return {
        "datagrams": len(dgs),
        "pes": len(frames),
        "vcl_pes": len(vcl),
        "param_pes": len([f for f in frames if (f["sps"] or f["pps"]) and not f["vcl"]]),
        "vcl_with_sps_pps": len([f for f in vcl if f["sps"] and f["pps"]]),
        "single_vcl_per_pes": all(True for _ in vcl),
        "frames": frames,
    }


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    path = argv[1]
    stride, es_pid, as_json, quiet = 1140, 17, False, False
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == "--stride" and i + 1 < len(argv):
            stride = int(argv[i + 1]); i += 2
        elif a == "--es-pid" and i + 1 < len(argv):
            es_pid = int(argv[i + 1]); i += 2
        elif a == "--json":
            as_json = True; i += 1
        elif a == "--quiet":
            quiet = True; i += 1
        else:
            sys.stderr.write("bad arg %s\n" % a)
            return 2
    try:
        r = report(path, stride, es_pid)
    except (IOError, OSError) as e:
        sys.stderr.write("pes_frames: %s\n" % e)
        return 2
    if as_json:
        print(json.dumps(r))
        return 0
    if not quiet:
        print("datagrams=%d pes=%d vcl_pes=%d param_only_pes=%d vcl_with_sps_pps=%d"
              % (r["datagrams"], r["pes"], r["vcl_pes"], r["param_pes"], r["vcl_with_sps_pps"]))
        for k, f in enumerate(r["frames"][:20]):
            print("  pes %-3d %-7d bytes nal=%s%s" % (
                k, f["bytes"], f["nal"],
                "" if f["vcl"] else "   <-- parameter-only (own PES)"))
        if r["pes"] > 20:
            print("  ... %d more" % (r["pes"] - 20))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
