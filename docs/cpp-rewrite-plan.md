# Handwritten C++ rewrite — working plan

Status: **not started, deliberately.** This is a template to come back to.

The goal is a personal, handwritten rewrite, done to learn the project properly and
brush up C++. It is not a mechanical port job for an agent to do.

- Original C implementation (reference, works): `src/shim/mib3shim.c` — 4,460 lines, 2,863 code
- Readable C draft (first pass, passes the benches): `src/shim/instrument_cluster_shim.c` on branch `rewrite/readable-shim` — 2,903 lines, 2,316 code
- This document: the C++ plan, the learning path, and the cleanup debt in the C draft

The C draft is the better thing to port *from*, because it is already ordered by job
and named in words. Use it as the structural reference and the original as the
behavioural reference.

---

## 1. What the program does

Four phases, in this order:

1. **Advertise** — `AirPlayInfoArrayAddScreenDisplay` is called once by the head unit for
   the centre screen. Call the real one, then call it again with cluster geometry, then
   annotate the new dictionary with `type=111`, a scene address and view areas.
2. **Answer `/info`** — `AirPlayCopyServerInfo` returns the dictionary the phone reads.
   Complete it to the modern CarKit schema, or the phone rejects the whole display list.
3. **Set up the stream** — `AirPlayReceiverSessionSetup`. The head unit's dispatcher
   handles types 100/101/102/110 and drops 111, and holds exactly one screen. Own your
   own listener and thread.
4. **Receive** — accept, read a 128-byte header, read the body, decrypt, convert
   length prefixes to start codes, stamp parameter sets, forward to a separate process.

Nothing draws. The only outbound path is a socket.

---

## 2. Why C++ is worth it here

Four specific problems, not "C++ is nicer".

### 2.1 The dictionary ceremony

Today, everywhere:

```c
CFMutableDictionaryRef params = p_CFDictionaryCreateMutable(0,0,p_kCFLDictKeyCB,p_kCFLDictValCB);
CFStringRef k = sstr("widthPixels");
p_CFDictionarySetInt64(params, k, 1280);
CFRelease(k);
...
CFRelease(params);
```

Target:

```cpp
auto params = cf::dictionary::create();
params.set("widthPixels", Pixels{1280});
params.set("initialURL", url);
// released at scope exit; no call site involved
```

One `set()` template with overloads for integer, string, and CF object. No
`dseti`/`dsetv`/`dseto` triplication, no key construction at call sites.

### 2.2 The release discipline

49 `CFRelease` calls in the original. Two leaks have already shipped: one CFString per
key per advertisement, and the area plus safe-area dictionaries. A `cf::owned<T>`
template with a release policy deletes that bug class.

### 2.3 Units

`chp` meant pixels and `phy_h` meant millimetres, and readers got it wrong every time.

```cpp
struct Pixels      { int value; };
struct Millimetres { int value; };
```

No implicit conversion. Passing millimetres where pixels are expected is a compile
error.

### 2.4 The null-check walls

Around 400 conditionals, most of the form
`if (!create_dictionary || !dict_callbacks || ...) return;`. Resolve once, then let the
types make the impossible state unrepresentable. The few operations that can genuinely
fail return `std::optional`.

**What C++ does not fix:** the reverse-engineered session offsets stay
`reinterpret_cast`; `dlsym` stays; the glibc floor stays; the protocol facts stay.

---

## 3. The freestanding constraint

`tools/abi_check.sh` requires `DT_NEEDED` to be exactly `libc.so.6 libdl.so.2`, and a
glibc floor of 2.17 or lower. That forces a freestanding C++ subset.

Build flags:

```sh
-fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdlib++
```

| Allowed (zero runtime cost) | Forbidden (pulls libstdc++ or allocates) |
|---|---|
| classes, RAII, references | `std::string`, `std::vector`, `std::map` |
| templates, `constexpr` | `iostream`, `printf` family |
| `std::optional`, `std::array`, `std::span`, `std::string_view` | exceptions, RTTI |
| `enum class`, `nullptr`, `auto`, `[[nodiscard]]` | `std::function` |
| `namespace`, `using`, default member initialisers | function-local statics with dynamic init |
| `[[gnu::visibility("default")]]` | global objects with destructors |

Two supporting details:

- **Provide `operator new` / `operator delete`** — six lines forwarding to `malloc`/`free`.
  Without them the linker reaches for libstdc++.
- **`-fno-threadsafe-statics`** removes `__cxa_guard_acquire`. Without it, any
  function-local `static` with a constructor drags in a libstdc++ symbol.

