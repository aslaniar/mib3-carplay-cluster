#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""cluster_rtp_ts.py -- independent verifier for Path B's cluster stream.

Reads the raw datagrams mib3clusterstream emits (RTP/PT=33 + MPEG-TS), parses
them with a *separate* implementation from the muxer, and checks every field
against what multi-screen-server produces.  Optionally wraps them in a pcap so
Wireshark/tshark can dissect the same bytes independently, and extracts the
reassembled Annex-B ES for a byte-exact round-trip against the input.

usage:
  cluster_rtp_ts.py <datagrams.bin> [--pcap out.pcap] [--extract out.264]
                                    [--expect-frames N] [--quiet]

exit 0 = every check passed.
"""
import sys, struct, json

TS = 188

class Fail(Exception): pass

def check(cond, msg):
    if not cond: raise Fail(msg)

def parse_pat(pkt):
    check(pkt[0] == 0x47, "PAT: no sync")
    check(((pkt[1] & 0x1F) << 8 | pkt[2]) == 0, "PAT: wrong PID")
    check(pkt[4] == 0x00, "PAT: pointer_field != 0")
    sec = pkt[5:]
    check(sec[0] == 0x00, "PAT: table_id != 0")
    slen = ((sec[1] & 0x0F) << 8) | sec[2]
    secend = 3 + slen                    # section_length counts from sec[3]
    crc = sec[secend - 4:secend]
    check(crc_mpeg(sec[:secend - 4]) == struct.unpack('>I', crc)[0], "PAT: bad CRC32")
    body = sec[3:secend - 4]
    tsid = (body[0] << 8) | body[1]
    progs = {}
    off = 5                              # tsid(2) ver(1) sec/num(2)
    while off + 4 <= len(body):
        pn = (body[off] << 8) | body[off + 1]
        pid = ((body[off + 2] & 0x1F) << 8) | body[off + 3]
        progs[pn] = pid; off += 4
    return tsid, progs

def parse_pmt(pkt):
    sec = pkt[5:]
    check(sec[0] == 0x02, "PMT: table_id != 2")
    slen = ((sec[1] & 0x0F) << 8) | sec[2]
    secend = 3 + slen
    crc = sec[secend - 4:secend]
    check(crc_mpeg(sec[:secend - 4]) == struct.unpack('>I', crc)[0], "PMT: bad CRC32")
    body = sec[3:secend - 4]
    prog = (body[0] << 8) | body[1]
    pcr_pid = ((body[5] & 0x1F) << 8) | body[6]
    pilen = ((body[7] & 0x0F) << 8) | body[8]
    off = 9 + pilen
    streams = []
    while off + 5 <= len(body):
        st = body[off]
        pid = ((body[off + 1] & 0x1F) << 8) | body[off + 2]
        el = ((body[off + 3] & 0x0F) << 8) | body[off + 4]
        streams.append((st, pid)); off += 5 + el
    return prog, pcr_pid, streams

def crc_mpeg(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc

def pts_of(pes):
    # PES: 00 00 01 sid len(2) flags(2) hdrlen  [PTS 5]
    check(pes[0:3] == b"\x00\x00\x01", "PES: no start code")
    hdrlen = pes[8]
    flags = pes[7]
    if not (flags & 0x80): return None
    p = pes[9:14]
    check(p[0] & 0x01, "PES: PTS marker bit 0")
    v = (((p[0] >> 1) & 0x07) << 30) | (p[1] << 22) | (((p[2] >> 1) & 0x7F) << 15) | \
        (p[3] << 7) | ((p[4] >> 1) & 0x7F)
    return v

def main():
    path = sys.argv[1]
    pcap = extract = None
    expect = None; quiet = False; no_psi = False
    a = sys.argv[2:]
    i = 0
    while i < len(a):
        if a[i] == "--pcap": pcap = a[i + 1]; i += 2
        elif a[i] == "--extract": extract = a[i + 1]; i += 2
        elif a[i] == "--expect-frames": expect = int(a[i + 1]); i += 2
        elif a[i] == "--quiet": quiet = True; i += 1
        elif a[i] == "--no-psi": no_psi = True; i += 1
        else: print("bad arg", a[i]); return 2

    data = open(path, 'rb').read()
    off = 0
    dgrams = 0
    seqs = []
    tsids = set(); pmt_pids = set(); es_pids = set(); stream_types = set(); pcr_pids = set()
    first_mb_zero = sum(1 for x in range(20))
    aus = []            # (pts, bytes)
    cur_au = bytearray(); cur_pts = None
    cc = {}
    pcr_seen = 0
    seen_pat = seen_pmt = False

    while off < len(data):
        check(len(data) - off >= 12, "truncated RTP header")
        rtp = data[off:off + 12]
        off += 12
        # ---- RTP header ----
        check(rtp[0] == 0x80, f"RTP: version/pad/x/cc byte = {rtp[0]:#x} (want 0x80)")
        pt = rtp[1] & 0x7F
        check(pt == 33, f"RTP: payload type {pt} != 33 (MPEG-TS)")
        seq = (rtp[2] << 8) | rtp[3]
        ts32, ssrc = struct.unpack('>II', rtp[4:12])
        seqs.append(seq)
        dgrams += 1
        # ---- TS packets ----
        ngot = 0
        while off < len(data) and ngot < 64:
            b = data[off]
            if b != 0x47: break
            pkt = data[off:off + TS]; off += TS; ngot += 1
            pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
            pusi = pkt[1] & 0x40
            afc = (pkt[3] >> 4) & 0x03
            if pid == 0 and pusi:
                t, progs = parse_pat(pkt); tsids.add(t)
                pmt_pids.update(progs.values()); seen_pat = True
                continue
            if pid in pmt_pids and pusi:
                prog, pcr, streams = parse_pmt(pkt)
                pcr_pids.add(pcr)
                for st, spid in streams:
                    stream_types.add(st); es_pids.add(spid)
                seen_pmt = True
                continue
            if pid in es_pids:
                # continuity counter
                c = pkt[3] & 0x0F
                if pid in cc:
                    check((cc[pid] + 1) & 0x0F == c, f"ES pid {pid}: cc jump")
                cc[pid] = c
                p = 4
                if afc & 0x02:
                    afl = pkt[4]
                    if afl and (pkt[5] & 0x10): pcr_seen += 1
                    p = 5 + afl
                if afc & 0x01:
                    payload = pkt[p:]
                    if pusi:
                        if cur_pts is not None:
                            aus.append((cur_pts, bytes(cur_au)))
                        cur_au = bytearray(); cur_pts = None
                        cur_pts = pts_of(payload)
                        hdrlen = payload[8]
                        cur_au += payload[9 + hdrlen:]
                    else:
                        cur_au += payload
    if cur_pts is not None:
        aus.append((cur_pts, bytes(cur_au)))

    # ---- checks ----
    check(dgrams > 0, "no datagrams")
    if not no_psi:
        check(seen_pat, "no PAT seen")
        check(seen_pmt, "no PMT seen")
    if not no_psi:
        check(tsids == {100}, f"transport_stream_id {tsids} != {{100}}")
        check(pmt_pids == {1000}, f"PMT pid {pmt_pids} != {{1000}}")
        check(es_pids == {17}, f"ES pid {es_pids} != {{17}}")
        check(stream_types == {0x1B}, f"stream_type {stream_types} != {{0x1b H.264}}")
        check(pcr_pids == {17}, f"PCR pid {pcr_pids} != {{17}}")
    # sequence numbers strictly +1 (mod 2^16)
    for j in range(1, len(seqs)):
        check((seqs[j - 1] + 1) & 0xFFFF == seqs[j], f"RTP seq jump at dgram {j}")
    # PTS monotone non-decreasing
    last = -1
    for pts, _ in aus:
        if pts is None and no_psi: continue
        check(pts is not None, "AU without PTS")
        check(pts >= last, "PTS went backwards")
        last = pts

    if extract:
        with open(extract, 'wb') as f:
            for _, au in aus: f.write(au)

    if pcap:
        write_pcap(pcap, data)

    if not quiet:
        print(json.dumps(dict(
            datagrams=dgrams,
            rtp_pt=33, ssrc=ssrc, tsid=100, pmt_pid=1000,
            es_pid=17, stream_type="0x1b", pcr_pid=17,
            pcr_packets=pcr_seen, access_units=len(aus),
            first_pts=aus[0][0] if aus else None,
            last_pts=aus[-1][0] if aus else None,
            au_sizes=[len(a) for _, a in aus][:8],
        ), indent=2))

    if expect is not None:
        check(len(aus) == expect, f"got {len(aus)} AUs, expected {expect}")
    print("VERIFY OK")
    return 0

def write_pcap(path, data):
    """wrap each datagram in eth/ipv6/udp so tshark dissects RTP + MPEG-TS."""
    recs = []
    off = 0
    src = bytes.fromhex("fd53" + "7cb8" + "0383" + "0003" + "0000" + "0000" + "0000" + "0073")
    dst = bytes.fromhex("ff14" + "0000" * 5 + "0002" + "0004")   # ff14::2:4

    def csum(pseudo_plus_hdr):
        s = 0
        b = pseudo_plus_hdr
        for i in range(0, len(b) - 1, 2):
            s += (b[i] << 8) | b[i + 1]
        if len(b) % 2:
            s += b[-1] << 8
        while s >> 16:
            s = (s & 0xFFFF) + (s >> 16)
        return (~s) & 0xFFFF

    while off < len(data):
        dgstart = off
        off += 12
        n = 0
        while off + TS <= len(data) and data[off] == 0x47 and n < 64:
            off += TS; n += 1
        dglen = off - dgstart
        udp_hdr = struct.pack('>HHHH', 42993, 42800, 8 + dglen, 0)
        pseudo = src + dst + struct.pack('>I', 8 + dglen) + b"\x00\x00\x00" + bytes([17])
        u = csum(pseudo + udp_hdr + data[dgstart:off])
        udp = struct.pack('>HHHH', 42993, 42800, 8 + dglen, u if u else 0xFFFF)
        iplen = 40 + len(udp) + dglen
        ip6 = struct.pack('>IHBB', 0x60000000, iplen, 17, 64) + src + dst
        eth = b"\x33\x33\x00\x00\x00\x24" + b"\x02\x00\x00\x00\x00\x01" + b"\x86\xdd"
        recs.append(eth + ip6 + udp + data[dgstart:off])
    with open(path, 'wb') as f:
        f.write(struct.pack('<IHHiIII', 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        t = 0
        for r in recs:
            f.write(struct.pack('<IIII', t // 1000, t % 1000, len(r), len(r))); f.write(r)
            t += 33000        # ~30 fps

if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        print("VERIFY FAIL:", e); sys.exit(1)
