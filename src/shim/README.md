# shim/ — the CarPlay cluster shim (MIB3 LG / R8830L)

The `.so` that makes the head unit advertise a **second (instrument-cluster) display** to the
iPhone, so iOS opens the **type-111** Apple Maps cluster stream. Injected into the live `carplay`
process; no LG binary is modified.

## What ships

- the three seams: advert / SETUP / `/info`;
- the self-contained type-111 receiver with chacha/AES decrypt;
- the AU-socket hop (`MIB3_SHIM_SINK=helper` → `bin/mib3clusterstream`, the RTP/TS sender);
- the command channel (showUI / forceKeyFrame / updateViewArea / zoom / appearance);
- the control inbox + wheel lane.

`MIB3_SHIM_SINK` is `off` | `helper` only; anything else is refused with a loud log line.

**Proven live:** the cluster stream connects on port 5000 and the receiver decrypts real H.264.

The in-process delivery machinery — a second Cinemo pipeline inside carplay, an EGL/Wayland
surface, the `dry`/`local` sink modes, and the `ScreenStreamProcessData` /
`CarPlayAppFrameworkSetSurface` interposers — is **not built**. The route-B, frame-interception and
decode-bridge sections below are kept because they document what the unit does and why the shipping
path avoids it. The delivery sink that once blacked the centre screen and reset the unit is now
fail-safe: default OFF, created on the receiver thread (never the SETUP seam), explicit ACK gate,
stick breadcrumbs, and an out-of-process helper socket.

## Files
- `mib3shim.c` — the shim (interposes the three seams + the type-111 receiver + the sink dispatcher).
- `mib3helper.h` — the shim↔helper frame protocol.
- `build.sh` — cross-builds `bin/mib3shim.so` (needs `brew llvm` + `brew lld`).

## What it hooks (verified against R8830L, `libAirPlay.so` 320.17.4)

**`AirPlayInfoArrayAddScreenDisplay`** — LG's framework calls it once for the main screen
(`CarPlayConfiguration::setDisplays` → `bl …@plt`). Real signature (recovered from the
decompiler output):
```c
int AddScreenDisplay(CFMutableArrayRef *inArray,
                     CFStringRef uuid,
                     int32 features,
                     int32 primaryInputDevice,
                     int32 maxFPS,          // 0 => key omitted
                     int32 widthPixels,
                     int32 heightPixels,
                     int32 widthPhysical,
                     int32 heightPhysical);
```
Keys set by the **real** function, in order: `uuid`, `features`, `primaryInputDevice`, `maxFPS`,
`widthPixels`, `heightPixels`, `widthPhysical`, `heightPhysical`. **It has no `type` argument and
sets no `type` key** — so a second call alone adds a *typeless* display that iOS will **not** treat
as the instrument cluster. The shim therefore calls it a second time **and then annotates the
appended dict** with `type=111`, `initialURL` and a `viewAreas[]` (+`initialViewArea`), matching the
field-level schema the phone expects. LG's own call passes `config[0x13]` as `primaryInputDevice`.

**`AirPlayReceiverSessionSetup`** — LG's handler for the iPhone's stream request. The shim logs it,
injects `enabledFeatures = [altScreen, viewAreas]` into the SETUP response (if absent), and — the important part — **handles the type-111 stream
itself**. The unit's dispatcher inside this function only handles types 100/101/102/110 and drops
111; and the session holds exactly **one** screen, so a second 110 is impossible. The shim therefore
owns a *second* screen object and reproduces the unit's `_ScreenSetup` + `_ScreenThread` for it via
the exported `AirPlayReceiverSessionScreen_*` API, opening a data port and appending
`{type:111, dataPort}` to the response, then servicing it in its own thread.

**`AirPlayCopyServerInfo`** — the `/info` builder (exported by libAirPlay; its intra-lib call goes
through the PLT/GOT slot `.got+0x3ec40`, so it interposes exactly like the two above). It returns the
mutable dictionary the unit then serialises with `CFBinaryPlistV0Create` and sends as the `/info`
response. The shim completes the **modern CarKit display schema** on it — the `AddScreenDisplay` seam
only reaches the `displays` array and cannot add a sibling top-level key, so this is the only seam
that can: per display `uid`, `properties:["Instrument Cluster"]` (cluster), a NON-empty
`initialVideoStreams`, `adjacentViewAreas`, `name`, `maxFPS`, `width/heightPixels`,
`width/heightPhysical`, and `cornerMasks`/`nightMode`/`limitedUI`/`limitedUIElements`; top-level
`displayPanels` = the same `[main,cluster]` array, and `extendedFeatures:["altScreen","viewAreas"]`.
The main display is modernised too (`uid` + `initialVideoStreams` naming its real uuid). Field shapes
verified against the iOS 26.1 CarKit decompile. `MIB3_SHIM_INFO=0` ablates it; nothing is published
unless a type-111 entry is actually present in `displays`.

