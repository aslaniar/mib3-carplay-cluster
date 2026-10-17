#!/usr/bin/python
# feed_test.py -- drive the OUT-OF-PROCESS delivery helper with a local Annex-B H.264 stream.
#   *** Runs ON THE UNIT.  NO PHONE NEEDED. ***
#
# WHY: the phone is the only other frame source, and it competes with the laptop's tether.
# This feeds the helper over its own socket (the same path the shim uses), so the whole
# decode -> render -> surface -> encode chain can be exercised without CarPlay at all.
#
# Wire format (mib3helper.h): 16-byte header { u32 magic, u32 len, u64 seq } + Annex-B AU.
#
# usage:  /usr/bin/python feed_test.py [file.264] [fps] [count]
import socket, struct, sys, time

MAGIC = 0x4d334331            # MIB3_HELP_MAGIC
SOCK  = "/dev/shm/mib3cluster.sock"

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/test1280.264"
fps  = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
cnt  = int(sys.argv[3]) if len(sys.argv) > 3 else 600

data = open(path, "rb").read()
# Annex-B.  Group NALs into ACCESS UNITS the way the shim's receiver does: SPS+PPS+IDR (and any
# leading SEI) belong to ONE AU.  Splitting them into separate frames is not decodable.
SC4 = b"\x00\x00\x00\x01"
raw = data.split(SC4) if SC4 in data else data.split(b"\x00\x00\x01")
nals = [p for p in raw if p]
aus = []
cur = b""
for p in nals:
    t = ord(p[0]) & 0x1F
    if t == 7 and cur:            # an SPS starts a new access unit
        aus.append(cur)
        cur = b""
    cur += SC4 + p
if cur:
    aus.append(cur)
sys.stderr.write("file=%s bytes=%d AUs=%d\n" % (path, len(data), len(aus)))
if not aus:
    sys.exit("no access units found")

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
while True:
    try:
        s.connect(SOCK)
        break
    except Exception, e:
        sys.stderr.write("waiting for the helper on %s (%s)\n" % (SOCK, e))
        time.sleep(1)

seq = 0
while seq < cnt:
    au = aus[seq % len(aus)]
    s.sendall(struct.pack("<IIQ", MAGIC, len(au), seq))
    s.sendall(au)
    seq += 1
    if seq % 50 == 0:
        sys.stderr.write("sent=%d au_len=%d\n" % (seq, len(au)))
    time.sleep(1.0 / fps)
sys.stderr.write("done (%d AUs sent)\n" % seq)
