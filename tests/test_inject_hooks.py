#!/usr/bin/env python3
"""test_inject_hooks.py -- the injector's new logic, tested on the bench.

inject_shim.py cannot be run without a live carplay, but the parts added for the
nav-text gate are pure functions plus one table:

  HOOKS              slot symbol -> hook symbol, per holder
  exe_path           the executable the kernel reports for a pid
  base_of_path       load base of an exact mapped object
  lib_syms_for       dynsym of a library, resolved the way the unit sees it
  elf_jumpslots      the call slots (also exercised by navtext_proof.py)

A wrong base here would patch a random page of carplay, so the arithmetic gets
its own assertions rather than a comment.

  python3 tests/test_inject_hooks.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))   # tests/
REPO = os.path.dirname(HERE)                        # repo root
sys.path.insert(0, os.path.join(REPO, "src", "inject"))

import inject_shim as I          # noqa: E402

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
    root = os.path.join(REPO, "R8830L_ROOTFS")
    have_dump = os.path.exists(os.path.join(root, "lge/app_rw/smartlink/carplay/carplay"))
    carplay = os.path.join(root, "lge/app_rw/smartlink/carplay/carplay")
    if not have_dump:
        print("  note: no R8830L_ROOTFS beside the repo -- skipping the checks that")
        print("        read the firmware (the JUMP_SLOT and libc/pthread assertions)")
    libc = os.path.join(root, "lib64/libc.so.6")
    shim = os.path.join(REPO, "bin", "mib3shim.so")

    # ---- 1) HOOKS is the table the runbook documents -----------------------
    ok(len(I.HOOKS) == 3, "HOOKS has three entries")
    ok([h[0] for h in I.HOOKS] == ["connect", "send", "close"], "HOOKS covers connect/send/close")
    ok(all(h[1] == "libc.so.6" for h in I.HOOKS), "every hook is a libc call")
    ok(all(h[2].startswith("mib3_nt_") for h in I.HOOKS),
       "hook symbols are mib3_nt_* (never the libc names: RTLD_DEFAULT must not find them)")

    # ---- 2) every hook symbol is really exported by the built shim --------
    # elf_dynsym keeps undefined entries too (value 0), which allows both halves
    # of the assertion: the hook has an address, and the bare libc name has none
    # because it is only an undefined reference the shim links against.
    syms = I.elf_dynsym(shim)
    for slot, lib, hook, holder in I.HOOKS:
        ok(syms.get(hook, 0) != 0, "the shim DEFINES %s" % hook)
        ok(syms.get(slot, 0) == 0, "the shim does not DEFINE %s (undefined ref only)" % slot)

    if not have_dump:
        print("\n%d checks, %d failure(s)  (firmware-dependent checks skipped)" % (CHECKS, FAILS))
        return FAILS

    # ---- 3) base_of_path: exact path, offset 0, lowest address ------------
    fake_maps = [
        (0x1000, 0x2000, "r--p", "00000000", carplay),   # base
        (0x2000, 0x9000, "r-xp", "00001000", carplay),   # text
        (0x9000, 0xa000, "r--p", "00000000", carplay),   # a second off-0 mapping
        (0xb000, 0xc000, "r--p", "00000000", "/lib64/libc.so.6"),
    ]
    I.maps = lambda pid: fake_maps
    ok(I.base_of_path(1234, carplay) == 0x1000,
       "base_of_path takes the LOWEST mapping whose offset is 0")
    ok(I.base_of_path(1234, "/not/mapped") is None,
       "base_of_path returns None for an object that is not mapped")

    # ---- 3b) the SONAME is not the mapped basename (libc.so.6 ->
    #          libc-2.23.so), which is what broke the first hot-swap of this
    #          build ---------------------------------------------------------
    resolved = [
        (0x7000, 0x8000, "r-xp", "00000000", "/lib64/libc-2.23.so"),
        (0x8000, 0x9000, "r--p", "00019000", "/lib64/libc-2.23.so"),
    ]
    saved = I.maps
    I.maps = lambda pid: resolved
    lb, mp = I.lib_object(1234, "libc.so.6")
    ok(lb == 0x7000 and mp == "/lib64/libc-2.23.so",
       "lib_object finds libc through the RESOLVED basename (libc-2.23.so)")
    ok(I.lib_object(1234, "libnope.so") == (None, None),
       "lib_object still fails cleanly for a library that is not there")
    I.maps = lambda pid: resolved          # exactly what the unit reports
    ok(I.load_base(1234, "libc.so.6") is None,
       "the failure mode is real: an EXACT suffix match misses libc-2.23.so")
    I.maps = lambda pid: fake_maps + resolved

    # ---- 4) lib_syms_for resolves through the mapped path first -----------
    I.mapped_path = lambda pid, suffix: libc if suffix == "libc.so.6" else None
    s, p = I.lib_syms_for(1234, "libc.so.6")
    ok(s is not None and p == libc, "lib_syms_for uses the path /proc/maps reports")
    for fn in ("connect", "send", "close"):
        ok(fn in s, "libc dynsym has %s" % fn)

    # a library the process does not have mapped: no crash, a clean (None, None)
    I.mapped_path = lambda pid, suffix: None
    s2, p2 = I.lib_syms_for(1234, "does-not-exist.so")
    ok(s2 is None and p2 is None, "lib_syms_for fails cleanly for an unknown library")

    # ---- 4b) the slot validator: libpthread, not libc, is the real provider --
    # On this image carplay's connect/send/close slots point into
    # libpthread-2.23.so, which holds glibc's cancellable wrappers.  The
    # injector must accept that as the real thing, and must not accept a value
    # that is merely inside a mapped file.
    pthread = os.path.join(root, "lib64/libpthread-2.23.so")
    if os.path.exists(pthread):
        psyms = I.elf_dynsym(pthread)
        base = 0x7f88779000
        I.maps = lambda pid: [
            (base, base + 0x18000, "r-xp", "00000000", pthread),
            (0x7f87ac2000, 0x7f87bef000, "r-xp", "00000000", libc),
        ]
        for sym in ("connect", "send", "close"):
            ok(I.value_is_real(1234, base + psyms[sym], sym),
               "value_is_real accepts libpthread's %s (object + offset match)" % sym)
        ok(not I.value_is_real(1234, base + psyms["connect"] + 8, "connect"),
           "value_is_real rejects a value 8 bytes off inside the same object")
        ok(not I.value_is_real(1234, 0x41414141, "connect"),
           "value_is_real rejects an address outside every mapping")
        # and the libc copy must be accepted too (a future loader order could pick it)
        lsyms = I.elf_dynsym(libc)
        ok(I.value_is_real(1234, 0x7f87ac2000 + lsyms["connect"], "connect"),
           "value_is_real accepts libc's %s as well (any object that defines it)" % "connect")

    # ---- 5) the slots themselves, through the injector's own helper -------
    # There is no local copy of the addresses here: an assertion that compares a
    # table against itself would pass forever, and section 6 below is the real
    # check, where the injector and the proof script must agree on the .got
    # addresses.
    for slot in ("connect", "send", "close"):
        sl = I.elf_jumpslots(carplay, slot)
        ok(len(sl) == 1, "%s has exactly one JUMP_SLOT in the carplay executable" % slot)

    # ---- 6) the documented addresses agree with navtext_proof -------------
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import navtext_proof as N       # noqa: E402
    for slot in ("connect", "send", "close"):
        ok(I.elf_jumpslots(carplay, slot)[0] == N.EXPECTED_GOT[slot],
           "%s: the injector and the proof quote the same .got address" % slot)

    print("\n%d checks, %d failure(s)" % (CHECKS, FAILS))
    return FAILS


if __name__ == "__main__":
    sys.exit(main())
