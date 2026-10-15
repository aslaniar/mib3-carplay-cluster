#!/usr/bin/env python
# ============================================================================
# inject_shim.py -- install the shim's seams into an ALREADY RUNNING carplay.
#
# WHY THIS AND NOT JUST dlopen:
#   Symbol interposition depends on LOAD ORDER -- the first object in the global
#   scope that defines a symbol wins. A library loaded with dlopen() is appended
#   LAST, so every existing call still resolves to the real LG/Apple function.
#   dlopen alone therefore delivers the code but installs NOTHING (which is why
#   the shim loaded, produced no log, and changed no behaviour).
#
#   So do it directly: resolve the real function AND the shim's replacement, then
#   find the pointers that already hold the real address and repoint them at the
#   shim. Those pointers are the GOT slots in the callers' data segments; by the
#   time a session has run once they are already bound to the real function,
#   which is exactly what makes them findable.
#
#   The shim's OWN mappings are skipped, so its saved "real function" pointers
#   are not clobbered (that would make it call itself forever).
#
#   /usr/bin/python inject_shim.py <pid> /dev/shm/mib3shim.so
#
# Everything is RAM only. Nothing is written to disk; a reboot erases it.
# ============================================================================
import ctypes, os, signal, struct, sys

PTRACE_ATTACH    = 16
PTRACE_DETACH    = 17
PTRACE_CONT      = 7
PTRACE_GETREGSET = 0x4204
PTRACE_SETREGSET = 0x4205
NT_PRSTATUS      = 1
NREGS            = 34
WALL             = getattr(os, "WALL", 0x40000000)
RTLD_NOW         = 2
RTLD_GLOBAL      = 0x100
RTLD_DEFAULT     = 0

SEAMS = ("AirPlayInfoArrayAddScreenDisplay", "AirPlayReceiverSessionSetup")

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
        raise OSError("ptrace(%d,%d): %s" % (req, pid, os.strerror(ctypes.get_errno())))
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

def sym_in_self(libname, soname, sym):
    a = lib_base(os.getpid(), libname)
    if not a:
        return None
    try:
        fn = getattr(ctypes.CDLL(soname, use_errno=True), sym)
    except AttributeError:
        return None
    p = ctypes.cast(fn, ctypes.c_void_p).value
    return (a, p) if p else None

def resolve_target(pid, libname, soname, sym):
    r = sym_in_self(libname, soname, sym)
    if not r:
        return None
    self_b, self_p = r
    tgt_b = lib_base(pid, libname)
    if not tgt_b:
        return None
    return tgt_b + (self_p - self_b)

def stack_lo(pid):
    for line in open("/proc/%d/maps" % pid):
        if line.rstrip().endswith("[stack]"):
            return int(line.split("-")[0], 16)
    return None

def pick_scratch(pid, sp):
    lo = stack_lo(pid)
    if lo:
        c = lo + 0x200
        if c < sp - 0x2000:
            return c
    return sp - 0x8000

def threads(pid):
    try:
        return sorted(int(t) for t in os.listdir("/proc/%d/task" % pid))
    except OSError:
        return [pid]

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
    regs = list(orig)
    regs[0], regs[1], regs[30], regs[32] = a0, a1, 0, fn
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

def cstr(pid, addr, n=256):
    try:
        return read_mem(pid, addr, n).split("\0")[0]
    except Exception, e:
        return "<read failed: %s>" % e

def rw_regions(pid):
    """All FILE-BACKED readable mappings -- that is where GOT slots live.

    Do NOT restrict to 'w': RELRO remaps .got to read-only after relocation, so
    the slots we need sit in r--p mappings. Restricting to rw- is what made the
    first run 'find' only our own stack residue (sp-8, the dlsym return value).
    Non-file mappings ([stack], [heap], [vvar], [vdso]) are excluded.
    The shim's own mappings are excluded so its saved real-function pointers are
    not clobbered (that would make it call itself forever).
    """
    out = []
    for line in open("/proc/%d/maps" % pid):
        p = line.rstrip().split()
        if len(p) < 6:
            continue
        perms, path = p[1], p[5]
        if "r" not in perms:
            continue
        if not path.startswith("/"):
            continue
        if "mib3shim" in path:
            continue
        lo, hi = [int(x, 16) for x in p[0].split("-")]
        if hi - lo > 64 * 1024 * 1024:
            continue
        out.append((lo, hi, path))
    return out