## Environment knobs
`MIB3_SHIM_ADD=1` (default) advertise the cluster · `MIB3_SHIM_W/H/FPS` = geometry (default
**1280×480@60**) · `MIB3_SHIM_TYPE` = cluster display `type` (default 111) ·
`MIB3_SHIM_URL` = cluster `initialURL` **and the showUI url** (default `maps:/car/instrumentcluster/map`;
one knob, one source of truth) · `MIB3_SHIM_CFEAT` /
`MIB3_SHIM_CINDEV` = cluster `features`/`primaryInputDevice` (default `0x0A`/`0`) ·
**`MIB3_SHIM_PORT=<n>`** = preferred cluster data port; the chain is knob → **5000** (inside the
unit's own firewall allowlist `ncm0 tcp 5000:5001`, observed free) →
**6031** (MHI3-native alt; firewall-dropped without the unit's own firewall helper, which
is not part of this repository) → ephemeral ·
**`MIB3_SHIM_CMDS=0/1`** (default 1) send `forceKeyFrame`/`showUI`/`setNightMode` to the phone once the
111 stream connects (the post-connect sequence — without them the phone connects but sends no
video) · **`MIB3_SHIM_EF_IAPCHANNEL=1`** = also claim `iAPChannel` in
`enabledFeatures` (default OFF — never claim a feature the stock does not implement) ·
**`MIB3_SHIM_CONNID=1`** = echo the request's `streamConnectionID` in the 111 response descriptor
(a belt knob for paired mode; the stock 110 descriptor proves it is not required) ·
**`MIB3_SHIM_SINK`** (default `off`) = `off` | `helper` — where cluster frames go ·
**`MIB3_SHIM_HELPER`** = helper socket path (default `/dev/shm/mib3cluster.sock`) ·
**`MIB3_SHIM_BC`** = breadcrumb file (default `<stick>/PART1/mib3shim.bc`) ·
`MIB3_SHIM_SKIP_REAL=1` = test hook (skip the real SessionSetup; harness only) ·
**`MIB3_SHIM_ENABLEDFEAT=0/1`** (default 1) inject `enabledFeatures` into the **control SETUP**
response only (the request carries the `features` proposal key) ·
**`MIB3_SHIM_INFO=0/1`** (default 1) complete the modern `/info` schema (the `AirPlayCopyServerInfo`
seam) · `MIB3_SHIM_NIGHT=0/1` = debug only, force the `setNightMode` command (unset: `/info`
mirrors the main display's mode) · `MIB3_SHIM_GATE=0/1` = test hook, force the activation gate
(unset: scan for `<stick>/PART1/mib3shim.enable`). Logs to **`/tmp/mib3shim.log`**.

The view-area, zoom, appearance and telemetry knobs are documented at their use sites in
`mib3shim.c` (`VIEW AREAS`, `ZOOM`, `APPEARANCE` blocks).

## Route-B (the shim's own cluster surface)

> **⛔ DO NOT ENABLE `MIB3_SHIM_SINK=local` LIVE** (nor the legacy `MIB3_SHIM_SURFACE=1`). On the car
> the in-process sink **blacked the centre screen and reset the unit**, which is why it is no longer
> built. The environment names survive only as refusals: the shim logs and stays `off`/`helper`.
> Use `MIB3_SHIM_SINK=helper`.

**The sink dispatcher.** `MIB3_SHIM_SINK` = `off` (default) | `helper` (**out-of-process** — the safe
path). Sink creation is **deferred to the receiver thread** (`cluster_thread`), never in the SETUP
seam. **Breadcrumbs**: each risky step appends a `write()`+`fsync()` line to
`<stick>/PART1/mib3shim.bc` (`MIB3_SHIM_BC` overrides) so a reset leaves the exact failing step.

Because the CarPlay media engine is a one-window singleton (every stream lands in the *same* surface),
the cluster stream cannot get its own surface from the framework. The one shipping provider is
**`helper`**: `bin/mib3clusterstream` runs as a **separate process**, is fed frames over
`/dev/shm/mib3cluster.sock` (`mib3helper.h` protocol), and publishes them straight onto the cluster's
video stream. The shim inside carplay only `connect()`+`write()`s — **no display call**, so a fault
downstream cannot black the screen or reboot carplay.

The retired **`local`** provider was the shim's own Wayland/EGL surface (`surface`) or its own Cinemo
pipeline (`bridge`) created IN carplay: desk-verified wiring only, never for the car.

The dynamic RSI swap (`src/control/deliver_ctl.sh`) then repoints the Kombi renderer at the surface;
the surface's owning **process is irrelevant** to `multi-screen-server` (it resolves by
`internalIdentifier`).

> **Limit:** a stub bench proves the shim's *wiring* (right calls, right order, no crash), **not**
> pixels — real EGL/Wayland/OMX acceptance and the real RSI service are live-only.

## Frame interception (the in-process surface's frame source — not built)
The shim once **interposed `ScreenStreamProcessData`** (exported `FUNC GLOBAL DEFAULT` by
`libCarPlayMediaFramework.so` @ 0x1e000; imported by `libAirPlay.so`) — the one seam that carries both
the frame payload **and** the per-stream `ScreenStreamRef`. It captured its own cluster stream
(`g_cl.screen+0x328` after the cluster screen's `StartSession`) and:
- **cluster stream → diverted** into the shim's own surface (via `surface_write_frame`), *not*
  forwarded — so the singleton never rendered it and the **main screen was undisturbed**;
- **any other stream → forwarded** unchanged.

> **Honest limit:** at this seam the payload is the cluster stream's **decrypted but still compressed
> H.264** access unit (decoded pixels exist only inside Cinemo). A **decode bridge (H.264→RGBA)** is
> required before an RGBA surface path can work on a live unit.

## Decode bridge (the shim's own Cinemo pipeline — not built)
The bridge closes the H.264→pixels gap **without a decoder in the shim**: it builds a **second
Cinemo pipeline** of its own (config + playlist + playback + window + projection) and feeds the
cluster's compressed frames to that projection, so **Cinemo decodes + renders** them into the shim's
ivi surface (`ivi-app-id` = `MIB3_SHIM_IVIID`). The sequence + vtable offsets are copied verbatim
from the unit's own `CinemoMediaEngineUtils::screenSetup` (proven on the unit).

> **Verdict:** the Cinemo SDK is factory-based with **no "only one window" guard**, so a second
> pipeline is **constructible at the API level** — but the unit's own code only ever makes one and the
> decode/render is hardware, so **acceptance is live-only**. The bridge was therefore defensive: every
> step checked, any failure falling back to the surface path.

## Half-2 (cluster delivery) diagnostic (not built)
An earlier shim interposed `CarPlayAppFrameworkSetSurface(fw, surface, id)` to log which surface the
CarPlay process paints into. The shipping shim does not. Delivery of decoded frames
to the cluster is **external** (the RSI/display-management config + a cluster surface) — see
`src/control/deliver_ctl.sh`.

> Geometry (`MIB3_SHIM_W/H/FPS`) defaults to `1280×480@60` but the cluster's real pixel size is
> **not in the unit's static data** (the ViWi specs are schemas; it's runtime). Treat the default
> as unverified and set it for the actual panel.

## Correctness notes

An adversarial pass over every module (memory safety, edge cases, threading, resources, error paths,
ABI, main-screen isolation) found and fixed **two crash paths** — the chacha (HomeKit) key-derive
argument and the RGBA fallback's out-of-bounds read — plus a reconnect state-corruption bug (now a
per-thread context + generation tokens) and several robustness defects.

