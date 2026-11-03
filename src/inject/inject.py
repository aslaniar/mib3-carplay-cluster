#!/usr/bin/env python
# ============================================================================
# inject.py -- load a .so into an ALREADY RUNNING process, via ptrace.
#
# WHY: restarting carplay.service reboots this head unit, so the shim cannot be
# delivered by a restart.  This attaches to the running process and dlopen()s the
# library into it: no restart -> no reboot -> retry freely.
#
#   /usr/bin/python inject.py <pid> </path/to/lib.so>
#
# RAM only: the library lives in the target's address space; a reboot (or the
# process exiting) erases it.  Nothing is written to disk.
#
# Things learned the hard way on the unit (each cost a run):
#   * glibc 2.23 keeps dlopen in libdl, NOT libc (merged only in 2.34).
#   * the real files are versioned: libdl-2.23.so, libc-2.23.so -- matching the
#     soname finds no mapping.
#   * this python2 has no os.pwrite -- use lseek+write.
#   * carplay is MULTI-THREADED (10 threads): attach every thread, or a stray
#     thread stop gets reported instead of ours.
#   * do NOT put a scratch BRK stub on the stack -- it is rw-, not x, and faults
#     SEGV_ACCERR (si_code=2).  Use lr=0 and accept the fault instead.
#   * always restore registers and detach, even on error.
#   * py2 only (the unit's interpreter): `print` statements, `except E, e`.
# ============================================================================
import ctypes, os, signal, struct, sys

PTRACE_ATTACH    = 16
PTRACE_DETACH    = 17
PTRACE_CONT      = 7
PTRACE_GETREGSET = 0x4204
PTRACE_SETREGSET = 0x4205
PTRACE_GETSIGINFO= 0x4202
NT_PRSTATUS      = 1
NREGS            = 34
WALL             = getattr(os, "WALL", 0x40000000)
RTLD_NOW         = 2

libc = ctypes.CDLL("libc.so.6", use_errno=True)
libc.ptrace.restype  = ctypes.c_long
libc.ptrace.argtypes = [ctypes.c_ulong, ctypes.c_ulong,
                        ctypes.c_void_p, ctypes.c_void_p]

class Timeout(Exception):
    pass

def _alarm(sig, frm):
    raise Timeout("timed out")

signal.signal(signal.SIGALRM, _alarm)

def ptrace(req, pid, addr, data):
    r = libc.ptrace(req, pid, ctypes.c_void_p(addr), ctypes.c_void_p(data))
    if r == -1:
        raise OSError("ptrace(%d, tid=%d): %s" % (req, pid, os.strerror(ctypes.get_errno())))
    return r

def getregs(pid):
    buf = ctypes.create_string_buffer(NREGS * 8)
    iov = ctypes.create_string_buffer(struct.pack("QQ", ctypes.addressof(buf), NREGS * 8), 16)
    ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, ctypes.addressof(iov))
    return list(struct.unpack("34Q", buf.raw))

def setregs(pid, regs):
    raw = struct.pack("34Q", *regs)
    buf = ctypes.create_string_buffer(raw, len(raw))
    iov = ctypes.create_string_buffer(struct.pack("QQ", ctypes.addressof(buf), len(raw)), 16)
    ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, ctypes.addressof(iov))

def lib_base(pid, name):
    """Base of a mapped library. Real files are versioned (libc-2.23.so), so
    match the name against the versioned and soname forms."""
    best = None
    for line in open("/proc/%d/maps" % pid):
        p = line.split()
        if len(p) < 6:
            continue
        b = p[5].rsplit("/", 1)[-1]
        if b == name or b.startswith(name + "-") or b.startswith(name + ".so"):
            a = int(p[0].split("-")[0], 16)
            if best is None or a < best:
                best = a
    return best

def resolve_sym(pid, libname, soname, sym):
    """Address of sym IN THE TARGET. Same library file in both processes, so the
    offset from the library base is the same."""
    self_b = lib_base(os.getpid(), libname)
    tgt_b  = lib_base(pid, libname)
    if not self_b or not tgt_b:
        return None
    try:
        fn = getattr(ctypes.CDLL(soname, use_errno=True), sym)
    except AttributeError:
        return None
    a = ctypes.cast(fn, ctypes.c_void_p).value
    if not a:
        return None
    return tgt_b + (a - self_b)

def resolve_dlopen(pid):
    for libname, soname, sym in (("libdl", "libdl.so.2", "dlopen"),
                                 ("libc",  "libc.so.6",  "__libc_dlopen_mode")):
        a = resolve_sym(pid, libname, soname, sym)
        if a:
            return (soname, sym, a)
    return None

def stack_lo(pid):
    """Start of the [stack] mapping -- as far from the live frames as we can get."""
    for line in open("/proc/%d/maps" % pid):
        if line.rstrip().endswith("[stack]"):
            return int(line.split("-")[0], 16)
    return None

