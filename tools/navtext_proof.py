#!/usr/bin/env python3
"""navtext_proof.py -- make the NAV-TEXT GATE's two claims executable.

The gate (shim/mib3navtext.c) drops records on carplay's /tmp/Simple_Socket
connection based on two facts read out of the SHIPPED binaries:

  (1) simplenav's Receive_Parsing dispatches on a u32 EVENT ID at offset 0 and a
      u32 TECHNOLOGY at offset 4, with the guidance events 100/101/102
      (0x64/0x65/0x66);
  (2) carplay reaches that socket through its OWN connect/send/close PLT slots --
      exactly one R_AARCH64_JUMP_SLOT each -- so a GOT patch inside the carplay
      executable is complete and unambiguous.

Both are asserted here against the firmware dump, so a firmware change (or a
sloppy edit to the gate) fails the bench instead of silently shipping a filter
that matches nothing.

  python3 tools/navtext_proof.py [ROOTFS]      exit 0 = both claims hold
"""
import os
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM
from elftools.elf.elffile import ELFFile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))          # .../car-projects
DEFAULT_ROOT = os.path.join(REPO, "R8830L_ROOTFS")

CARPLAY = "lge/app_rw/smartlink/carplay/carplay"
SIMPLENAV = "lge/app_rw/smartlink/simplenav/simplenav"
LIBC = "lib64/libc.so.6"

# A move is not fatal (the injector re-derives the slots from the relocations)
# but it must be noticed, because inject_shim.py's HOOKS table and the docs
# quote these numbers.
EXPECTED_GOT = {"connect": 0x371230, "send": 0x371388, "close": 0x371720}


class Elf(object):
    def __init__(self, path):
        self.path = path
        self.fh = open(path, "rb")
        self.e = ELFFile(self.fh)
        self.data = open(path, "rb").read()
        self.secs = {}
        for s in self.e.iter_sections():
            if s.header.sh_size:
                self.secs[s.name] = s

    def dynsym(self):
        out = {}
        s = self.secs.get(".dynsym")
        if s is None:
            return out
        strtab = self.e.get_section(s.header.sh_link)
        for k in range(s.header.sh_size // 24):
            o = s.header.sh_offset + k * 24
            nameoff, info, other, shndx, val, size = struct.unpack_from("<IBBHQQ", self.data, o)
            if not nameoff:
                continue
            raw = strtab.data()
            end = raw.find(b"\0", nameoff)
            out[raw[nameoff:end].decode("latin1")] = (val, size)
        return out

    def jumpslots(self, want):
        """r_offsets of R_AARCH64_JUMP_SLOT relocations whose symbol is `want`.

        A JUMP_SLOT (type 1026) is a call slot: the GOT word the PLT jumps
        through.  Finding it by RELOCATION rather than by value scan is what
        makes the patch deterministic -- two slots can hold the same value once
        one of them has been repointed.
        """
        dyn = self.secs.get(".dynsym")
        if dyn is None:
            return []
        strtab = self.e.get_section(dyn.header.sh_link).data()

        def symname(k):
            nameoff = struct.unpack_from("<I", self.data, dyn.header.sh_offset + k * 24)[0]
            if not nameoff:
                return ""
            end = strtab.find(b"\0", nameoff)
            return strtab[nameoff:end].decode("latin1")

        out = []
        for s in self.secs.values():
            if s.header.sh_type != "SHT_RELA" or s.header.sh_entsize != 24:
                continue
            for k in range(s.header.sh_size // 24):
                r_offset, r_info, _ = struct.unpack_from("<QQq", self.data,
                                                         s.header.sh_offset + k * 24)
                if (r_info & 0xffffffff) == 1026 and symname(r_info >> 32) == want:
                    out.append(r_offset)
        return out


def check_simplenav(root):
    path = os.path.join(root, SIMPLENAV)
    elf = Elf(path)
    syms = elf.dynsym()
    key = "_ZN16SimpleIPCService15Receive_ParsingEP8stPacket"
    if key not in syms:
        print("[!] simplenav: %s is not exported -- the parser moved" % key)
        return False
    addr, size = syms[key]
    text = elf.secs[".text"]
    off = addr - text.header.sh_addr
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    insns = list(md.disasm(text.data()[off:off + size], addr))
    seen, tech = {}, False
    for ins in insns:
        for op in ins.operands:
            if op.type == ARM64_OP_IMM and op.imm in (0x64, 0x65, 0x66):
                seen.setdefault(op.imm, ins.address)
        if "[x" in ins.op_str and ins.op_str.rstrip().endswith("#4]"):
            tech = True
    print("[i] simplenav Receive_Parsing @0x%x (%d B, %d insns)" % (addr, size, len(insns)))
    ok = True
    for evt in (0x64, 0x65, 0x66):
        if evt in seen:
            print("    event id %3d (0x%02x) compared at 0x%x" % (evt, evt, seen[evt]))
        else:
            print("[!] event id %3d (0x%02x) is NOT compared in Receive_Parsing" % (evt, evt))
            ok = False
    if tech:
        print("    technology field read at payload offset #4")
    else:
        print("[!] no [xN, #4] read found -- the technology field moved")
        ok = False
    elf.fh.close()
    return ok


def check_carplay(root):
    path = os.path.join(root, CARPLAY)
    elf = Elf(path)
    ok = True
    print("[i] carplay: %s" % CARPLAY)
    for sym in ("connect", "send", "close"):
        sl = elf.jumpslots(sym)
        if len(sl) != 1:
            print("[!] %-8s: %d JUMP_SLOT relocations (want exactly 1) -- a GOT patch "
                  "would be incomplete or ambiguous" % (sym, len(sl)))
            ok = False
            continue
        doc = EXPECTED_GOT[sym]
        note = "as documented" if sl[0] == doc else "MOVED (doc says 0x%x)" % doc
        print("    %-8s slot .got 0x%x   %s" % (sym, sl[0], note))
    elf.fh.close()
    libc = Elf(os.path.join(root, LIBC))
    ls = libc.dynsym()
    for sym in ("connect", "send", "close"):
        if sym in ls:
            print("    libc %-8s @0x%x  (what the slot must hold before patching)" % (sym, ls[sym][0]))
        else:
            print("[!] libc does not define %s" % sym)
            ok = False
    libc.fh.close()
    return ok


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_ROOT
    if not os.path.isdir(root):
        print("[!] no rootfs dump at %s" % root)
        return 1
    a = check_simplenav(root)
    b = check_carplay(root)
    if a and b:
        print("\nNAVTEXT PROOF: OK -- the event ids and the three call slots are where "
              "the gate says they are")
        return 0
    print("\nNAVTEXT PROOF: FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
