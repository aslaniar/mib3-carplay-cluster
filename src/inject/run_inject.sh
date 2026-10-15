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
# ============================================================================
set -u
STICK="$(cd "$(dirname "$0")" && pwd)"
CP="$(pidof carplay)"
[ -n "$CP" ] || { echo "[!] carplay is not running"; exit 1; }
echo "[i] carplay pid $CP   state=$(systemctl is-active carplay 2>&1)"

echo "[*] staging the shim"
cp -f "$STICK/mib3shim.so" /dev/shm/mib3shim.so
chmod 755 /dev/shm/mib3shim.so
chsmack -a common::lib /dev/shm/mib3shim.so >/dev/null 2>&1
echo "    label: $(chsmack /dev/shm/mib3shim.so 2>&1)"
echo "    md5:   $(md5sum /dev/shm/mib3shim.so | cut -d' ' -f1)   want d11d283ef0c92e536fee2ee794fff389"

echo "[*] log + SMACK grants (RAM only)"
: > /tmp/mib3shim.log
chmod 666 /tmp/mib3shim.log
echo "carplay::app common::bin rwx" > /smack/load2 2>/dev/null
echo "carplay::app _ rwx" > /smack/load2 2>/dev/null

echo "[*] installing seams into pid $CP"
/usr/bin/python "$STICK/inject_shim.py" "$CP" /dev/shm/mib3shim.so

echo
echo "[*] verify"
echo "    shim mapped: $(grep -c mib3shim /proc/$CP/maps)"
echo "    carplay:     $(systemctl is-active carplay 2>&1)"
echo "    cores dir:   $(ls /var/lib/nav/coredump/ | tail -1)"
echo
echo "[*] NEXT: plug in the iPhone, then watch:  tail -f /tmp/mib3shim.log"
echo "    want: 'init: resolved' -> 'ADDING cluster display' -> 'type=111'"
echo "          -> 'cluster: response entry {type:111,dataPort} appended'"
