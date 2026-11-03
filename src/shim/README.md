# shim/ — the CarPlay cluster shim (MIB3 LG / R8830L)

> **STATUS 2026-10-29 — PATH B ONLY (redteam refactor).** The in-process Path A
> machinery is REMOVED from `mib3shim.c`: the Cinemo decode bridge, the
> EGL/Wayland surface module, the `dry`/`local` sink modes, the
> `ScreenStreamProcessData` and `CarPlayAppFrameworkSetSurface` interposers, and
> the `mib3clusterhelper` build. What ships now: the three seams (advert /
> SETUP / /info), the self-contained type-111 receiver with chacha/AES decrypt,
> the AU-socket hop (`MIB3_SHIM_SINK=helper` -> `tools/mib3clusterstream`), the
> command channel (showUI/forceKeyFrame/updateViewArea/zoom/appearance), the
> control inbox + wheel lane, and the crash guard. `MIB3_SHIM_SINK` is `off` |
> `helper` only; anything else is refused with a loud log line. The notes below
> that describe surface/bridge/local/dry describe the REMOVED code and stay as
> the historical record (sources also preserved in
> `../artifacts/PATH_A_WORKING_20261026/src/`). See
> `../REDTEAM_REFACTOR_2026-10-29.md`.

> **STATUS 2026-10-16 — PROVEN LIVE on the car.** The cluster stream **connected** on port 5000 and
> the receiver **decrypted real H.264** — see `../LANE_CLUSTER_LIVE_2026-10-16.md`.

> **STATUS 2026-10-16 (delivery-safety rev).** The delivery sink that BLACKED the screen / reset the
> unit is now fail-safe: default OFF, created on the receiver thread (never the SETUP seam), dry-run,
> per-half isolation, an explicit `MIB3_SHIM_LOCAL_ACK` gate, stick breadcrumbs, and the **SAFE
> out-of-process helper** (`mib3clusterhelper`). Artifact md5 **`68ce7033…`** (93,280 B). See
> `../LANE_DELIVERY_SAFETY_2026-10-16.md`.

> **The deploy section below is SUPERSEDED and FATAL on this unit — do NOT use it.** The shim is
> delivered by **INJECTION into the live `carplay` process** (`shim/inject/run_inject.sh`), **not**
> `LD_PRELOAD` + `systemctl restart carplay`: the stock binary starts in secure-exec mode (the
> `security.SMACK64EXEC` file xattr) so the loader ignores preloads, **and** the unit resets whenever
> CarPlay is unhealthy, so a restart is a guaranteed reboot. See
> **`MIB3_RE/LANE_LIVE_SESSION_2026-10-16.md`** and `LANE_INJECTION_2026-10-15.md`. Everything below
> on the **seams and the `MIB3_SHIM_*` knobs is current**; only the delivery/deploy text is not.

The `.so` that makes the head unit advertise a **second (instrument-cluster) display** to the
iPhone, so iOS opens the **type-111** Apple Maps cluster stream. Injected into the live `carplay`
process; no LG binary is modified.

## Files
- `mib3shim.c` — the shim (interposes three seams + the Part-B receiver + the sink dispatcher).
- `mib3clusterhelper.c` — the **out-of-process delivery helper** (the safe sink; see Route-B).
- `mib3helper.h` — the shim↔helper frame protocol.
- `build.sh` — cross-builds `mib3shim.so` (needs `brew llvm` + `brew lld`; the
  `mib3clusterhelper` build step was removed 2026-10-29 with the Path A code).
- `mib3shim.so` — the built artifact (aarch64, glibc-2.23-compatible).
- `shim_deploy.sh` / `shim_revert.sh` — optional wrappers (the raw commands are below).

## What it hooks (verified against R8830L, `libAirPlay.so` 320.17.4)

**`AirPlayInfoArrayAddScreenDisplay`** — LG's framework calls it once for the main screen
(`CarPlayConfiguration::setDisplays` → `bl …@plt`). Real signature (recovered from the
decompiled corpus):
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
PlayPort/LIVI field-level spec. LG's own call passes `config[0x13]` as `primaryInputDevice`.

