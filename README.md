# MIB3 CarPlay Cluster

Apple Maps — and Waze, and Google Maps — on the instrument cluster of a Volkswagen
with an LG **MIB3** head unit, driven by the phone's **own** H.264 stream.

<!-- a real cluster photo belongs here: docs/images/cluster.jpg -->

The cluster in these cars ("Digital Cockpit Pro", 1280×480) is not a second screen the
phone can draw on. It is a **video sink**: `multi-screen-server` encodes a surface and
ships it to the cluster ECU as RTP/MPEG-TS. Stock, that surface is the car's own nav map.

This project replaces the video that reaches the cluster with the video the iPhone is
already sending for CarPlay — **no decode, no re-encode, no pixels touched by us**. The
phone's access units are re-muxed and paced straight onto the cluster's RTP feed.

## What it does

| | |
| --- | --- |
| **Shim** | Interposes three CarPlay library seams inside the running `carplay` process, advertises a second display (type 111) for the cluster, receives that stream, decrypts it, and hands access units to a socket. |
| **Sender** | Re-muxes each access unit into MPEG-TS and paces it onto the cluster's RTP multicast, byte-proportionally inside each frame so no frame is ever emitted as a clump. |
| **Supervisor** | One resident process. When the phone starts streaming it takes the cluster's UDP port; when the session ends it hands the port back to the car. No verbs, no sleeps used as synchronisation. |

The instrument cluster then shows whatever the phone is rendering — which for the cluster
display Apple Maps renders as a full-screen map, and Waze/Google Maps centre their card
layout because the shim asks for the `topAligned` maneuver layout.

## Status

Working and field-verified on the owner's car:

- takeover from the main screen: **2.4 s**
- delivered frame rate: **26.6 fps** (60 advertised)
- RTP loss: **0**
- end-to-end latency: **p50 228–260 ms**
- hand-back to the car's nav on disconnect: automatic, **~5.3 s**
- bench: `tests/run_pathb.sh` **53/0**, `tests/run_pathb_daemon.sh` **48/0**

Known limitation, stated honestly: this ECU's receiver is built for the car's own stream —
about 10 fps at 190 kbps. The phone sends roughly 2.5 Mbps, so light compression artifacts
remain in the label band under the mileage/trip text. Everything else is clean. The full
history of how that was narrowed down is in the commit log.

## Requirements

**Root on the head unit.** The shim is loaded into a live `carplay` with `ptrace` +
`dlopen` + GOT-slot patching. That requires privilege, and there is no way around it.

This repository **does not contain, describe, or require any method of obtaining root**.
`up.sh` opens by demanding a root shell and exits if it does not have one. If you do not
already have privileged access to your own unit, this project has nothing to offer you.

Tested on: VW MIB3 (LG), train `MOI3_EU_VW_R8830L_MU0883`, part `3G8919360AR`, cluster
1280×480. Other trains are untested.

## Install

```sh
# copy this tree to the unit, then from a root shell:
sh up.sh
```

`up.sh` stages the kit into `/tmp/kit`, checks the md5 of all three pinned binaries,
injects the shim into the running CarPlay, and starts the supervisor. Re-runnable.

```sh
sh /tmp/kit/pathbctl status     # what the supervisor is doing, and why
sh /tmp/kit/pathbctl release    # hand the cluster back to the car
sh /tmp/kit/pathbctl stop       # stop the supervisor
```

Nothing is persistent: `/tmp` and `/dev/shm` are all it writes, and a reboot clears
everything.

## Layout

```
up.sh                  the one-command bring-up (root required)
src/shim/              the interposer: three seams, type-111 advert, receiver, decrypt
src/sender/            Path B sender: re-mux to MPEG-TS and pace onto the cluster feed
src/wheel/             steering-wheel zoom tap (read-only RSI watcher)
src/control/           the resident supervisor, its CLI, and the RSI handover
src/inject/            ptrace + dlopen loader for the live carplay
tools/                 analysis: PES/TS extraction, SPS dump, AU boundary checks, reports
tests/                 the benches, plus the H.264 fixture they replay
bin/                   the pinned aarch64 builds the tests and up.sh expect
docs/                  architecture and development reports
```

## Building

The unit runs aarch64 with its own glibc (≤ 2.23) and ships no CRT, so every tool has its
own `_start` and is linked `-nostdlib` against the unit's libs:

```sh
sh src/shim/build.sh     # -> bin/mib3shim.so
sh tools/build.sh        # -> bin/mib3clusterstream, bin/mib3wheel
```

Both scripts want the unit's `lib64` in `R8830L_ROOTFS/` beside this repo, which is not
distributed here. The pinned binaries in `bin/` are the builds that ran on the car, and
`up.sh` verifies them by md5 before deploying.

## Prior art and credit

The protocol shapes here were derived from working CarPlay receivers that had already
proved a cluster display is possible: **[PlayPort](https://github.com/youcci/playport)**
(Kotlin, `CarPlayClusterDisplay.kt`), **[LIVI](https://github.com/f-io/LIVI)**
(`info.rs`, `add_display_panels`), and **luka-dev/mib2q-carplay-rgi**
(`altscreen_hook.c`, the RSI/`dmdt` approach on MHI2Q). None of their code is included;
their observed field layouts and URL grammar are cited where they settled a question.

No LG, VW or Apple code is redistributed. `libAirPlay.so`, `NmeSDK` and the `carplay`
binary are proprietary and are **not** part of this repository.

## License

MIT — see `LICENSE`. The source is all first-party; the citations above are references,
not derived code.
