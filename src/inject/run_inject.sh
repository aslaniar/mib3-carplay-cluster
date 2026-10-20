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
# The shim that MUST be staged.  Reproduces byte-identical from mib3shim.c via
# sh ../build.sh (verified 2026-10-16).  Its deltas vs the long-proven d2cb7cd7 are
# HELPER-ONLY (fmt_2int 'x' separator, resolution/surface_width/height params, the
# window-bind + writeVideoFormat parity calls) plus log lines -- none of which run in
# carplay, because the Cinemo bridge executes only in the out-of-process helper
# (MIB3_SHIM_SINK=helper).  NEVER pin d785b25c (the batched build that caused the two
# 2026-10-16 resets).  See LANE_CLUSTER_DELIVERY_IMPLEMENT_2026-10-16.md S"pins".
WANT_MD5=9c02768a61c779c18c496c9ce05b5af0
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
