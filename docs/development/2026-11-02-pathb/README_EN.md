# Development report — 2026-11-02

## What changed

The control plane became **one resident process**. Before this, two scripts raced each
other: `pathb_ctl.sh up` parked the nav and took the cluster, and a separate guard polled
for the connection to close and switched it back. It worked, but every hand-back cost
roughly eleven seconds and the two halves could disagree about who held the port.

The replacement is `pathb_daemon.sh` with four states and a read-only CLI (`pathbctl`) that
only ever writes a marker file. There is no `up` verb any more: taking the cluster is the
daemon's decision, driven by the sender's access-unit counter, and `pathbctl` exists to
report and to ask for a release.

## What the car found

Three defects, none of which the bench could have caught:

**1. The counter froze exactly when it mattered.** The daemon reads a published state file
to decide whether the phone is still streaming. `bound` was only written while the cluster
was *unbound*, so the moment we took the port the counter stopped advancing — the daemon
read "no frames", handed back, the unbind published the jump, and it took the port again.
An endless flap with an ~8 s period, which looked exactly like a broken stream.

**2. The state file was rewritten truncate-then-write.** A reader landing in that window
saw a blank file and concluded the port was gone. One 3.5 s blink, once. Both halves are
now fixed: the file is published atomically (write, then rename), and **any "port lost"
claim is verified against the kernel before acting**. The file only reports counters.

**3. The marker did not draw.** The cluster scene activation was acknowledged at connect
(`rc=0`) and the vehicle marker still did not appear until something forced a redraw —
starting or stopping guidance did it, quitting Maps redrew both screens. Activation sent
before the screen is ready is accepted but does not fully draw. Fixed with a single
delayed re-send 1.8 s after the stream starts (`MIB3_SHIM_SHOWUI_MS`, `0` disables). It
lands before the 2.5–3 s takeover, so the first frame the ECU sees already carries the
marker.

## Wired is not wireless

Guidance start/stop pauses the phone's video for **3.1–4.1 s on wired CarPlay** (wireless
measured 1.5–2.3 s). The original 3 s grace therefore classified every scene change as a
disconnect and cost about nine seconds of the car's map each time. The grace is now
4,500 ms, and every tolerated pause of 2 s or more is logged as `tolerated a Nms silence` so
the behaviour is visible rather than inferred.

## Numbers from the car

| measurement | value |
| --- | --- |
| takeover from the main screen | 2.4 s |
| delivered frame rate | 26.6 fps (60 advertised) |
| RTP loss | 0 |
| end-to-end latency | p50 228–260 ms |
| hand-back on disconnect | ~5.3 s, automatic |
| flapping across a long session | none |
| reconcile after a supervisor restart | clean |

Bench at this state: `tests/run_pathb.sh` **53/0**, `tests/run_pathb_daemon.sh` **48/0**.

## Scene configuration

Set by the daemon at every boot, so a reboot reproduces it:

```
SINK=helper
FPS=60
VIEWAREAS=0
URL=maps:/car/instrumentcluster/map?showETA=no&maneuverLayout=topAligned&showSpeedLimit=no
```

`maneuverLayout=topAligned` is the interesting one. With `maneuverLayout=no` the phone falls
back to its own rule, and a left-hand-drive tie picks the **left** column — so Waze and
Google Maps reserve that strip the moment guidance starts and slide their map to the right.
`topAligned` forces the centred layout. Both apps were verified centred with guidance on
and off; Apple Maps is unaffected because it does not use the card layout.

The multi-area card advertisement (`VIEWAREAS=1`) is implemented and was tried: it is
buggy on this unit, so it ships off.

## Still open

- The ECU's receiver is built for the car's own 10 fps / 190 kbps. The phone sends around
  2.5 Mbps, so light artifacts remain in the label band under the mileage/trip text. Our
  own capture decodes clean, so the residual defect is downstream of the sender.
- One injection per boot. Injecting a second shim into a live `carplay` hung the phone's
  handshake and the unit rebooted; fresh boot, one injection.