def pick_scratch(pid, sp):
    """Where to put the string.

    DO NOT use sp-0x400: a function call pushes frames BELOW sp and overwrites
    it -- that is exactly what clobbered our path string (the loader then saw
    garbage and reported ENOENT on a file that exists). Put it near the bottom of
    the stack mapping instead, well clear of the live frames.
    """
    lo = stack_lo(pid)
    if lo:
        cand = lo + 0x200
        if cand < sp - 0x2000:
            return cand
    return sp - 0x8000

def threads(pid):
    try:
        return sorted(int(t) for t in os.listdir("/proc/%d/task" % pid))
    except OSError:
        return [pid]

    try:
        return sorted(int(t) for t in os.listdir("/proc/%d/task" % pid))
    except OSError:
        return [pid]

def decode(status):
    if os.WIFSTOPPED(status):
        return "stopped sig=%d" % os.WSTOPSIG(status)
    if os.WIFSIGNALED(status):
        return "KILLED sig=%d" % os.WTERMSIG(status)
    if os.WIFEXITED(status):
        return "exited code=%d" % os.WEXITSTATUS(status)
    return "status=0x%x" % status

def read_mem(pid, addr, n):
    fd = os.open("/proc/%d/mem" % pid, os.O_RDONLY)
    try:
        os.lseek(fd, addr, os.SEEK_SET)
        return os.read(fd, n)
    finally:
        os.close(fd)

def write_mem(pid, addr, data):
    fd = os.open("/proc/%d/mem" % pid, os.O_RDWR)
    try:
        os.lseek(fd, addr, os.SEEK_SET)
        os.write(fd, data)
    finally:
        os.close(fd)

def call_in_target(pid, fn, a0, a1, orig):
    """Call fn(a0,a1) inside the target; return its x0.

    lr=0, so the return faults and we get control back at a clean stop. The
    fault is suppressed on detach. No executable scratch stub is used: the stack
    is rw- and would fault with SEGV_ACCERR.
    """
    regs = list(orig)
    regs[0]  = a0
    regs[1]  = a1
    regs[30] = 0
    regs[32] = fn
    setregs(pid, regs)
    ptrace(PTRACE_CONT, pid, 0, 0)
    while True:
        w, status = os.waitpid(-1, WALL)
        if w == pid:
            return getregs(pid)[0]
        try:
            ptrace(PTRACE_CONT, w, 0, 0)
        except OSError:
            pass

def cstr(pid, addr, maxlen=256):
    try:
        return read_mem(pid, addr, maxlen).split("\0")[0]
    except Exception, e:
        return "<read failed: %s>" % e

def main():
    if len(sys.argv) != 3:
        print "usage: inject.py <pid> <lib.so>"
        return 2
    pid = int(sys.argv[1]); so = sys.argv[2]
    if not os.path.exists(so):
        print "[!] %s does not exist" % so
        return 1

    r = resolve_dlopen(pid)
    if not r:
        print "[!] could not resolve a dlopen in the target"
        return 1
    soname, sym, dlopen = r
    dlerror = resolve_sym(pid, "libdl", "libdl.so.2", "dlerror") or \
              resolve_sym(pid, "libc", "libc.so.6", "dlerror")
    print "[i] %s:%s -> 0x%x   dlerror -> %s" % (
        soname, sym, dlopen, ("0x%x" % dlerror) if dlerror else "unresolved")

    tids = threads(pid)
    attached = []
    orig = None
    try:
        for t in tids:
            try:
                ptrace(PTRACE_ATTACH, t, 0, 0)
                os.waitpid(t, WALL)
                attached.append(t)
            except OSError, e:
                print "[i] attach tid %d failed: %s" % (t, e)
        if not attached:
            print "[!] attached to nothing -- SMACK blocking ptrace?"
            print "    echo 'common::lib carplay::app rwx' > /smack/load2"
            return 1
        print "[+] attached %d/%d threads" % (len(attached), len(tids))

        orig = getregs(pid)
        path_addr = pick_scratch(pid, orig[31])
        write_mem(pid, path_addr, so + "\0")
        print "[i] path at 0x%x (sp=0x%x)  readback=%r" % (
            path_addr, orig[31], cstr(pid, path_addr))

        signal.alarm(15)
        try:
            handle = call_in_target(pid, dlopen, path_addr, RTLD_NOW, orig)
        finally:
            signal.alarm(0)
        print "[%s] dlopen returned 0x%x" % ("+" if handle else "!", handle)

        if not handle and dlerror:
            signal.alarm(15)
            try:
                ep = call_in_target(pid, dlerror, 0, 0, orig)
            finally:
                signal.alarm(0)
            print "[i] dlerror() -> 0x%x : %r" % (ep, cstr(pid, ep) if ep else "")
    except Timeout, e:
        print "[!] %s -- the target may be wedged; detaching" % e
        return 1
    finally:
        try:
            if orig:
                setregs(pid, orig)
        except Exception:
            pass
        for t in attached:
            try:
                ptrace(PTRACE_DETACH, t, 0, 0)
            except OSError:
                pass
        print "[+] detached %d thread(s); registers restored" % len(attached)

    if not handle:
        print "[!] the library did NOT load -- see dlerror above"
        return 1
    print "[+] INJECTED %s into pid %d" % (so, pid)
    return 0

sys.exit(main())
