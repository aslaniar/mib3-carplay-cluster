#!/usr/bin/env python
"""msport.py -- discover the UDP SOURCE PORT multi-screen-server sends the
cluster video from.  Read-only; works on the unit's python 2.7 and on python 3.

Why this exists: the cluster receiver drops video that does not arrive from the
port MS-S uses (measured on the car, 2026-10-26: ephemeral -> ignored, MS-S's
port -> decodes).  MS-S binds once per process and keeps it across stream
restarts and park/hand-back, but the number is not in any config, so it must be
read at runtime.  Never hardcode it, never guess it.

Path, all read-only:
    /proc/<pid>/cmdline        -> find the process by name
    /proc/<pid>/fd/*           -> "socket:[<inode>]"
    /proc/net/udp6             -> inode -> local port

usage:
    msport.py [--proc /proc] [--name multi-screen-server] [--pid N] [--json]

output:
    default  bare port number on stdout (so: --sport "$(msport.py)")
    --json   {"port":N,"pid":N,"inodes":[...],"candidates":[ports],"source":"..."}

exit codes:
    0  exactly one candidate
    2  MS-S not found, or it owns no usable UDP socket
    3  more than one candidate -- ambiguous, refuse to guess
"""
import os
import sys

NAME = "multi-screen-server"


def _is_dir(p):
    try:
        return os.path.isdir(p)
    except OSError:
        return False


def _read(path):
    try:
        f = open(path, "rb")
        try:
            return f.read()
        finally:
            f.close()
    except (IOError, OSError):
        return None


def find_pids(proc, name):
    """pids whose cmdline mentions `name` (exact basename of argv[0] preferred)."""
    out = []
    try:
        entries = os.listdir(proc)
    except OSError:
        return out
    for e in entries:
        if not e.isdigit():
            continue
        raw = _read(os.path.join(proc, e, "cmdline"))
        if not raw:
            continue
        argv = [a for a in raw.split(b"\x00") if a]
        if not argv:
            continue
        base = os.path.basename(argv[0].decode("utf-8", "replace"))
        if base == name or any(name.encode() in a for a in argv[1:]):
            out.append(int(e))
    return out


def socket_inodes(proc, pid):
    """inodes of every socket this pid holds open."""
    out = []
    d = os.path.join(proc, str(pid), "fd")
    try:
        fds = os.listdir(d)
    except OSError:
        return out
    for fd in fds:
        p = os.path.join(d, fd)
        try:
            t = os.readlink(p)
        except OSError:
            continue
        if t.startswith("socket:["):
            try:
                out.append(int(t[8:-1]))
            except ValueError:
                pass
    return out


def udp_ports(proc):
    """inode -> local port map from /proc/net/udp6 (+ udp, v4 sockets are legal)."""
    m = {}
    for name in ("udp6", "udp"):
        raw = _read(os.path.join(proc, "net", name))
        if not raw:
            continue
        lines = raw.decode("utf-8", "replace").splitlines()
        for ln in lines[1:]:
            f = ln.split()
            if len(f) < 10:
                continue
            local, inode = f[1], f[9]
            if ":" not in local:
                continue
            try:
                port = int(local.rsplit(":", 1)[1], 16)
                m[int(inode)] = port
            except ValueError:
                continue
    return m


def discover(proc, name, want_pid=None):
    res = {"port": None, "pid": None, "inodes": [], "candidates": [], "source": None}
    pids = [want_pid] if want_pid else find_pids(proc, name)
    if not pids:
        res["source"] = "no process named %r under %s" % (name, proc)
        return res
    ports = udp_ports(proc)
    for pid in pids:
        inodes = socket_inodes(proc, pid)
        if not inodes:
            continue
        cand = sorted(set(ports[i] for i in inodes if i in ports and ports[i] != 0))
        res["inodes"].extend(inodes)
        res["candidates"].extend(cand)
        if res["pid"] is None:
            res["pid"] = pid
    res["candidates"] = sorted(set(res["candidates"]))
    if len(res["candidates"]) == 1:
        res["port"] = res["candidates"][0]
        res["source"] = "pid %s -> socket:%s -> udp6 port %d" % (
            "/".join(str(p) for p in pids), res["inodes"], res["port"])
    elif not res["candidates"]:
        res["source"] = "pid %s owns no bound UDP socket" % ("/".join(str(p) for p in pids))
    else:
        res["source"] = "ambiguous: pid %s owns UDP ports %s" % (
            "/".join(str(p) for p in pids), res["candidates"])
    return res


def main(argv):
    proc, name, want_pid, as_json = "/proc", NAME, None, False
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--proc" and i + 1 < len(argv):
            proc = argv[i + 1]; i += 2
        elif a == "--name" and i + 1 < len(argv):
            name = argv[i + 1]; i += 2
        elif a == "--pid" and i + 1 < len(argv):
            want_pid = int(argv[i + 1]); i += 2
        elif a == "--json":
            as_json = True; i += 1
        else:
            sys.stderr.write(__doc__ + "\nbad arg: %s\n" % a)
            return 2
    r = discover(proc, name, want_pid)
    if as_json:
        sys.stdout.write('{"port": %s, "pid": %s, "candidates": %s, "inodes": %s, "source": "%s"}\n'
                         % (r["port"] if r["port"] is not None else "null",
                            r["pid"] if r["pid"] is not None else "null",
                            r["candidates"], r["inodes"],
                            r["source"].replace('"', "'")))
    elif r["port"] is not None:
        sys.stdout.write("%d\n" % r["port"])
    else:
        sys.stderr.write("msport: %s\n" % r["source"])
    if r["port"] is not None:
        return 0
    return 3 if r["candidates"] else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
