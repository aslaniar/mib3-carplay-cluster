#!/usr/bin/env python
# SPDX-License-Identifier: GPL-3.0-or-later
# ============================================================================
# inject_shim.py -- install the shim's seams into an ALREADY RUNNING carplay.
#
# Every address is resolved DETERMINISTICALLY: the real symbol from the ELF files
# on disk, plus the process's load bases in /proc/maps.  There is no dlsym inside
# the target -- that returned garbage once and silently patched a call slot with a
# bogus address, so the shim was never entered and the session died.
#
# WHY PATCH AT ALL: symbol interposition depends on load order, and a dlopen()ed
# library lands LAST, so every call still resolves to the real function.  So the
# GOT slots that already hold the real addresses are repointed at the shim's own
# symbols, then READ BACK and asserted.  The run fails (non-zero) if a seam has no
# slot, if a slot does not verify, or if the on-disk shim's md5 is not the expected
# one -- a "0/0 patched" run can no longer look like success.
#
# py2/py3 compatible: the unit is python2, the desk runs it through
# python3 -m py_compile as a syntax check.
#
#   /usr/bin/python inject_shim.py <pid> /dev/shm/mib3shim.so [expected_md5]
#
# RAM only; a reboot erases it.
# ============================================================================
from __future__ import print_function
import os, struct, sys

try:
    xrange
except NameError:
    xrange = range

PTRACE_ATTACH = 16
PTRACE_DETACH = 17
WALL = getattr(os, "WALL", 0x40000000)

# (symbol, defining library, library whose GOT holds the call slot)
# AirPlayInfoArrayAddScreenDisplay is called FROM libCarPlayAppFramework, so its
# slot lives there; the other two are intra-libAirPlay calls via libAirPlay's own
# PLT, so their slots live in libAirPlay (verified: R_AARCH64_JUMP_SLOT
# .got+0x3ec40 -> AirPlayCopyServerInfo; .got+0x3e440 -> AirPlayReceiverSessionSetup).
SEAMS = (
    ("AirPlayInfoArrayAddScreenDisplay", "libAirPlay.so", "libCarPlayAppFramework.so"),
    ("AirPlayReceiverSessionSetup",     "libAirPlay.so", "libAirPlay.so"),
    ("AirPlayCopyServerInfo",           "libAirPlay.so", "libAirPlay.so"),
)

def md5_file(path):
    try:
        import hashlib
        h = hashlib.md5()
        f = open(path, "rb")
        while True:
            b = f.read(65536)
            if not b:
                break
            h.update(b)
        f.close()
        return h.hexdigest()
    except Exception:
        return None

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
        return f[o:f.index(b"\0", o)].decode("latin1")
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
                n = f[stroff + nameoff:f.index(b"\0", stroff + nameoff)].decode("latin1")
                syms[n] = val
    return syms