Rule for globals: **POD only.** All setup happens in an explicit `initialise()` called
from the seams, never in a static constructor.

---

## 4. File layout

One file to start (`instrument_cluster_shim.cpp`), split by namespace. Splitting into
headers later is cheap once the types exist.

```
instrument_cluster_shim.cpp
├── namespace mib3::platform    libc declarations, flags, offsets, errno helpers
├── namespace mib3::cf          owned<T>, dictionary, array, string, key cache
├── namespace mib3::text        fixed-capacity builder, log
├── namespace mib3::config      typed env getters
├── namespace mib3::units       Pixels, Millimetres, density
├── namespace mib3::symbols     one constexpr table + typed resolver
├── namespace mib3::gate        USB marker
├── namespace mib3::geometry    cluster size resolution
├── namespace mib3::view_area   rectangle, classifier, presets, builder
├── namespace mib3::advert      display descriptor + /info schema
├── namespace mib3::commands    one generic sender + command table
├── namespace mib3::inbox       queue, line parser, socket thread
├── namespace mib3::transport   listener, receiver, cipher, annex-b, delivery
├── extern "C"                  three seams + three nav-text hooks
└── six mangled librsic symbols (raw __asm__ labels, NOT extern "C")
```

### The one structural change to make

The C draft has 237 lines of repeated `dictionary_put_integer(dict, "key", value)`.
Replace with a table:

```cpp
constexpr SchemaEntry cluster_display_schema[] = {
    {"type",             Kind::Integer,     111},
    {"maxFPS",           Kind::Integer,      60},
    {"widthPixels",      Kind::Pixels},
    {"heightPixels",     Kind::Pixels},
    {"widthPhysical",    Kind::Millimetres},
    {"heightPhysical",   Kind::Millimetres},
    // ...
};
```

One loop emits them; the same table validates the result, so the advert self-check
becomes free instead of a separate 115-line function.

---

## 5. Learning path

Read/refresh in this order before writing the file. Each item maps to a concrete
section of the target file, so nothing is abstract.

**C++ to brush up**

1. RAII and scope guards — a destructor that calls `CFRelease`.
2. Templates, minimal: one class template (`owned<T>`) and one variadic-free `set()`
   overload set. No metaprogramming.
3. `constexpr` and `std::array` — for the schema and symbol tables.
4. `enum class` and strong typedef structs — for opcodes, cipher kinds, units.
5. `std::optional` and `[[nodiscard]]` — replacing return-code plus out-parameter.
6. `std::span` / `std::string_view` — passing byte ranges without copying.
7. Why freestanding C++ needs `operator new` and why statics need a guard.

**Target-specific facts to internalise (from the C draft, all commented there)**

1. ELF symbol interposition: `RTLD_NEXT` vs `RTLD_DEFAULT`, and why injection means "last".
2. CoreFoundation Lite ownership: create is +1, inserting into a collection retains.
3. The CarPlay screen wire format: 128-byte header, body size, opcode, the tag being the
   last 16 bytes and not data.
4. The view-area classifier thresholds and why a wrong box means a blank cluster.
5. The firewall port chain: 5000 is the only legal free port.

**Order of study suggestion:** write `owned<T>` and a tiny `dictionary` wrapper first,
compiled into a throwaway test program on the host. If those two are pleasant to use,
the rest of the file writes itself.

---

## 6. Phases

| # | Work | Gate |
|---|---|---|
| 0 | Toolchain proof: 20-line `.so` exporting one symbol | `abi_check.sh` passes; `DT_NEEDED` exact; no `_ZSt*`, `__cxa_*`, `_Unwind*` undefined symbols |
| 1 | `platform`, `text`, `config`, `cf` | compiles with `-Wall -Wextra -Werror`; log output matches the C build on a replay |
| 2 | `units`, `symbols`, `gate`, `geometry` | geometry identical at 1x, 1.5x, 2x |
| 3 | `view_area` | the three presets produce identical rectangles to the C draft |
| 4 | `advert` + schema table | `/info` dictionary byte-identical to the C build's dump |
| 5 | `commands`, `inbox` | command dictionaries identical |
| 6 | `transport` | replay a captured stream; outgoing AU bytes identical |
| 7 | seams + mangled symbols | `test_inject_hooks.py`, `test_glue_aarch64.py`, GOT verification |

Phase 0 is the only one that can invalidate the plan. Do it first, alone, before
writing anything real.

### Build and test