def find_points_to(pid, value, regions):
    """Every 8-byte-aligned occurrence of `value` in the target's writable data."""
    needle = struct.pack("Q", value)
    hits = []
    for lo, hi, path in regions:
        try:
            data = read_mem(pid, lo, hi - lo)
        except Exception:
            continue
        i = data.find(needle)
        while i != -1 and (i % 8) == 0:
            hits.append((lo + i, path))
            i = data.find(needle, i + 1)
    return hits

def main():
    if len(sys.argv) != 3:
        print "usage: inject_shim.py <pid> </path/to/mib3shim.so>"
        return 2
    pid = int(sys.argv[1]); so = sys.argv[2]
    if not os.path.exists(so):
        print "[!] %s missing" % so
        return 1

    dlopen = resolve_target(pid, "libdl", "libdl.so.2", "dlopen") or \
             resolve_target(pid, "libc", "libc.so.6", "__libc_dlopen_mode")
    dlsym = resolve_target(pid, "libdl", "libdl.so.2", "dlsym")
    if not dlopen or not dlsym:
        print "[!] could not resolve dlopen/dlsym in the target"
        return 1
    print "[i] dlopen=0x%x  dlsym=0x%x" % (dlopen, dlsym)

    tids = threads(pid)
    attached = []
    orig = None
    try:
        for t in tids:
            try:
                ptrace(PTRACE_ATTACH, t, 0, 0)
                os.waitpid(t, WALL)
                attached.append(t)
            except OSError:
                pass
        if not attached:
            print "[!] could not attach -- SMACK blocking ptrace?"
            return 1
        print "[+] attached %d/%d threads" % (len(attached), len(tids))
        orig = getregs(pid)

        scratch = pick_scratch(pid, orig[31])
        signal.alarm(20)
        try:
            # ---- dlopen the shim, GLOBAL so it CAN take part in lookup -------
            write_mem(pid, scratch, so + "\0")
            h = call_in_target(pid, dlopen, scratch, RTLD_NOW | RTLD_GLOBAL, orig)
            print "[%s] dlopen -> 0x%x" % ("+" if h else "!", h)
            if not h:
                print "[i] note: already loaded is fine only if dlsym below works"

            # ---- for each seam: shim's version and the real one ---------------
            for seam in SEAMS:
                write_mem(pid, scratch, seam + "\0")
                shim_fn = call_in_target(pid, dlsym, h, scratch, orig)
                real_fn = call_in_target(pid, dlsym, RTLD_DEFAULT, scratch, orig)
                print "[i] %s\n      real=0x%x  shim=0x%x" % (seam, real_fn, shim_fn)
                if not real_fn or not shim_fn or real_fn == shim_fn:
                    print "      -> cannot patch (unresolved)"
                    continue

                regions = rw_regions(pid)
                hits = find_points_to(pid, real_fn, regions)
                print "      -> %d pointer(s) point at the real function" % len(hits)
                ok = 0
                for addr, path in hits:
                    try:
                        write_mem(pid, addr, struct.pack("Q", shim_fn))
                        print "         patched 0x%x  in %s" % (addr, path)
                        ok += 1
                    except Exception, e:
                        print "         FAILED  0x%x  in %s : %s" % (addr, path, e)
                print "      -> patched %d/%d" % (ok, len(hits))
                left = find_points_to(pid, real_fn, regions)
                print "      -> remaining references to real: %d" % len(left)
        finally:
            signal.alarm(0)
    except Timeout, e:
        print "[!] %s" % e
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

    print "[i] now plug the iPhone and watch /tmp/mib3shim.log"
    return 0

sys.exit(main())