**`AirPlayReceiverSessionSetup`** — LG's handler for the iPhone's stream request. The shim logs it,
injects `enabledFeatures = [altScreen, viewAreas]` into the SETUP response (if absent; `uiContext`
only with `MIB3_SHIM_EF_UICONTEXT=1`), and — the important part — **handles the type-111 stream
itself**. The unit's dispatcher inside this function only handles types 100/101/102/110 and drops
111; and the session holds exactly **one** screen, so a second 110 is impossible. The shim therefore
owns a *second* screen object and reproduces the unit's `_ScreenSetup` + `_ScreenThread` for it via
the exported `AirPlayReceiverSessionScreen_*` API, opening a data port and appending
`{type:111, dataPort}` to the response, then servicing it in its own thread. See
`LANE_TYPE111_HANDLER_2026-10-09.md`.

**`AirPlayCopyServerInfo`** — the `/info` builder (exported by libAirPlay; its intra-lib call goes
through the PLT/GOT slot `.got+0x3ec40`, so it interposes exactly like the two above). It returns the
mutable dictionary the unit then serialises with `CFBinaryPlistV0Create` and sends as the `/info`
response. The shim completes the **modern CarKit display schema** on it — the `AddScreenDisplay` seam
only reaches the `displays` array and cannot add a sibling top-level key, so this is the only seam
that can: per display `uid`, `properties:["Instrument Cluster"]` (cluster), a NON-empty
`initialVideoStreams`, `adjacentViewAreas`, `name`, `maxFPS`, `width/heightPixels`,
`width/heightPhysical`, and `cornerMasks`/`nightMode`/`limitedUI`/`limitedUIElements`; top-level
`displayPanels` = the same `[main,cluster]` array, and `extendedFeatures:["altScreen","viewAreas"]`.
The main display is modernised too (`uid` + `initialVideoStreams` naming its real uuid). Field shapes:
luka-dev/mib2q-carplay-rgi (verified against the iOS 26.1 CarKit decompile) — see
`../LANE_ADVERT_COMPLETE_2026-10-15.md`. `MIB3_SHIM_INFO=0` ablates it; nothing is published unless a
type-111 entry is actually present in `displays`.

