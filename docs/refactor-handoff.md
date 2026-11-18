# Refactor handoff

Branch `rewrite/readable-shim`. Last commit `fe95b37`. The goal is to apply the
rewritten-shim style to every remaining source file, then to the shell scripts
and the Python injector.

---

## 1. Where it stands

| file | lines | state |
|---|---|---|
| `src/shim/instrument_cluster_shim.c` | 2,863 | rewritten, builds, passes benches |
| `src/shim/instrument_cluster_shim.h` | 333 | new: types, macros, prototypes, borrowed symbols |
| `src/mib3_shared.h` | 69 | new: the three cross-binary contracts |
| `src/shim/mib3shim.c` | — | **deleted** (uncommitted) |
| `src/shim/build_readable.sh` | — | **deleted** (uncommitted) |
| `src/shim/build.sh` | 65 | now builds `instrument_cluster_shim.c` |
| `src/shim/mib3navtext.c` | 534 | untouched |
| `src/wheel/mib3wheel.c` | 875 | untouched except the shared-header include |
| `src/sender/mib3clusterstream.c` | 2,675 | untouched except the shared-header include |
| `src/sender/toolkit.h` | 104 | untouched |
| `src/inject/proof.c` | 32 | untouched |

`bin/mib3shim.so` has been rebuilt from the new source:
**`f2ebad03c523d84330c7ff873378929b`**, while `up.sh:32` still pins
`c789666f…`. That mismatch is deliberate — see §6.

---

## 2. What the rewrite actually accomplished

Naming and structure, not size.

| | before | after |
|---|---|---|
| total lines | 4,460 | 2,864 |
| code lines | 2,863 | 2,238 |
| comments | 1,437 | 260 |
| functions | 177 | 92 |
| conditionals | 595 | 413 |
| binary | 144,984 B | 78,928 B |

Also: three ring-buffer implementations became one; 20 harness exports with no
callers were dropped; the librsic crash guard was removed (six exported symbols
that were **never reachable** — see §4); the frame protocol lost two of its three
duplicate definitions.

Not accomplished: it is not dramatically shorter. The features and C's verbosity
are most of the file either way. A ~1,100-line version was estimated early and
was wrong.

---

## 3. Conventions to copy

1. **One file, numbered sections, in execution order.** The three entry points
   are declared first; their bodies sit with the feature they implement.
2. **Full-word names.** No `chp`, `va_`, `hlp_`, `dseti`. Units go in the name:
   `widthInPixels` vs `widthInMillimetres`. This is the single biggest readability
   win and it costs nothing.
3. **One header per `.c`.** It holds every typedef, macro and prototype. The one
   exception: the borrowed-symbol pointers are *defined* in the header, which is
   only safe because exactly one translation unit includes it — the header says so.
4. **Freestanding rules.** The shim, the wheel and the sender are all built
   `-nostdlib` with no libc headers. Any header they include must contain no
   `#include`, no libc types, and no variable definitions other than that
   documented exception.
5. **Constants before types** in the header. `QUEUE_CAPACITY` sizes a struct
   member, so it must precede the struct.
6. **One readiness gate at the entry points**, not a seven-pointer conjunction on
   every call path. `runtime_is_ready()` exists for this.
7. **Comments explain mechanism in one to three lines.** The dated incident
   narratives belong in `docs/`, not inline. That is what took the comment count
   from 1,437 to 230.
8. **Every knob defaults to shipped behaviour**, and a knob naming a removed
   feature is refused loudly rather than silently ignored.

---

## 4. Landmines — each of these cost real time

**The crash guard was dead code, not protection.** Six mangled librsic
interposers exported to catch calls in a library loaded *before* the shim. The
shim is injected last, so those call slots were already bound. Nothing in
`src/inject/inject_shim.py` patches librsic. The symbols could not be reached.
This is the same reason the injector repoints the three seams instead of relying
on exports. Removed; the fault itself is still documented at the end of the `.c`.

**`tests/test_navtext.c` includes a `.c` file.**
`#include "../src/shim/mib3navtext.c"` — it works because there is no header. The
moment navtext is split, that line must become "include the header, compile the
`.c` alongside", and `tests/run_navtext.sh` builds it natively with
`MIB3NAVTEXT_HOST` defined.

**Moving a header breaks includes you forgot about.** When `src/shared/` moved to
`src/`, two of four consumers kept the stale path and the build failed. Grep all
consumers, not the ones you remember.

**`-Wno-unused-function` hides dead code.** It is still in `build.sh` and
`tools/build.sh`. Deleting those two flags is a free audit of every file left to
do. The shim's own dead-code removal started there.

**The binary size is a useful oracle.** After the header extraction the `.so` was
byte-identical, which proves the move was purely textual. Use the same check at
every step.

**A suspiciously small binary means a corrupted source, not a clever compiler.**
Twenty commands were spent theorising about dead-code elimination when the real
cause was a bad intermediate file. Run `git diff --stat` before interpreting an
unexpected build result.

---

## 5. Verified / not verified

**Verified on the desk:** builds against the unit's link stubs; `abi_check`
(`DT_NEEDED` exactly `libc.so.6 libdl.so.2`, glibc floor 2.17); the three seams
and three nav-text hooks exported; no bare `connect`/`send`/`close` exports;
`tests/test_inject_hooks.py` 10/10; `tests/test_glue_aarch64.py` 14/14. The sender
and wheel rebuild to the exact md5s `up.sh` pins, so the shared-header
consolidation was behaviourally a no-op.

**Not verified:** any runtime behaviour. The rewritten shim has never been
injected into a car. Specifically untested: decryption, `/info` acceptance by the
phone, view-area classification on real hardware, output pacing, and whether the
five behaviour fixes behave as intended.

---

## 6. Remaining work

| # | item | notes |
|---|---|---|
| 1 | `src/shim/mib3navtext.c` (534) | freestanding, already well-structured; mostly renaming plus a header |
| 2 | `src/wheel/mib3wheel.c` (875) | freestanding, same shape as the shim |
| 3 | `src/sender/mib3clusterstream.c` (2,675) | largest, full libc, most to gain |
| 4 | shell: `pathb_daemon.sh` (813), `deliver_ctl.sh` (1,103), `pathbctl`, `wheel_zoom.sh` | different exercise — no headers |
| 5 | Python: `inject_shim.py` (548), `inject.py` (275), `target_env.py` (181) | |
| 6 | **`up.sh` pin** | deferred by decision until the refactor is finished |

`docs/cpp-rewrite-plan.md` is a separate, later plan: a handwritten C++ port of
the shim. It contains the freestanding-C++ constraints and the measured sizing,
and is not part of this branch's work.

---

## 7. Decisions already made

- **No features are cut.** Zoom, multi-layout view areas, the nav-text gate — all
  stay. Bloat means badly written code, not features.
- **The C++ rewrite is a personal handwritten project**, not agent work.
- **`up.sh` stays on the old pin** until the whole refactor is done.
- **The crash guard is removed**, with the fault documented rather than guarded.

## 8. Open questions

- Whether the mod survives a full shutdown without the guard. The fault needs a
  graceful carplay stop with the radio still up; the unit powers off as a whole,
  so the acknowledgement likely has no live path back. Unconfirmed either way.
- Whether `mib3navtext.c` gets a private header at all, since its API already
  lives in `mib3_shared.h`.
- Whether the shell scripts are worth restructuring or only documenting.
