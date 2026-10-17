#!/usr/bin/env python
# ============================================================================
# target_env.py -- set an environment variable INSIDE a running process.
#
#   /usr/bin/python target_env.py <pid> "MIB3_SHIM_ADD=0"
#
# WHY: the injected shim reads its knobs with getenv(). We cannot change the
# target's environment from outside -- but we can call setenv() inside it, and
# the shim's next getenv() will see the new value. That lets us flip behaviour
# (advertise on/off, gate forced, surface/bridge off/on) between phone plugs
# with NO rebuild.
#
# 2026-10-16 FIX: putenv() -> setenv().
#   glibc putenv() stores the POINTER to the caller's string (no copy), and the
#   scratch we handed it lives on the target's STACK -- which gets reused before
#   the next plug, so the value silently evaporated (every fresh RUN banner read
#   SURFACE=0, and no bridge/surface log appeared, across RUN 4 + RUN 5 even
#   though both putenv calls returned "0 = ok"). setenv() COPIES the value onto
#   the heap, so it survives to the next getenv(). A read-back is printed so the
#   knob can be confirmed BEFORE a plug.
#
# RAM only; a reboot erases it.
# ============================================================================
import os, struct, sys

PTRACE_ATTACH = 16
PTRACE_DETACH = 17
PTRACE_CONT    = 7
PTRACE_GETREGSET = 0x4204
PTRACE_SETREGSET = 0x4205
NT_PRSTATUS   = 1
NREGS         = 34
WALL          = getattr(os, "WALL", 0x40000000)

def maps(pid):
    out = []
    for line in open("/proc/%d/maps" % pid):
        p = line.rstrip().split()
        if len(p) < 6:
            continue
        lo, hi = [int(x, 16) for x in p[0].split("-")]
        out.append((lo, hi, p[1], p[2], p[5]))
    return out

def load_base(pid, suffix):
    best = None
    for lo, hi, perms, off, path in maps(pid):
        if path.endswith(suffix) and off == "00000000":
            if best is None or lo < best:
                best = lo
    return best

def stack_lo(pid):
    for line in open("/proc/%d/maps" % pid):
        if line.rstrip().endswith("[stack]"):
            return int(line.split("-")[0], 16)
    return None

def main():
    if len(sys.argv) != 3:
        print "usage: target_env.py <pid> NAME=VALUE"
        return 2
    pid = int(sys.argv[1]); kv = sys.argv[2]
    if "=" not in kv:
        print "[!] needs NAME=VALUE"
        return 1
    name, _, value = kv.partition("=")

    import ctypes, signal
    class TO(Exception):
        pass
    def _a(s, f):
        raise TO()
    signal.signal(signal.SIGALRM, _a)
    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    libc.ptrace.restype = ctypes.c_long
    libc.ptrace.argtypes = [ctypes.c_ulong, ctypes.c_ulong,
                            ctypes.c_void_p, ctypes.c_void_p]

    def ptrace(req, p, a, d):
        if libc.ptrace(req, p, ctypes.c_void_p(a), ctypes.c_void_p(d)) == -1:
            raise OSError("ptrace %d: %s" % (req, os.strerror(ctypes.get_errno())))

    def getregs(p):
        buf = ctypes.create_string_buffer(NREGS * 8)
        iov = ctypes.create_string_buffer(struct.pack("QQ", ctypes.addressof(buf), NREGS * 8), 16)
        ptrace(PTRACE_GETREGSET, p, NT_PRSTATUS, ctypes.addressof(iov))
        return list(struct.unpack("34Q", buf.raw))

    def setregs(p, r):
        raw = struct.pack("34Q", *r)
        buf = ctypes.create_string_buffer(raw, len(raw))
        iov = ctypes.create_string_buffer(struct.pack("QQ", ctypes.addressof(buf), len(raw)), 16)
        ptrace(PTRACE_SETREGSET, p, NT_PRSTATUS, ctypes.addressof(iov))

    def wr(a, data):
        f = os.open("/proc/%d/mem" % pid, os.O_RDWR)
        try:
            os.lseek(f, a, os.SEEK_SET); os.write(f, data)
        finally:
            os.close(f)

    def rd(a, n):
        f = os.open("/proc/%d/mem" % pid, os.O_RDONLY)
        try:
            os.lseek(f, a, os.SEEK_SET); return os.read(f, n)
        finally:
            os.close(f)

    # setenv/getenv live in libc (same libc file for the target and for this tool)
    self_libc = ctypes.CDLL("libc.so.6")
    self_b = load_base(os.getpid(), "libc-2.23.so") or load_base(os.getpid(), "libc.so.6")
    tgt_b  = load_base(pid, "libc-2.23.so") or load_base(pid, "libc.so.6")
    setenv = tgt_b + (ctypes.cast(self_libc.setenv, ctypes.c_void_p).value - self_b)
    getenv = tgt_b + (ctypes.cast(self_libc.getenv, ctypes.c_void_p).value - self_b)
    print "[i] setenv in target = 0x%x" % setenv

    lo = stack_lo(pid) or 0
    tids = sorted(int(t) for t in os.listdir("/proc/%d/task" % pid))
    attached = []
    orig = None
    try:
        for t in tids:
            try:
                ptrace(PTRACE_ATTACH, t, 0, 0); os.waitpid(t, WALL); attached.append(t)
            except OSError:
                pass
        if not attached:
            print "[!] could not attach"
            return 1
        print "[+] attached %d threads" % len(attached)
        orig = getregs(pid)

        def call(fn, a0, a1=0, a2=0):
            # run one libc call in the MAIN thread, based on the attach-time regs
            r = list(orig)
            r[0], r[1], r[2], r[30], r[32] = a0, a1, a2, 0, fn
            setregs(pid, r)
            signal.alarm(10)
            try:
                ptrace(PTRACE_CONT, pid, 0, 0)
                while True:
                    w, st = os.waitpid(-1, WALL)
                    if w == pid:
                        return getregs(pid)[0]
                    try:
                        ptrace(PTRACE_CONT, w, 0, 0)
                    except OSError:
                        pass
            finally:
                signal.alarm(0)
            return 0

        need = len(name) + len(value) + 2
        scratch = (lo + 0x400) if (lo and lo + 0x400 + need < orig[31] - 0x2000) else (orig[31] - 0x8000)
        wr(scratch, name + "\0")
        wr(scratch + len(name) + 1, value + "\0")
        print "[i] wrote %r / %r at 0x%x" % (name, value, scratch)

        rc = call(setenv, scratch, scratch + len(name) + 1, 1)
        print "[+] setenv returned 0x%x (0 = ok)" % rc

        # read-back: getenv(name) in the target, then read the string it points at
        p = call(getenv, scratch)
        print "[i] getenv(%s) -> 0x%x" % (name, p)
        if p:
            s = rd(p, 256).split("\0")[0]
            print "[i] readback: %s=%s" % (name, s)
    finally:
        if orig:
            try:
                setregs(pid, orig)
            except Exception:
                pass
        for t in attached:
            try:
                ptrace(PTRACE_DETACH, t, 0, 0)
            except OSError:
                pass
        print "[+] detached"
    return 0

sys.exit(main())