## Environment knobs
`MIB3_SHIM_ADD=1` (default) advertise the cluster · `MIB3_SHIM_W/H/FPS` = geometry (default
**1280×480@30**; the height default is 480 per the grounded cluster mode table — see
`LANE_SURFACE_BINDING_DESK_2026-10-09.md` §3a) · `MIB3_SHIM_TYPE` = cluster display `type` (default 111) ·
`MIB3_SHIM_URL` = cluster `initialURL` **and the showUI url** (default `maps:/car/instrumentcluster/map`;
one knob, one source of truth) · `MIB3_SHIM_CFEAT` /
`MIB3_SHIM_CINDEV` = cluster `features`/`primaryInputDevice` (default `0`/`0`; cluster is display-only) ·
**`MIB3_SHIM_PORT=<n>`** = preferred cluster data port; the chain is knob → **5000** (inside the
unit's own firewall allowlist `ncm0 tcp 5000:5001`, observed free; see `REDTEAM_SHIM_2026-10-16.md`) →
**6031** (MHI3-native alt; firewall-dropped without `the unit's own firewall helper (not part of this repository)`) → ephemeral ·
**`MIB3_SHIM_CMDS=0/1`** (default 1) send `forceKeyFrame`/`showUI`/`setNightMode` to the phone once the
111 stream connects (the refs' post-connect sequence — without them the phone connects but sends no
video) · **`MIB3_SHIM_EF_IAPCHANNEL=1`** / **`MIB3_SHIM_EF_UICONTEXT=1`** = also claim that feature in
`enabledFeatures` (default OFF — the rgi's rule: never claim a feature the stock does not implement) ·
**`MIB3_SHIM_CONNID=1`** = echo the request's `streamConnectionID` in the 111 response descriptor
(rgi paired-mode belt; stock 110 proves it is not required) ·
**`MIB3_SHIM_SINK`** (default `off`) = the SINK DISPATCHER: `off` | `dry` | `helper` | `local` (see
Route-B above) · **`MIB3_SHIM_LOCAL`** = `bridge`|`surface`|`both` (default `bridge`) ·
**`MIB3_SHIM_LOCAL_ACK=1`** required to arm `local` (and to make the legacy knobs below do anything) ·
**`MIB3_SHIM_LOCAL_FALLBACK=1`** allow bridge→surface fallback ·
**`MIB3_SHIM_HELPER`** = helper socket path (default `/dev/shm/mib3cluster.sock`) ·
**`MIB3_SHIM_BC`** = breadcrumb file (default `<stick>/PART1/mib3shim.bc`) ·
**legacy: `MIB3_SHIM_SURFACE=0/1`** / **`MIB3_SHIM_BRIDGE=0/1`** (default 0) arm the in-process sink
ONLY with `MIB3_SHIM_LOCAL_ACK=1` ·
**`MIB3_SHIM_IVIID`** = ivi surface id (default `0x4d4233`) ·
**`MIB3_SHIM_SURFNAME`** = its name/`internalIdentifier` (default `mib3-cluster-surface`) ·
`MIB3_SHIM_SKIP_REAL=1` = test hook (skip the real SessionSetup; harness only) ·
**`MIB3_SHIM_ENABLEDFEAT=0/1`** (default 1) inject `enabledFeatures` into the **control SETUP**
response only (request carries the `features` proposal key — rgi-faithful scoping) ·
**`MIB3_SHIM_INFO=0/1`** (default 1) complete the modern `/info` schema (the `AirPlayCopyServerInfo`
seam) · **`MIB3_SHIM_NIGHT=0/1`** force the cluster/main `nightMode` (default: copy the main entry) ·
`MIB3_SHIM_FRAMEDBG=1` = log CarPlay output-surface changes · `MIB3_SHIM_DBG=1` verbose. Logs to
**`/tmp/mib3shim.log`**.

## Route-B (the shim's own cluster surface)

> **⛔ DO NOT ENABLE `MIB3_SHIM_SINK=local` LIVE** (nor the legacy `MIB3_SHIM_SURFACE=1`). On the car
> 2026-10-16 the in-process sink **blacked the centre screen and reset the unit**. It is now OFF by
> default, the legacy knobs are inert without `MIB3_SHIM_LOCAL_ACK=1`, and the sink is created on the
> receiver thread (never in the SETUP seam). Use `MIB3_SHIM_SINK=dry` then `=helper`.
> **`../LANE_DELIVERY_SAFETY_2026-10-16.md`**.

**The sink dispatcher (2026-10-16).** `MIB3_SHIM_SINK` = `off` (default) | `dry` (plan only, create
nothing) | `helper` (**out-of-process** — the safe path) | `local` (in-carplay, ACK-gated). The
legacy `MIB3_SHIM_SURFACE`/`MIB3_SHIM_BRIDGE=1` only arm `local` **with `MIB3_SHIM_LOCAL_ACK=1`**.
`MIB3_SHIM_LOCAL` = `bridge` | `surface` | `both` (one half at a time); bridge→surface fallback only
with `MIB3_SHIM_LOCAL_FALLBACK=1`. Sink creation is **deferred to the receiver thread**
(`cluster_thread`), never in the SETUP seam. **Breadcrumbs**: each risky step appends a
`write()`+`fsync()` line to `<stick>/PART1/mib3shim.bc` (`MIB3_SHIM_BC` overrides) so a reset leaves
the exact failing step.

Because the CarPlay media engine is a one-window singleton (every stream lands in the *same* surface —
`LANE_SURFACE_BINDING_DESK_2026-10-09.md`), the cluster stream cannot get its own surface from the
framework. Two providers:

- **`helper` (recommended, SAFE)** — `mib3clusterhelper` runs as a **separate process**, `dlopen`s this
  shim .so, owns its own surface + Cinemo decode, and is fed frames over `/dev/shm/mib3cluster.sock`
  (`mib3helper.h` protocol). The shim inside carplay only `connect()`+`write()`s — **no display call**,
  so a helper fault cannot black the screen or reboot carplay. Start it with
  `attic/path_a/start_cluster_helper.sh`. See `../LANE_DELIVERY_SAFETY_2026-10-16.md`.
- **`local` (hazardous)** — the shim's own Wayland/EGL surface (`surface`) or its own Cinemo pipeline
  (`bridge`) created IN carplay. Desk-verified wiring only
  (`LANE_ROUTE_B_STUB_BENCH_2026-10-09.md`, `attic/harness_legacy/run_routeb.sh`); **ACK-gated; not for the car**.

The dynamic RSI swap (`cluster_swap.sh`) then repoints the Kombi renderer at the surface; the surface's
owning **process is irrelevant** to `multi-screen-server` (it resolves by `internalIdentifier`).

> **Limit:** the stub bench proves the shim's *wiring* (right calls, right order, no crash), **not**
> pixels — real EGL/Wayland/OMX acceptance and the real RSI service are live-only.

## Frame interception (the cluster surface's frame source)
The shim **interposes `ScreenStreamProcessData`** (exported `FUNC GLOBAL DEFAULT` by
`libCarPlayMediaFramework.so` @ 0x1e000; imported by `libAirPlay.so`) — the one seam that carries both
the frame payload **and** the per-stream `ScreenStreamRef`. It captures its own cluster stream
(`g_cl.screen+0x328` after the cluster screen's `StartSession`) and:
- **cluster stream → diverted** into the shim's own surface (via `surface_write_frame`), *not*
  forwarded — so the singleton never renders it and the **main screen is undisturbed**;
- **any other stream → forwarded** unchanged.

> **Honest limit:** at this seam the payload is the cluster stream's **decrypted but still compressed
> H.264** access unit (decoded pixels exist only inside Cinemo). A **decode bridge (H.264→RGBA)** is
> required before the RGBA surface path on a live unit. See
> **`../LANE_FRAME_INTERCEPT_2026-10-09.md`**; desk-verified by `attic/harness_legacy/run_routeb.sh` TEST D
> (`driver_intercept.c`).

## Decode bridge (the shim's own Cinemo pipeline)
The bridge closes the H.264→pixels gap **without a decoder in the shim**: it builds a **second
Cinemo pipeline** of its own (config + playlist + playback + window + projection) and feeds the
cluster's compressed frames to that projection, so **Cinemo decodes + renders** them into the shim's
ivi surface (`ivi-app-id` = `MIB3_SHIM_IVIID`). The sequence + vtable offsets are copied verbatim
from the unit's own `CinemoMediaEngineUtils::screenSetup` (proven on the unit).

> **Verdict:** the Cinemo SDK is factory-based with **no "only one window" guard**, so a second
> pipeline is **constructible at the API level** — but the unit's own code only ever makes one and the
> decode/render is hardware, so **acceptance is live-only**. The bridge is therefore defensive: every
> step is checked, any failure falls back to the surface path. Env `MIB3_SHIM_BRIDGE=0` disables it.
> See **`../LANE_DECODE_BRIDGE_2026-10-09.md`**; desk-verified (wiring + stress: 200k frames,
> malformed input, restart, bounded resources, clean degradation) by `attic/harness_legacy/run_routeb.sh` TEST E/E2
> (`driver_bridge.c`).

## Half-2 (cluster delivery) diagnostic
The shim interposes `CarPlayAppFrameworkSetSurface(fw, surface, id)` and, with `MIB3_SHIM_FRAMEDBG=1`,
logs which surface the CarPlay process paints into (`SetSurface: ... surfaceId=`). It is a pure
forwarder when the log is off. Delivery of the decoded frames to the cluster is **external** (the
RSI/display-management config + a cluster surface) — see `LANE_CLUSTER_DELIVERY_2026-10-09.md` and
`cluster_swap.sh`.

> Geometry (`MIB3_SHIM_W/H/FPS`) defaults to `1280×480@30` but the cluster's real pixel size is
> **not in the unit's static data** (the ViWi specs are schemas; it's runtime). Treat the default
> as unverified and set it for the actual panel. See `LANE_TYPE111_HANDLER_2026-10-09.md`.

## Correctness pass (2026-10-09/10)

A full adversarial correctness pass over every module (memory safety, edge cases, threading,
resources, error paths, ABI, main-screen isolation) is in
**`../LANE_SHIM_CORRECTNESS_2026-10-09.md`**. It found and fixed **two crash paths** — the chacha
(HomeKit) key-derive argument and the RGBA fallback's out-of-bounds read — plus a reconnect
state-corruption bug (now a per-thread context + a cluster-stream registry) and several robustness
defects. New probes: `attic/harness_legacy/run_adversarial.sh` (`driver_adversarial.c`, `driver_registry.c`) and
native proofs (`native_logline_proof.c`, `native_chacha_contract.c`). All existing bench tests still
pass.

## Desk verification (container harness)

> Archived 2026-11-02: these benches now live under `attic/harness_legacy/`; only `run_pathb.sh` and `run_pathb_daemon.sh` test the live chain; see attic/README.md.

`attic/harness_legacy/run_harness.sh` loads the unit's **real** libAirPlay/FW under the unit's own glibc 2.23
with the shim preloaded and proves the seam is reached (direct call **and** the framework's real
`setDisplays`). See `LANE_SHIM_CONTAINER_2026-10-09.md` for results and gaps.

`attic/harness_legacy/run_routeb.sh` is the **route-B stub bench**: it loads the real framework with a stub
graphics stack (`libstubgfx.so`) and a stub RSI endpoint, and verifies the second-surface creation,
the dynamic swap (discover→apply→restore), and the full type-111→surface→frame wiring. See
`LANE_ROUTE_B_STUB_BENCH_2026-10-09.md`.

## Build
```sh
./build.sh     # brew install llvm lld   (once)
```
Built with **no libc dependency beyond the unit's own** (`-nostdlib`, linked against
`R8830L_ROOTFS/lib64/libc.so.6` + `libdl-2.23.so`) → NEEDED = `libc.so.6`, `libdl.so.2`, and only a
`GLIBC_2.17` version ref (unit is 2.23). This is how we sidestep the glibc-2.23 toolchain problem
with only Apple/Homebrew clang.

## Deploy (over `ssh mib3`, as root)
The override lives in **`/run`** (tmpfs) → non-persistent, and a shim crash (`FailureAction=reboot-immediate`)
reboots **once** and clears it → **no boot loop**.
```sh
# from the Mac:
scp -i ~/.ssh/mib3_rsa mib3shim.so mib3:/tmp/mib3shim.so
# on the unit:
cp /tmp/mib3shim.so /lge/app_rw/smartlink/carplay/mib3shim.so
chmod 755 /lge/app_rw/smartlink/carplay/mib3shim.so
mkdir -p /run/systemd/system/carplay.service.d
printf '[Service]\nEnvironment=LD_PRELOAD=/lge/app_rw/smartlink/carplay/mib3shim.so\n' \
  > /run/systemd/system/carplay.service.d/mib3shim.conf
: > /tmp/mib3shim.log
systemctl daemon-reload && systemctl restart carplay
tail -n 60 /tmp/mib3shim.log
```
Revert:
```sh
rm -f /run/systemd/system/carplay.service.d/mib3shim.conf /lge/app_rw/smartlink/carplay/mib3shim.so
systemctl daemon-reload && systemctl restart carplay
```

## Success looks like
`/tmp/mib3shim.log` shows `AddScreenDisplay: CALL` with the real args, then
`AddScreenDisplay: ADDING cluster display`, `cluster: type=111`, `cluster/initialURL: …`,
`cluster: viewAreas added` — and the iPhone's SETUP then opens a **second stream**.
If the log stays **empty**, LD_PRELOAD didn't interpose the framework's call — but that is now
ruled out: the seam is a genuine cross-module PLT call and the container harness catches it
(`LANE_SHIM_CONTAINER_2026-10-09.md`).

Status: **seam, advertisement, and the type-111 stream handler desk-verified in the container
(2026-10-09); not yet field-run.** The unit no longer drops the type-111 stream (it is set up and
serviced by the shim's own screen). Remaining before field-run: validate against a live phone/session
(the chacha security path is RVA-based; frame routing to the cluster lane is Part B).
