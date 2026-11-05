#!/bin/sh
# ============================================================================
# run_inject.sh -- stage the shim and INSTALL ITS SEAMS into the RUNNING carplay.
# Run ON THE UNIT, from the stick. No restart. No reboot. Re-runnable.
#
#   sh /tmp/USB/PORT2/PART1/run_inject.sh
#
# WHY: symbol interposition depends on load order, and a dlopen()'d library is
# appended LAST, so every call still lands on the real LG/Apple function. So we
# don't rely on lookup at all -- inject_shim.py finds the pointers that already
# hold the real function addresses and repoints them at our shim.
#
# CHAIN:  stage -> label -> md5-check -> dlopen (if absent) -> patch+VERIFY
# Every step aborts loudly on mismatch.  Nothing here writes to disk beyond
# /dev/shm and /tmp; a reboot is the full undo.  A failed patch leaves the shim
# mapped-but-inert (it forwards when a seam is not reached), never the reverse.
# ============================================================================
set -u
STICK="$(cd "$(dirname "$0")" && pwd)"
# The shim that MUST be staged: the field-proven Path B build (2026-11-02).
# It reproduces byte-identical from mib3shim.c with `sh shim/build.sh`.
# The fence exists because a stale shim on this unit looks healthy from every
# counter (2026-10-19: five builds injected into one carplay) -- so an md5
# mismatch aborts before anything is persisted.
WANT_MD5=dc723befb0cd4c636ca2aa2a8f2c3743
# DELIBERATE VALIDATION ESCAPE: set MIB3_SHIP_NEW_MD5=<md5> to inject a build
# that has not run on the car yet.  It must be stated, never implied.
if [ -n "${MIB3_SHIP_NEW_MD5:-}" ]; then
  WANT_MD5="$MIB3_SHIP_NEW_MD5"
  echo "[i] UN-PINNED BUILD UNDER TEST: shim md5 $WANT_MD5"
fi
echo "[i] shim md5 $WANT_MD5"
fail(){ echo "[!] $*"; echo "[!] ABORT -- nothing persisted; a reboot is the full undo"; exit 1; }

CP="$(pidof carplay)"
[ -n "$CP" ] || { echo "[!] carplay is not running"; exit 1; }
echo "[i] carplay pid $CP   state=$(systemctl is-active carplay 2>&1)"

echo "[*] staging the shim"
cp -f "$STICK/mib3shim.so" /dev/shm/mib3shim.so || fail "could not copy the shim to /dev/shm"
chmod 755 /dev/shm/mib3shim.so
chsmack -a common::lib /dev/shm/mib3shim.so >/dev/null 2>&1
echo "    label: $(chsmack /dev/shm/mib3shim.so 2>&1)"
GOT_MD5="$(md5sum /dev/shm/mib3shim.so | cut -d' ' -f1)"
echo "    md5:   $GOT_MD5   want $WANT_MD5"
[ "$GOT_MD5" = "$WANT_MD5" ] || fail "staged shim md5 mismatch"

# Do NOT pre-create /tmp/mib3shim.log as root: a root-created file is labelled
# common::bin, which carplay::app cannot write -> the shim's log stays EMPTY even
# when it works (THE LABEL TRAP, LANE_INJECTION_2026-10-15).  Let the shim create
# it (then it carries carplay::app).  The additive grant is a belt.
rm -f /tmp/mib3shim.log
echo "carplay::app common::bin rwx" > /smack/load2 2>/dev/null
echo "carplay::app _ rwx" > /smack/load2 2>/dev/null

# 1) dlopen the shim INTO the live process (only if it is not already mapped).
if grep -q mib3shim /proc/$CP/maps; then
  echo "[i] the shim is already mapped -- skipping dlopen (re-injection)"
else
  echo "[*] dlopen the shim into pid $CP"
  /usr/bin/python "$STICK/inject.py" "$CP" /dev/shm/mib3shim.so || fail "dlopen failed"
fi
[ "$(grep -c mib3shim /proc/$CP/maps)" -gt 0 ] || fail "the shim is not mapped after inject.py"

# 2) patch the call slots, and VERIFY each one reads back.
echo "[*] installing seams into pid $CP (patch + verify)"
/usr/bin/python "$STICK/inject_shim.py" "$CP" /dev/shm/mib3shim.so "$WANT_MD5" || fail "seam install failed"

echo
echo "[*] verify"
echo "    shim mapped: $(grep -c mib3shim /proc/$CP/maps)"
echo "    carplay:     $(systemctl is-active carplay 2>&1)"
echo "    cores dir:   $(ls /var/lib/nav/coredump/ | tail -1)"

# ---- ALWAYS-ON CARPLAY COMPONENTS (owner, 2026-10-28) --------------------
# The steering-wheel watcher is PART of the shim project, not a side add-on:
# every deploy of this kit brings it up, every time, on every path.  It is
# read-only RSI (no service restarted, no display call, nothing outside
# /dev/shm and the stick) and idempotent (a running watcher is left alone).
echo
echo "[*] wheel: the steering-wheel zoom watcher ships with the shim (always on)"
WZ=""
[ -f "$STICK/wheel_zoom.sh" ] && WZ="$STICK/wheel_zoom.sh"
if [ -z "$WZ" ] && [ -f /tmp/kit/wheel_zoom.sh ]; then WZ=/tmp/kit/wheel_zoom.sh; fi
if [ -n "$WZ" ]; then
  sh "$WZ" labels >/dev/null 2>&1 || echo "[!] wheel: labels step failed (zoom may be inert -- nonfatal)"
  sh "$WZ" start  >/dev/null 2>&1 || true
  if [ -n "$(pidof mib3wheel 2>/dev/null)" ]; then
    echo "    wheel: ACTIVE (pid $(pidof mib3wheel))"
  else
    echo "    wheel: FAILED -- steering-wheel zoom is DOWN; run: sh $WZ start"
  fi
else
  echo "    wheel: wheel_zoom.sh not found on this stick -- zoom lane down"
fi

echo
echo "[*] OPTIONAL live knobs (no rebuild, between phone plugs):"
echo "      /usr/bin/python $STICK/target_env.py $CP MIB3_SHIM_ADD=0"
echo "      /usr/bin/python $STICK/target_env.py $CP MIB3_SHIM_INFO=0"
echo "      /usr/bin/python $STICK/target_env.py $CP MIB3_SHIM_GATE=1"
echo
echo "[*] NEXT: plug in the iPhone, then watch:  tail -f /tmp/mib3shim.log"
echo "    want: 'init: resolved' -> 'AddScreenDisplay: ADDING cluster display'"
echo "          -> 'cluster: type=111'"
echo "          -> 'info: completed: uid/properties/initialVideoStreams + displayPanels'"
echo "          -> 'cluster: response entry {type:111,dataPort} appended'"
echo
echo "[!] AFTER A HOT-SWAP of this shim (re-injection while an AU sender is running):"
echo "    the sender still holds the PREVIOUS shim copy's AU-socket connection and will not"
echo "    accept the new copy's until that one ends -> the cluster freezes on its last frame"
echo "    with every counter healthy.  ALWAYS follow a hot-swap with:"
echo "        sh /tmp/kit/pathbctl stop && sh /tmp/kit/pathbctl start"
echo "    (the 100 ms reattach pump reconnects the new copy within ~1 s; no phone replug)."