def _elf_sections(f):
    e_shoff     = struct.unpack_from("<Q", f, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", f, 0x3a)[0]
    e_shnum     = struct.unpack_from("<H", f, 0x3c)[0]
    e_shstrndx  = struct.unpack_from("<H", f, 0x3e)[0]
    sh = [struct.unpack_from("<IIQQQQIIQQ", f, e_shoff + i * e_shentsize)
          for i in range(e_shnum)]
    return sh, e_shstrndx

def elf_jumpslots(path, want):
    """r_offsets of R_AARCH64_JUMP_SLOT relocations whose symbol is `want`.

    The deterministic way to find a seam's GOT slot: a value scan cannot tell two
    seams in the SAME library apart once one of them has been repointed into a
    shim (both slots then 'point into a shim'), which is how SessionSetup's slot
    was stolen by CopyServerInfo."""
    f = open(path, "rb").read()
    sh, shstrndx = _elf_sections(f)
    def sname(i):
        o = sh[shstrndx][4] + sh[i][0]
        return f[o:f.index(b"\0", o)].decode("latin1")
    dynidx = None
    for i, s in enumerate(sh):
        if sname(i) == ".dynsym":
            dynidx = i
            break
    if dynidx is None:
        return []
    symoff = sh[dynidx][4]
    stroff = sh[sh[dynidx][6]][4]
    def symname(k):
        o = symoff + k * 24
        nameoff = struct.unpack_from("<I", f, o)[0]
        if not nameoff:
            return ""
        return f[stroff + nameoff:f.index(b"\0", stroff + nameoff)].decode("latin1")
    out = []
    for s in sh:
        if s[1] != 4 or s[9] != 24:        # SHT_RELA with 24-byte entries
            continue
        reloff, relsize = s[4], s[5]
        for k in range(relsize // 24):
            r_offset, r_info, _ = struct.unpack_from("<QQq", f, reloff + k * 24)
            if (r_info & 0xffffffff) == 1026 and symname(r_info >> 32) == want:
                out.append(r_offset)
    return out

def mapped_path(pid, suffix):
    for lo, hi, perms, off, path in maps(pid):
        if off == "00000000" and path.endswith(suffix):
            return path
    return None

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

def scan_regions(pid, hold_lib):
    """File-backed readable mappings OF THE OWNING LIBRARY -- where its GOT slots
    live. NOT restricted to writable: RELRO remaps .got read-only after
    relocation. Scanning only the owning library (not every readable region)
    removes false-positive matches of the same qword elsewhere (e.g. a
    function-pointer table) -- a stale/bogus slot could otherwise be repointed."""
    return [(lo, hi, p) for lo, hi, perms, off, p in maps(pid)
            if "r" in perms and p.endswith(hold_lib)
            and (hi - lo) <= 64 * 1024 * 1024]

def shim_ranges(pid):
    """Mappings of the loaded shim(s). A slot pointing anywhere in here is a slot
    WE patched earlier -- so re-injection can find and repair its own work."""
    out = []
    for lo, hi, perms, off, p in maps(pid):
        if "mib3shim" in p:
            out.append((lo, hi))
    return out

def in_shim(v, shim_rng):
    for a, b in shim_rng:
        if a <= v < b:
            return True
    return False

def find_slots(pid, real, shim_rng, regions):
    """Slots to (re)point at the shim: they hold the REAL address (fresh
    injection), or they point into a shim mapping (a previous injection, which
    would otherwise leave the new shim unreferenced -- the 0/0 patched case)."""
    out = []
    for lo, hi, path in regions:
        try:
            data = read_mem(pid, lo, hi - lo)
        except Exception:
            continue
        if len(data) < 8:
            continue
        for i in xrange(0, len(data) - 8 + 1, 8):
            v = struct.unpack_from("Q", data, i)[0]
            if v == real or in_shim(v, shim_rng):
                out.append((lo + i, path))
    return out

def main():
    if len(sys.argv) < 3 or len(sys.argv) > 4:
        print("usage: inject_shim.py <pid> </path/to/mib3shim.so> [expected_md5]")
        return 2
    pid = int(sys.argv[1]); shim_path = sys.argv[2]
    want_md5 = sys.argv[3] if len(sys.argv) == 4 else None
    if not os.path.exists(shim_path):
        print("[!] %s missing" % shim_path)
        return 1

    got_md5 = md5_file(shim_path)
    print("[i] shim md5 = %s%s" % (got_md5, ("  want " + want_md5) if want_md5 else ""))
    if want_md5 and got_md5 and got_md5 != want_md5:
        print("[!] SHIM MD5 MISMATCH -- refusing to install the wrong artifact")
        return 1

    shim_base = load_base(pid, os.path.basename(shim_path))
    if not shim_base:
        print("[!] the shim is not mapped in pid %d -- dlopen it first (inject.py)" % pid)
        return 1
    shim_syms = elf_dynsym(shim_path)
    shim_rng = shim_ranges(pid)
    print("[i] shim base = 0x%x   shim mappings = %d" % (shim_base, len(shim_rng)))

    # ---- resolve + validate every plan BEFORE touching the target -------------
    plan = []
    for sym, lib, hold in SEAMS:
        lib_base = load_base(pid, lib)
        if not lib_base:
            print("[!] %s not mapped in the target" % lib)
            return 1
        lib_syms = elf_dynsym("/lge/app_rw/lib64/" + lib)
        if sym not in lib_syms:
            print("[!] %s not defined in %s" % (sym, lib))
            return 1
        if sym not in shim_syms:
            print("[!] %s not exported by the shim -- build.sh should have caught this" % sym)
            return 1
        real = lib_base + lib_syms[sym]
        shim = shim_base + shim_syms[sym]
        if not in_shim(shim, shim_rng):
            print("[!] shim symbol %s (+0x%x) is NOT inside a shim mapping -- abort" % (sym, shim_syms[sym]))
            return 1
        plan.append((sym, lib, hold, real, shim))
        print("[i] %s\n      real = 0x%x  (%s + 0x%x)\n      shim = 0x%x  (shim + 0x%x)\n      slot holder = %s" % (
            sym, real, lib, lib_syms[sym], shim, shim_syms[sym], hold))

    attached = []
    failures = []
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
            print("[!] could not attach (SMACK blocking ptrace?)")
            return 1
        print("[+] attached %d thread(s)" % len(attached))

        for sym, lib, hold, real, shim in plan:
            hold_base = load_base(pid, hold)
            hold_path = mapped_path(pid, hold)
            if not hold_base or not hold_path:
                failures.append("%s: %s not mapped" % (sym, hold))
                continue
            # the slot is the JUMP_SLOT relocation for this symbol in the CALLING
            # library -- see elf_jumpslots for why a value scan cannot do it.
            offs = elf_jumpslots(hold_path, sym)
            hits = [(hold_base + o, hold_path) for o in offs]
            print("[i] %s: %d relocation slot(s) in %s" % (sym, len(hits), hold))
            if not hits:
                failures.append("%s: no JUMP_SLOT relocation in %s" % (sym, hold))
                continue
            ok = 0
            for addr, path in hits:
                try:
                    old = struct.unpack_from("Q", read_mem(pid, addr, 8), 0)[0]
                    # sanity: holds the real symbol, or a previous shim's addr
                    if old != real and not in_shim(old, shim_rng):
                        print("      SKIP 0x%x: holds 0x%x (neither real nor a shim addr)" % (addr, old))
                        continue
                    write_mem(pid, addr, struct.pack("Q", shim))
                    # VERIFY: read the slot back immediately
                    back = struct.unpack_from("Q", read_mem(pid, addr, 8), 0)[0]
                    if back == shim:
                        print("      patched 0x%x  in %s  (verified)" % (addr, path))
                        ok += 1
                    else:
                        print("      VERIFY FAILED 0x%x (read back 0x%x)" % (addr, back))
                except Exception as e:
                    print("      FAILED  0x%x  in %s : %s" % (addr, path, e))
            if ok != len(hits):
                failures.append("%s: %d/%d verified" % (sym, ok, len(hits)))
            else:
                print("      -> %d/%d patched+verified" % (ok, len(hits)))
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
        print("[+] detached %d thread(s)" % len(attached))

    if failures:
        print("[!] INSTALL FAILED: " + "; ".join(failures))
        return 1

    print("[+] ALL SEAMS INSTALLED AND VERIFIED (%d)" % len(plan))
    print("[i] NEXT: plug in the iPhone.")
    print("    /dev/shm/mib3shim_hit  appears  => the shim WAS entered")
    print("    /tmp/mib3shim.log      shows     => how far it got")
    print("    want: 'init: resolved' -> 'AddScreenDisplay: ADDING cluster display'")
    print("          -> 'cluster: type=111'")
    print("          -> 'info: completed: uid/properties/initialVideoStreams + displayPanels'")
    print("          -> 'cluster: response entry {type:111,dataPort} appended'")
    return 0

if __name__ == "__main__":
    sys.exit(main())
