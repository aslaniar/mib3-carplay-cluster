#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-or-later
# -*- coding: utf-8 -*-
"""pathb_listen.py -- join the cluster multicast group and report what is really on it.

Runs ON THE UNIT (python 2.7 compatible).  READ-ONLY: it never sends and never
touches a service.  It exists because the C probe prints only the first
datagram's header, and Path B needs two yes/no answers:

    1. is PT=33 (video) on the group right now?
    2. after we park MS-S, did PT=33 actually stop, leaving only the PT=98 beacon?

The C probe counts datagrams; this one classifies them (PT, SSRC, size, TS PID)
so "video present" and "beacon only" cannot be confused.

usage:
  python pathb_listen.py [--seconds N] [--every S] [--out FILE]
                         [--group ff14::2:4] [--port 42800] [--iface eth0.5]

  --out writes LENGTH-PREFIXED records (u32 BE length + datagram), so the
  laptop can analyse them exactly; the raw bytes of a datagram socket capture
  otherwise have no boundaries.

exit: 0 = PT=33 seen, 3 = no PT=33 (so a shell caller can branch on it)
"""
from __future__ import print_function

import socket
import struct
import sys
import time

SO_BINDTODEVICE = 25  # Linux


def o(x):
    """byte value: python3 gives int, python2.7 gives a 1-char str"""
    return ord(x) if isinstance(x, str) else x


def parse_argv(argv):
    a = {"seconds": 10, "every": 2, "out": "", "group": "ff14::2:4",
         "port": 42800, "iface": "eth0.5"}
    i = 0
    while i < len(argv):
        k = argv[i]
        v = argv[i + 1] if i + 1 < len(argv) else ""
        if k == "--seconds":
            a["seconds"] = int(v); i += 2
        elif k == "--every":
            a["every"] = max(1, int(v)); i += 2
        elif k == "--out":
            a["out"] = v; i += 2
        elif k == "--group":
            a["group"] = v; i += 2
        elif k == "--port":
            a["port"] = int(v); i += 2
        elif k == "--iface":
            a["iface"] = v; i += 2
        else:
            sys.stderr.write("unknown arg: %s\n" % k)
            sys.exit(2)
    return a


def ifindex(ifname):
    """The unit's python 2.7 has no socket.if_nametoindex -- read sysfs instead."""
    try:
        return socket.if_nametoindex(ifname)
    except Exception:
        pass
    try:
        f = open("/sys/class/net/%s/ifindex" % ifname)
        v = int(f.read().strip())
        f.close()
        return v
    except Exception:
        return 0


def open_group(a):
    s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except Exception:
        pass
    try:
        s.setsockopt(socket.SOL_SOCKET, SO_BINDTODEVICE, a["iface"] + "\0")
    except Exception as e:
        sys.stderr.write("warn: SO_BINDTODEVICE %s: %s\n" % (a["iface"], e))
    s.bind(("::", a["port"]))
    idx = ifindex(a["iface"])
    if not idx:
        sys.stderr.write("warn: no ifindex for %s -- join may fail\n" % a["iface"])
    mreq = socket.inet_pton(socket.AF_INET6, a["group"]) + struct.pack("@I", idx)
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, mreq)
    s.settimeout(0.5)
    return s


def ts_pids(dg):
    """PID histogram of a PT=33 datagram (12-byte RTP header, then 188-byte TS)."""
    out = {}
    for off in range(12, len(dg) - 187, 188):
        p = dg[off:off + 188]
        if o(p[0]) != 0x47:
            continue
        pid = ((o(p[1]) & 0x1F) << 8) | o(p[2])
        out[pid] = out.get(pid, 0) + 1
    return out


def bump(d, k, n=1):
    d[k] = d.get(k, 0) + n


def main():
    a = parse_argv(sys.argv[1:])
    try:
        s = open_group(a)
    except Exception as e:
        sys.stderr.write("join failed: %s\n" % e)
        return 4

    out = open(a["out"], "wb") if a["out"] else None
    print("listening %s port %d on %s for %ds" % (a["group"], a["port"], a["iface"], a["seconds"]))

    pt, ssrc, size, pids = {}, {}, {}, {}
    n = 0
    seq_first = seq_last = None
    seq_gaps = 0
    t0 = time.time()
    nxt = t0 + a["every"]

    while time.time() - t0 < a["seconds"]:
        try:
            dg, _ = s.recvfrom(8192)
        except socket.timeout:
            pass
        else:
            if len(dg) >= 12:
                n += 1
                p = o(dg[1]) & 0x7F
                seq = (o(dg[2]) << 8) | o(dg[3])
                ss = struct.unpack(">I", dg[8:12])[0]
                bump(pt, p); bump(ssrc, (p, ss)); bump(size, len(dg))
                if p == 33:
                    for k, v in ts_pids(dg).items():
                        bump(pids, k, v)
                if seq_first is None:
                    seq_first = seq
                elif seq_last is not None and ((seq_last + 1) & 0xFFFF) != seq:
                    seq_gaps += 1
                seq_last = seq
                if out:
                    out.write(struct.pack(">I", len(dg)) + dg)
        now = time.time()
        if now >= nxt:
            nxt = now + a["every"]
            print("t=%3ds datagrams=%d PT=%s ssrc=%s" %
                  (int(now - t0), n,
                   ",".join("%d:%d" % (k, pt[k]) for k in sorted(pt)),
                   ",".join("%d/%x:%d" % (p, s, c) for (p, s), c in sorted(ssrc.items()))))

    if out:
        out.close()
    video = pt.get(33, 0)
    print("sizes=%s" % ",".join("%d:%d" % (k, size[k]) for k in sorted(size)))
    if pids:
        print("ts_pids=%s" % ",".join("%d:%d" % (k, pids[k]) for k in sorted(pids)))
    print("seq_first=%s seq_last=%s seq_gaps=%d" % (seq_first, seq_last, seq_gaps))
    if video:
        print("VERDICT video=YES pt33=%d" % video)
        return 0
    print("VERDICT video=NO pt98=%d" % pt.get(98, 0))
    return 3


if __name__ == "__main__":
    sys.exit(main())
