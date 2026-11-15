#!/usr/bin/env python3
"""test_glue_aarch64.py -- the aarch64 GLUE, checked by disassembly.

The host bench drives mib3navtext.c natively, but the three exported wrappers in
mib3shim.c (mib3_nt_connect/send/close + navtext_arm + navtext_real) are only ever
built for the unit, so nothing exercises them on the desk.  They cannot be run
here, but their SHAPE can be asserted from the built .so, and the two failure
modes that would break carplay are shapes:

  1. the wrapper must delegate to the gate (a call/branch to another function in
     this library), not re-implement it;
  2. the wrapper must call the REAL libc function through the resolved POINTER
     (an indirect branch), never through a direct call to this library's own
     `send`/`connect`/`close` PLT slot -- a direct call would make the hook call
     libc unconditionally and bypass the gate's whole decision.

A remaining gap is stated, not hidden: the runtime behaviour of the wrappers
(arm-on-first-entry, the dlvsym fallback) is first exercised on the car, and the
log line `cluster/navtext: armed fd=` is the on-car proof that it happened.

  python3 shim/harness/test_glue_aarch64.py
"""
import os
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_REG
from elftools.elf.elffile import ELFFile

HERE = os.path.dirname(os.path.abspath(__file__))   # tests/
REPO = os.path.dirname(HERE)                        # repo root
sys.path.insert(0, os.path.join(REPO, "src", "inject"))
import inject_shim as I      # noqa: E402

FAILS = 0
CHECKS = 0


def ok(cond, what):
    global FAILS, CHECKS
    CHECKS += 1
    if cond:
        print("  ok    %s" % what)
    else:
        FAILS += 1
        print("  FAIL  %s" % what)


def main():
    so = os.path.join(REPO, "bin", "mib3shim.so")
    if not os.path.exists(so):
        print("  FAIL  bin/mib3shim.so missing (run sh src/shim/build.sh)")
        return 1
    fh = open(so, "rb")
    e = ELFFile(fh)
    data = open(so, "rb").read()
    secs = {}
    for s in e.iter_sections():
        if s.header.sh_size:
            secs[s.name] = s
    dynsym = {}
    for sec in e.iter_sections():
        if sec.name != ".dynsym":
            continue
        strtab = e.get_section(sec.header.sh_link).data()
        for k in range(sec.header.sh_size // 24):
            o = sec.header.sh_offset + k * 24
            nameoff, info, other, shndx, val, size = struct.unpack_from("<IBBHQQ", data, o)
            if nameoff:
                end = strtab.find(b"\0", nameoff)
                dynsym[strtab[nameoff:end].decode("latin1")] = (val, size)
    text = secs[".text"]
    tdata, tbase = text.data(), text.header.sh_addr

    # the PLT slot addresses of the real libc calls this library makes: a hook
    # that branches to one of these has bypassed the gate's decision
    plt_of = {}
    for slot in ("connect", "send", "close"):
        plt_of[slot] = I.elf_jumpslots(so, slot)

    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True

    def disas(name):
        addr, size = dynsym.get(name, (0, 0))
        if not addr or not size:
            return None, 0
        off = addr - tbase
        return list(md.disasm(tdata[off:off + size], addr)), addr

    for slot, lib, hook, holder in I.HOOKS:
        got = dynsym.get(hook)
        if not got or not got[0]:
            ok(0, "%s is exported" % hook)
            continue
        insns, addr = disas(hook)
        calls = [i for i in insns if i.mnemonic in ("bl", "b") and i.operands
                 and i.operands[0].type == ARM64_OP_IMM]
        indirect = [i for i in insns if i.mnemonic in ("blr", "br")
                    and i.operands and i.operands[0].type == ARM64_OP_REG]
        ok(len(calls) >= 1,
           "%s delegates to code in this library (%d direct branch(es))" % (hook, len(calls)))
        # NB the wrapper TAIL-CALLS the gate, so the real libc call is an indirect
        # branch INSIDE mib3_navtext_<slot>, not here -- that is the next loop.
        # and it must NOT fall back to this library's own PLT slot for the real call
        own_plt = []
        for plt in plt_of[slot]:
            own_plt.append(plt)
        bad = [i for i in calls if i.operands[0].imm in own_plt]
        ok(not bad,
           "%s never calls the shim's own %s PLT slot (that would bypass the gate)" % (hook, slot))

    # ---- the GATE functions: this is where the real call happens, through the
    # pointer the arm resolved (an indirect branch), never a direct one.
    for sym in ("mib3_navtext_connect", "mib3_navtext_send", "mib3_navtext_close"):
        ok(sym in dynsym and dynsym[sym][0] != 0, "%s is defined in the shim" % sym)
        insns, _ = disas(sym)
        if not insns:
            ok(0, "%s disassembles" % sym)
            continue
        ind = [i for i in insns if i.mnemonic in ("blr", "br")
               and i.operands and i.operands[0].type == ARM64_OP_REG]
        ok(len(ind) >= 1,
           "%s calls the REAL libc function through a pointer (%d indirect branch(es))"
           % (sym, len(ind)))

    for sym in ("mib3_navtext_install", "mib3_navtext_mode"):
        ok(sym in dynsym and dynsym[sym][0] != 0, "%s is defined in the shim" % sym)

    fh.close()
    print("\n%d checks, %d failure(s)" % (CHECKS, FAILS))
    return FAILS


if __name__ == "__main__":
    sys.exit(main())
