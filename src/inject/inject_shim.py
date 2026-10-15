#!/usr/bin/env python
# ============================================================================
# inject_shim.py -- install the shim's seams into an ALREADY RUNNING carplay.
#
# v2: resolves EVERY address deterministically from the ELF files on disk plus
#     the process's load bases in /proc/maps. No dlsym inside the target (the v1
#     dlsym call returned garbage, which silently patched a call slot with a
#     bogus address -- so the shim was never entered and the session died).
#
# WHY PATCH AT ALL: symbol interposition depends on load order, and a dlopen()ed
# library lands LAST, so every call still resolves to the real function. So find
# the GOT slots that already hold the real addresses and repoint them at the
# shim's own symbols.
#
#   /usr/bin/python inject_shim.py <pid> /dev/shm/mib3shim.so
#
# RAM only; a reboot erases it.
# ============================================================================
import os, struct, sys

PTRACE_ATTACH = 16
PTRACE_DETACH = 17
WALL = getattr(os, "WALL", 0x40000000)

SEAMS = (
    ("AirPlayInfoArrayAddScreenDisplay", "libAirPlay.so"),
    ("AirPlayReceiverSessionSetup",     "libAirPlay.so"),
)

def threads(pid):
    try:
        return sorted(int(t) for t in os.listdir("/proc/%d/task" % pid))
    except OSError:
        return [pid]

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
    """Base of a mapped object: its first mapping at file offset 0."""
    best = None
    for lo, hi, perms, off, path in maps(pid):
        if path.endswith(suffix) and off == "00000000":
            if best is None or lo < best:
                best = lo
    return best

def elf_dynsym(path):
    f = open(path, "rb").read()
    e_shoff     = struct.unpack_from("<Q", f, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", f, 0x3a)[0]
    e_shnum     = struct.unpack_from("<H", f, 0x3c)[0]
    e_shstrndx  = struct.unpack_from("<H", f, 0x3e)[0]
    sh = [struct.unpack_from("<IIQQQQIIQQ", f, e_shoff + i * e_shentsize)
          for i in range(e_shnum)]
    def sname(i):
        o = sh[e_shstrndx][4] + sh[i][0]
        return f[o:f.index("\0", o)]
    syms = {}
    for i, s in enumerate(sh):
        if sname(i) != ".dynsym":
            continue
        symoff, symsize, link = s[4], s[5], s[6]
        stroff = sh[link][4]
        for k in range(symsize // 24):
            o = symoff + k * 24
            nameoff, info, other, shndx, val, size = struct.unpack_from("<IBBHQQ", f, o)
            if nameoff:
                n = f[stroff + nameoff:f.index("\0", stroff + nameoff)]
                syms[n] = val
    return syms

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

def scan_regions(pid):
    """File-backed readable mappings -- where GOT slots live. NOT restricted to
    writable: RELRO remaps .got read-only after relocation. The shim's own
    mappings are excluded so its saved real pointers are never clobbered."""
    return [(lo, hi, p) for lo, hi, perms, off, p in maps(pid)
            if "r" in perms and p.startswith("/") and "mib3shim" not in p
            and (hi - lo) <= 64 * 1024 * 1024]

def find_points_to(pid, value, regions):
    needle = struct.pack("Q", value)
    hits = []
    for lo, hi, path in regions:
        try:
            data = read_mem(pid, lo, hi - lo)
        except Exception:
            continue
        i = data.find(needle)
        while i != -1:
            if i % 8 == 0:
                hits.append((lo + i, path))
            i = data.find(needle, i + 1)
    return hits

def main():
    if len(sys.argv) != 3:
        print "usage: inject_shim.py <pid> </path/to/mib3shim.so>"
        return 2
    pid = int(sys.argv[1]); shim_path = sys.argv[2]
    if not os.path.exists(shim_path):
        print "[!] %s missing" % shim_path
        return 1

    shim_base = load_base(pid, os.path.basename(shim_path))
    if not shim_base:
        print "[!] the shim is not mapped in pid %d -- dlopen it first" % pid
        return 1
    shim_syms = elf_dynsym(shim_path)
    print "[i] shim base = 0x%x" % shim_base

    plan = []
    for sym, lib in SEAMS:
        lib_base = load_base(pid, lib)
        if not lib_base:
            print "[!] %s not mapped in the target" % lib
            return 1
        lib_syms = elf_dynsym("/lge/app_rw/lib64/" + lib)
        if sym not in lib_syms or sym not in shim_syms:
            print "[!] %s not found in %s/shim" % (sym, lib)
            return 1
        real = lib_base + lib_syms[sym]
        shim = shim_base + shim_syms[sym]
        plan.append((sym, lib, real, shim))
        print "[i] %s\n      real = 0x%x  (%s + 0x%x)\n      shim = 0x%x  (shim + 0x%x)" % (
            sym, real, lib, lib_syms[sym], shim, shim_syms[sym]))

    attached = []
    try:
        for t in threads(pid):
            try:
                import ctypes
                libc = ctypes.CDLL("libc.so.6", use_errno=True)
                libc.ptrace.restype = ctypes.c_long
                libc.ptrace.argtypes = [ctypes.c_ulong, ctypes.c_ulong,
                                        ctypes.c_void_p, ctypes.c_void_p]
                if libc.ptrace(PTRACE_ATTACH, t, None, None) == -1:
                    continue
                os.waitpid(t, WALL)
                attached.append(t)
            except Exception:
                pass
        if not attached:
            print "[!] could not attach (SMACK blocking ptrace?)"
            return 1
        print "[+] attached %d thread(s)" % len(attached)

        regions = scan_regions(pid)
        for sym, lib, real, shim in plan:
            hits = find_points_to(pid, real, regions)
            print "[i] %s: %d pointer(s) to the real function" % (sym, len(hits))
            ok = 0
            for addr, path in hits:
                try:
                    write_mem(pid, addr, struct.pack("Q", shim))
                    print "      patched 0x%x  in %s" % (addr, path)
                    ok += 1
                except Exception, e:
                    print "      FAILED  0x%x  in %s : %s" % (addr, path, e)
            print "      -> %d/%d patched" % (ok, len(hits))
            left = find_points_to(pid, real, regions)
            print "      -> remaining references to real: %d" % len(left)
    finally:
        import ctypes
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        libc.ptrace.restype = ctypes.c_long
        libc.ptrace.argtypes = [ctypes.c_ulong, ctypes.c_ulong,
                                ctypes.c_void_p, ctypes.c_void_p]
        for t in attached:
            try:
                libc.ptrace(PTRACE_DETACH, t, None, None)
            except Exception:
                pass
        print "[+] detached %d thread(s)" % len(attached)

    print "[i] NEXT: plug in the iPhone."
    print "    /dev/shm/mib3shim_hit  appears  => the shim WAS entered"
    print "    /tmp/mib3shim.log      shows     => how far it got"
    return 0

sys.exit(main())
