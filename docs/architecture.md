# Architecture

## The problem

A VW cluster ("Digital Cockpit Pro") is not a display the phone can target. It is a video
sink. On an MIB3 unit, `multi-screen-server` (MS-S) captures an ivi surface, encodes it to
H.264, muxes it to MPEG-TS and transmits RTP/multicast to the cluster ECU at roughly
10 fps and 190 kbps. Stock, that surface belongs to the car's own navigation engine.

So there are exactly two ways to put CarPlay on it:

- **regenerate the pixels** — decode the phone's H.264, present it on a surface MS-S will
  take, accept a second encode. Every frame is decoded and re-encoded on the unit's CPU,
  and the receiver gets whatever our encoder produced.
- **replace the video** — take the access units the phone already encoded and put *those*
  on the cluster's RTP feed. No decode, no encode, no second generation of compression.

This project is the second one. It is why the picture is as good as the phone's own stream
allows, and it is why the CPU cost is close to zero.

## The three parts

### 1. The shim (`src/shim/mib3shim.c`)

Loaded into the already-running `carplay` process. It interposes three library functions
via GOT-slot patching (the shim is loaded last, so symbol interposition by load order does
not apply — the call sites are rewritten instead):

| seam | what it buys |
| --- | --- |
| `AirPlayInfoArrayAddScreenDisplay` | advertises the second display to iOS: a type-111 cluster entry, 1280×480, with the ISO `viewAreas` grammar |
| `AirPlayReceiverSessionSetup` | adds `[viewAreas, altScreen]` to the `enabledFeatures` the receiver answers, so iOS will compose a second screen at all |
| `AirPlayCopyServerInfo` | completes the modern CarKit `/info` schema (`uid`, `initialVideoStreams`, `displayPanels`, `extendedFeatures`) on the cluster entry and mirrors the main display's current `nightMode` onto it |

With those in place iOS opens a type-111 video stream to the unit. The shim owns the
listener, receives the stream, decrypts each frame with the unit's own primitives
(ChaCha20-Poly1305 or AES-CTR, mirroring stock `_ScreenSetup`), rebuilds Annex-B access
units and writes them to a Unix socket — `MIB3_SHIM_SINK=helper`. There is no other sink
mode: anything else is refused with a loud log line, because "off" silently decrypts and
discards every frame while every counter still looks healthy.

The shim also owns the **scene URL** the cluster renders and the **command channel** back
to the phone (`showUI`, `forceKeyFrame`, `updateViewArea`, `setNightMode`), which is what
makes the map appear at all and stay centred during guidance.

### 2. The sender (`src/sender/mib3clusterstream.c`)

Two jobs, and the second one is the whole trick.

**Framing.** Each access unit becomes one PES, and the parameter sets are folded into the
frame that needs them rather than travelling as their own record. Every stream this ECU
has ever decoded carries SPS/PPS only on the IDR; a stream that repeats them per frame is
accepted for about a second and then dropped.

**Pacing.** The sender must transmit at the rate the receiver expects, spread *inside* the
frame, not at the rate the socket accepts. Emitting a frame as fast as it will go puts a
192 KB keyframe on the wire as 169 datagrams inside one 25 ms window, against the car's
own peak of 61 — and that clump *is* the visual artifact. Deadlines are byte-proportional
and driven by `ppoll`, with a ceiling that makes a catch-up burst impossible. A frame may
take two or three frame-times to leave; nothing else is allowed to happen until it has.
Every access unit also ends on a datagram boundary, because a partial AU sitting in the
buffer during the phone's next pause is indistinguishable from a stall.

### 3. The supervisor (`src/control/pathb_daemon.sh` + `pathbctl`)

One resident process with four states and no verbs:

```
IDLE ──phone streaming──▶ TAKEOVER ──▶ OWNED
 ▲                                     │
 └──────────── RELEASE ◀───────────────┘
```

It reads one thing — the sender's published state file — and decides. It does not parse
logs, and sleeps are never used as synchronisation.

Two hard-won details are load-bearing:

- **A socket is not a session.** Wireless CarPlay keeps the phone's TCP connection
  ESTABLISHED for hours after it has left, so "connected" outlives the session. The only
  honest signal is the sender's access-unit counter. The sender therefore runs long-lived
  (`--monitor`), owns the AU socket for the whole boot, and binds the cluster's UDP port
  only on command — the supervisor never spawns or kills it per session.
- **The kernel decides ownership; the file only reports counters.** Any "port lost" claim
  is verified against the kernel before acting, and the state file is published by
  write-then-rename so a reader can never land inside a truncated write.

The grace period is 4,500 ms because **wired** CarPlay pauses 3.1–4.1 s at guidance
start/stop (wireless: 1.5–2.3 s). A shorter grace turns every scene change into a false
disconnect and hands the cluster back in the middle of a route.

## Why the ECU accepts our stream

Two requirements, both discovered the hard way and both non-obvious:

1. **The RTP source port must be the one MS-S uses.** The receiver drops video from any
   other source port, even with a correct payload. The supervisor reads that port from
   `/proc/<ms-s-pid>/fd` and `/proc/net/udp6` at runtime; the sender binds it.
2. **Timing must be real-time.** A burst knocks the ECU into its "Starting map display…"
   placeholder, from which it does not recover on its own.

With those satisfied the ECU cannot tell our feed from the car's own — and it never needs
to know.

## What is deliberately not done

- **Keyframe-chain retention on takeover.** Saving the ~0.4 s it would buy is not worth up
  to a keyframe period of staleness. 2.4 s is fast enough.
- **A faster disconnect hand-back.** The 3 s grace covers the phone's own pauses; the
  hand-back completes while the driver is still walking away.
- **Decoding anything.** The moment we decode, we are re-encoding, and the artifact
  argument comes back.
