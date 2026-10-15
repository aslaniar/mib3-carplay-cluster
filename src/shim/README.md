# shim/ — the CarPlay cluster shim (MIB3 LG / R8830L)

The `.so` that makes the head unit advertise a **second (instrument-cluster) display** to the
iPhone, so iOS opens the **type-111** Apple Maps cluster stream. Loaded into the `carplay` service
via `LD_PRELOAD`; no LG binary is modified.

## Files
- `mib3shim.c` — the shim (interposes two seams).
- `build.sh` — cross-builds `mib3shim.so` (needs `brew llvm` + `brew lld`).
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
injects `enabledFeatures = [altScreen, viewAreas, uiContext]` into the SETUP response (if absent),
and — the important part — **handles the type-111 stream itself**. The unit's dispatcher inside this
function only handles types 100/101/102/110 and drops 111; and the session holds exactly **one**
screen, so a second 110 is impossible. The shim therefore owns a *second* screen object and
reproduces the unit's `_ScreenSetup` + `_ScreenThread` for it via the exported
`AirPlayReceiverSessionScreen_*` API, opening a data port and appending `{type:111, dataPort}` to
the response, then servicing it in its own thread. See `LANE_TYPE111_HANDLER_2026-10-09.md`.

## Environment knobs
`MIB3_SHIM_ADD=1` (default) advertise the cluster · `MIB3_SHIM_W/H/FPS` = geometry (default
**1280×480@60**; the height default is 480 per the grounded cluster mode table — see
`LANE_SURFACE_BINDING_DESK_2026-10-09.md` §3a) · `MIB3_SHIM_TYPE` = cluster display `type` (default 111) ·
`MIB3_SHIM_URL` = cluster `initialURL` (default `maps:/car/instrumentcluster/map`) · `MIB3_SHIM_CFEAT` /
`MIB3_SHIM_CINDEV` = cluster `features`/`primaryInputDevice` (default `0`/`0`; cluster is display-only) ·
**`MIB3_SHIM_SURFACE=0/1`** (default 1) stand up the shim's own cluster surface (route B) ·
**`MIB3_SHIM_IVIID`** = ivi surface id for that surface (default `0x4d4233`) ·
**`MIB3_SHIM_SURFNAME`** = its name/`internalIdentifier` (default `mib3-cluster-surface`) ·
`MIB3_SHIM_SKIP_REAL=1` = test hook (skip the real SessionSetup; harness only) ·
`MIB3_SHIM_FRAMEDBG=1` = log CarPlay output-surface changes · `MIB3_SHIM_DBG=1` verbose. Logs to
**`/tmp/mib3shim.log`**.

## Route-B (the shim's own cluster surface)
Because the CarPlay media engine is a one-window singleton (every stream lands in the *same* surface —
`LANE_SURFACE_BINDING_DESK_2026-10-09.md`), the cluster stream cannot get its own surface from the
framework. The shim therefore **creates its own** (Wayland/ivi + EGL/GLES) and paints the cluster frames
into it; a dynamic RSI swap (`cluster_swap.sh`) then repoints the Kombi renderer at it. The shim's
`cluster_setup` stands the surface up (env-gated `MIB3_SHIM_SURFACE`) and exposes it via
`mib3shim_surface_name()`; it needs the unit's Wayland compositor (Weston/ivi-shell) + EGL/GLES, and
degrades gracefully if they are absent. Desk-verified with a **stub** graphics stack:
**`LANE_ROUTE_B_STUB_BENCH_2026-10-09.md`** (and `harness/run_routeb.sh`).

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
> **`../LANE_FRAME_INTERCEPT_2026-10-09.md`**; desk-verified by `harness/run_routeb.sh` TEST D
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
> malformed input, restart, bounded resources, clean degradation) by `harness/run_routeb.sh` TEST E/E2
> (`driver_bridge.c`).

## Half-2 (cluster delivery) diagnostic
The shim interposes `CarPlayAppFrameworkSetSurface(fw, surface, id)` and, with `MIB3_SHIM_FRAMEDBG=1`,
logs which surface the CarPlay process paints into (`SetSurface: ... surfaceId=`). It is a pure
forwarder when the log is off. Delivery of the decoded frames to the cluster is **external** (the
RSI/display-management config + a cluster surface) — see `LANE_CLUSTER_DELIVERY_2026-10-09.md` and
`cluster_swap.sh`.

> Geometry (`MIB3_SHIM_W/H/FPS`) defaults to `1280×720@60` but the cluster's real pixel size is
> **not in the unit's static data** (the ViWi specs are schemas; it's runtime). Treat the default
> as unverified and set it for the actual panel. See `LANE_TYPE111_HANDLER_2026-10-09.md`.

## Correctness pass (2026-10-09/10)

A full adversarial correctness pass over every module (memory safety, edge cases, threading,
resources, error paths, ABI, main-screen isolation) is in
**`../LANE_SHIM_CORRECTNESS_2026-10-09.md`**. It found and fixed **two crash paths** — the chacha
(HomeKit) key-derive argument and the RGBA fallback's out-of-bounds read — plus a reconnect
state-corruption bug (now a per-thread context + a cluster-stream registry) and several robustness
defects. New probes: `harness/run_adversarial.sh` (`driver_adversarial.c`, `driver_registry.c`) and
native proofs (`native_logline_proof.c`, `native_chacha_contract.c`). All existing bench tests still
pass.

## Desk verification (container harness)
`harness/run_harness.sh` loads the unit's **real** libAirPlay/FW under the unit's own glibc 2.23
with the shim preloaded and proves the seam is reached (direct call **and** the framework's real
`setDisplays`). See `LANE_SHIM_CONTAINER_2026-10-09.md` for results and gaps.

`harness/run_routeb.sh` is the **route-B stub bench**: it loads the real framework with a stub
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
