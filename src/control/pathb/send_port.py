#!/usr/bin/env python
"""send_port.py -- replay a mib3clusterstream datagram file from a chosen SOURCE PORT,
paced over a given duration.  Runs ON THE UNIT.

  python send_port.py 42989 /dev/shm/ours.dgrams 20      # 20 s

The cluster only accepts the flow from multi-screen-server's port, and a burst that
outruns the decoder drops the cluster into "Starting map display...".
"""
import socket, sys, time
port = int(sys.argv[1]); path = sys.argv[2]; secs = float(sys.argv[3])
d = open(path, "rb").read()
n = len(d) // 1140
delay = secs / n if n else 0.008
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.SOL_SOCKET, 25, "eth0.5\0")
try:
    s.bind(("::", port))
except Exception as e:
    print("bind %d failed: %s" % (port, e)); sys.exit(1)
print("bound %d, %d datagrams over %.0fs (one per %.2f ms)" % (port, n, secs, delay * 1000))
dst = ("ff14::2:4", 42800)
t0 = time.time()
for i in range(n):
    s.sendto(d[i * 1140:(i + 1) * 1140], dst)
    if i % 520 == 0:
        print("  %4.1fs sent %d/%d" % (time.time() - t0, i, n))
    time.sleep(delay)
print("done %d in %.1fs" % (n, time.time() - t0))
