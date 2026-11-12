#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-or-later
"""fake_shim.py -- play an Annex-B file into the muxer's AU socket EXACTLY the way the
shim feeds it: one record for the parameter sets (SPS/PPS), then one record per frame,
with the frame records carrying SLICES ONLY.

Purpose: isolate whether the cluster's refusal of the phone's live stream is caused by
the shim's framing (SPS/PPS as their own PES) rather than by the bitstream itself.
The fixture that renders through the muxer's FILE mode must be re-tested here.

Faithfulness matters, so two details are modelled rather than approximated:
  * the config record is the CURRENT sequence header -- the last SPS plus the last PPS
    seen -- not every header the file ever contained (the shim holds one avcC);
  * frame records are slices only, because the phone's media AUs contain no parameter
    sets; that is precisely why the shim stamps them.

usage: python fake_shim.py <file.264> [fps] [socket_path] [--noconfig]
"""
import socket, struct, sys, time


def o(x):
    return ord(x) if isinstance(x, str) else x


path = sys.argv[1]
pace = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
sockpath = sys.argv[3] if len(sys.argv) > 3 else "/dev/shm/mib3cluster.sock"
send_cfg = "--noconfig" not in sys.argv
es_out = None
if "--es-out" in sys.argv:
    es_out = sys.argv[sys.argv.index("--es-out") + 1]

d = open(path, "rb").read()
nals = []
i = 0
while i + 4 < len(d):
    if o(d[i]) == 0 and o(d[i+1]) == 0 and o(d[i+2]) == 1:
        j = i + 3
        k = j
        while k + 3 < len(d) and not (o(d[k]) == 0 and o(d[k+1]) == 0 and o(d[k+2]) == 1):
            k += 1
        nals.append(d[i:k])
        i = k
    else:
        i += 1

cur_sps = b""
cur_pps = b""
frames = []
for n in nals:
    t = o(n[3]) & 0x1F
    if t == 7:
        cur_sps = n
    elif t == 8:
        cur_pps = n
    elif t == 9:
        pass                      # AUD: the muxer opens an AU on it; not needed here
    elif t in (1, 5):
        frames.append((cur_sps + cur_pps, n))

print("nals %d cfg %d frames %d" % (len(nals), len(frames[0][0]) if frames else 0, len(frames)))

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(50):
    try:
        s.connect(sockpath)
        break
    except OSError:
        time.sleep(0.1)
else:
    print("cannot connect to %s" % sockpath)
    sys.exit(2)

seq = 0


def rec(p):
    global seq
    seq += 1
    s.sendall(struct.pack("<IIQ", 0x4D334331, len(p), seq) + p)


t0 = time.time()
es = []
for cfg, au in frames:
    if send_cfg and cfg:
        rec(cfg)                  # parameter sets as their OWN record -- exactly the shim
        es.append(cfg)
    rec(au)                       # one frame, slices only
    es.append(au)
    time.sleep(1.0 / pace)
print("sent %d frames in %.1fs" % (len(frames), time.time() - t0))
if es_out:
    f = open(es_out, "wb")
    f.write(b"".join(es))
    f.close()
    print("fed ES -> %s (%d bytes)" % (es_out, sum(len(x) for x in es)))
s.close()