## Desk verification

No bench loads the shim. `tests/run_pathb.sh` drives the real sender from a fake shim
(`src/control/pathb/fake_shim.py`) and verifies the RTP/TS on the wire.
`tests/run_pathb_daemon.sh` checks the supervisor's ordering against fake sender and RSI seams.
`tests/run_clusterstream_bench.sh` pins the emitted container against the fixture. The older container harnesses loaded the unit's **real** libAirPlay/FW under the unit's own
glibc 2.23 with the shim preloaded and proved the seam is reached (direct call **and** the framework's
real `setDisplays`); the route-B stub bench proved second-surface creation, the dynamic swap
(discover→apply→restore) and the type-111→surface→frame wiring. Both were desk-only and have been
retired.

## Build
```sh
sh src/shim/build.sh     # brew install llvm lld   (once)
```
Built with **no libc dependency beyond the unit's own** (`-nostdlib`, linked against the unit's
`libc.so.6` + `libdl.so.2`) → NEEDED = `libc.so.6`, `libdl.so.2`, and only a `GLIBC_2.17` version ref
(unit is 2.23). This is how we sidestep the glibc-2.23 toolchain problem with only Apple/Homebrew
clang.

## Deploy — injection, not LD_PRELOAD

The shim is delivered by **injection into the live `carplay` process**
(`src/inject/run_inject.sh` → `src/inject/inject_shim.py`), which is what `up.sh` runs.

`LD_PRELOAD` + `systemctl restart carplay` **does not work on this unit and will reboot it**: the
stock binary starts in secure-exec mode (the `security.SMACK64EXEC` file xattr) so the loader ignores
preloads, **and** the unit resets whenever CarPlay is unhealthy (`FailureAction=reboot-immediate`), so
a restart is a guaranteed reboot.

Why repoint instead of preload: symbol interposition depends on load order, and a `dlopen`ed library
is appended LAST, so lookup still finds the real LG/Apple function. The injector finds the pointers
that already hold the real function addresses and repoints them at the shim (it patches exactly the
three GOT slots). A failed patch leaves the shim mapped-but-inert — when a seam is never reached it
forwards, never the reverse.

Nothing is persisted outside `/dev/shm`, `/tmp` and the breadcrumb file `<stick>/PART1/mib3shim.bc`,
so **a reboot plus pulling the stick is the full undo**.

## Success looks like
`/tmp/mib3shim.log` shows `AddScreenDisplay: CALL` with the real args, then
`AddScreenDisplay: ADDING cluster display`, `cluster: type=111`, `cluster/initialURL: …`,
`cluster: viewAreas added` — and the iPhone's SETUP then opens a **second stream**.

If the log stays **empty**, the seam was not reached. That is ruled out on a healthy injection: the
seam is a genuine cross-module PLT call, and `src/inject/run_inject.sh` verifies each repointed GOT
slot before it returns.
