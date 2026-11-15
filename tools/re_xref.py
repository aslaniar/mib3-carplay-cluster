#!/usr/bin/env python3
"""re_xref.py -- find code that references a string/address in an aarch64 ELF.

Built while chasing the cluster's navigation-text path: the carplay binary is
stripped of names but not of strings, so the way in is "who loads this literal".
It resolves ADRP+ADD / ADRP+LDR pairs (the aarch64 idiom for RIP-relative data)
across the whole .text once, then answers queries.

  python3 re_xref.py <elf> <string-or-hex-addr> [--limit N]

Output: for every reference, the containing function (nearest preceding
STP x29,x30 frame setup or the nearest 'ret'-delimited boundary), the
instruction address, and a short disassembly window -- enough to pick the next
function to read.
"""
import re
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_REG, ARM64_OP_IMM, ARM64_OP_MEM
from elftools.elf.elffile import ELFFile


def load(path):
    fh = open(path, "rb")
    elf = ELFFile(fh)
    secs = {}
    for s in elf.iter_sections():
        if s.header.sh_flags & 0x4 and s.data_size:  # SHF_EXECINSTR
            secs[s.name] = (s.header.sh_addr, s.data()) 
    rodata = b""
    roaddr = 0
    for s in elf.iter_sections():
        if s.name in (".rodata", ".data.rel.ro", ".data"):
            rodata = s.data()
            roaddr = s.header.sh_addr
            break
    return fh, elf, secs, rodata, roaddr


def find_string(rodata, roaddr, needle):
    b = needle.encode() if isinstance(needle, str) else needle
    off = rodata.find(b)
    if off < 0:
        return None
    return roaddr + off


def decode_all(secs):
    """Disassemble everything; yield (addr, insn) once."""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    out = {}
    for name, (base, data) in secs.items():
        out[name] = list(md.disasm(data, base))
    return out


def resolve_xrefs(insns):
    """Return {target_addr: [(addr, reg)]} for ADRP+ADD and ADRP+?LDR cases."""
    pages = {}
    refs = {}
    last_adrp = {}       # reg -> (addr, page)
    for ins in insns:
        m = ins.mnemonic
        if m == "adrp":
            rd = ins.operands[0].reg
            page = ins.operands[1].imm
            last_adrp[rd] = (ins.address, page)
            continue
        if m in ("add", "sub", "ldr", "str", "ldrb", "ldrh", "ldrsw", "adrp") and len(ins.operands) >= 2:
            dst = ins.operands[0]
            src = ins.operands[1]
            if src.type == ARM64_OP_MEM and src.mem.base in last_adrp:
                # adrp page; ldr/str [xN, #disp]
                _, page = last_adrp[src.mem.base]
                target = page + src.mem.disp
                refs.setdefault(target, []).append((ins.address, m))
                continue
            # `add xD, xPage, #low12` -- capstone's operands are (dst, srcreg, imm),
            # so the immediate lives in operands[2]; reading operands[1] as the imm
            # silently found nothing.  (Both bugs cost a debug cycle here.)
            if len(ins.operands) == 3 and src.type == ARM64_OP_REG and \
               src.reg in last_adrp and ins.operands[2].type == ARM64_OP_IMM:
                _, page = last_adrp[src.reg]
                target = page + ins.operands[2].imm
                refs.setdefault(target, []).append((ins.address, m))
    return refs


def function_start(insns, addr):
    """Nearest preceding 'stp x29, x30' or literal-pool gap; approximate."""
    best = None
    for i, ins in enumerate(insns):
        if ins.address > addr:
            break
        if ins.mnemonic == "stp" and len(ins.operands) == 3 and \
           ins.operands[0].reg and ins.operands[1].reg:
            best = ins.address
        if ins.mnemonic == "ret":
            best = None
    return best


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, want = sys.argv[1], sys.argv[2]
    limit = 40
    if "--limit" in sys.argv:
        limit = int(sys.argv[sys.argv.index("--limit") + 1])
    fh, elf, secs, rodata, roaddr = load(path)
    if re.match(r"^(0x)?[0-9a-fA-F]+$", want) and not want.isalpha():
        target = int(want, 16) if want.startswith("0x") else int(want, 16)
    else:
        target = find_string(rodata, roaddr, want)
        if target is None:
            print("string not found: %r" % want)
            return 1
        print("string %r at 0x%x" % (want, target))
    decoded = decode_all(secs)
    hits = 0
    for name, insns in decoded.items():
        refs = resolve_xrefs(insns)
        for addr, m in refs.get(target, []):
            fs = function_start(insns, addr)
            print("\n[%s] xref at 0x%x (%s)  function~0x%s" %
                  (name, addr, m, ("%x" % fs) if fs else "?"))
            lo = max(0, addr - 0x40)
            for ins in insns:
                if lo <= ins.address <= addr + 0x20:
                    mark = ">>" if ins.address == addr else "  "
                    print("  %s 0x%x  %-8s %s" % (mark, ins.address, ins.mnemonic, ins.op_str))
            hits += 1
            if hits >= limit:
                print("\n(limit reached)")
                return 0
    print("\n%d reference(s)" % hits)
    return 0


if __name__ == "__main__":
    sys.exit(main())