```sh
# build the C draft and run the benches
sh src/shim/build_readable.sh /tmp/mib3shim.so
cp /tmp/mib3shim.so bin/mib3shim.so        # the benches read bin/mib3shim.so
python3 -m venv /tmp/shimvenv && /tmp/shimvenv/bin/pip install pyelftools capstone
/tmp/shimvenv/bin/python tests/test_inject_hooks.py
/tmp/shimvenv/bin/python tests/test_glue_aarch64.py
git checkout bin/mib3shim.so               # restore the pinned binary afterwards
```

---

## 7. New automated gate

Add to `tools/abi_check.sh`. This is what stops a future `#include <string>` from
silently adding a dependency the car cannot satisfy:

```sh
llvm-nm -u "$BIN" | grep -E '(_ZSt|__cxa_|_Unwind)' && die "libstdc++ leaked in"
```

---

## 8. Honest sizing

| | code lines |
|---|---|
| Original C | 2,863 |
| C draft (readable, all features) | 2,316 |
| C++ estimate, same features | **1,700–1,900** |

Of the C++ saving: ~120 lines are RAII/template machinery *added*, ~140 saved by the
schema table, ~200 saved by dropping release calls and null checks.

**Nothing with twenty features and a reverse-engineered protocol reaches 1,100 lines in
any language.** An earlier estimate of 1,085 was a guess presented as arithmetic. Do not
chase it.

---

## 9. Risks

- **Static initialisation.** Any global with a constructor needs `__cxa_atexit` or a
  guard. POD globals only; explicit `initialise()`.
- **The mangled librsic symbols.** `extern "C"` would break them. Keep raw
  `__asm__("...")` labels exactly as in the C version.
- **No exceptions.** Every failure is a return value. `[[nodiscard]]` everywhere so a
  discarded error is at least a warning.
- **New flag surface.** A wrong flag shows up as a link error, not a compile error.

---

## 10. Cleanup debt in the C draft

Fix these before treating `instrument_cluster_shim.c` as the reference to port from.

**Functional**

1. **The crash guard is missing.** The librsic teardown interposition is not in the C
   draft. It prevents a real reboot. Port it as its own unit (~120 lines of mangled
   wrappers, unaffected by the rewrite).
2. **Unguarded `create_dictionary` calls.** Three sites call it without checking the
   pointer resolved — a null call if symbol lookup failed: line 1269 (the stream entry
   inside `complete_display_entry`), and lines 1332 and 1355 inside
   `build_display_panels`. The other seven call sites are guarded. Either guard these
   three or assert once after `resolve_all_symbols()`.

**Structural**

3. **237 lines of repeated dictionary writes** → the schema table (section 4).
4. **Six near-identical `command_*` functions** → one sender plus a command table.
5. **The remaining null-check walls.** One `runtime_is_ready()` assertion after
   resolution would remove most of them.
6. **359 blank lines.** Bracing style is inconsistent; pick one.

**Cosmetic**

7. The file header claims "read top to bottom, entry points first". Only the
   *declarations* are first; the seam bodies are at 1150, 1230 and 2790. Either move the
   bodies or fix the comment.
8. `log_number("receiver", "authentication failed", 0, 0)` is a useless line — log the
   frame counter and the tag length instead.
9. `heightWasGiven` in `resolve_cluster_geometry` is set then read in two branches;
   simplify.

---

## 11. Decisions to make before starting

1. One file or split into headers from the start?
2. Does the C draft get cleaned up first, or is it frozen as the porting reference and
   cleaned afterwards?
3. Does the crash guard get ported to C++ or stay a separate C unit compiled into the
   same library?
4. Is `/info` self-checking part of the shipped path or a debug-only build?

---

## 12. References

- Parnas, information hiding — <https://faculty.cs.olemiss.edu/~hcc/docs/ModularDesign/ModularDesign.html>
- Modular C, one header per module — <https://www.icosaedro.it/c-modules.html>
- Characterization / golden-master testing — <https://michaelfeathers.silvrback.com/characterization-testing>
- Interposition, `RTLD_NEXT` and caching — <https://cookie.engineer/wiki/articles/reverse-engineering/ld-preload.html>
- Oracle linker guide, interposition — <https://docs.oracle.com/cd/E19683-01/816-1386/chapter3-26/index.html>
- Freestanding requirements (`memcpy`/`memset`/`memmove`/`memcmp`) — <https://gcc.gnu.org/onlinedocs/gcc/Standards.html#C-Language>
- Dependency inversion in C — <https://www.volatileint.dev/posts/dependency-inversion-c/>
- Why globals hurt — <https://embeddedartistry.com/fieldatlas/the-problems-with-global-variables/>
