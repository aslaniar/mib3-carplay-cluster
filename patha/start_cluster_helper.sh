#!/bin/sh
# ============================================================================
# start_cluster_helper.sh -- start the OUT-OF-PROCESS cluster delivery helper.
#
# Run ON THE UNIT, over the live foothold, AFTER the shim is injected
# (run_inject.sh).  This is the SAFE delivery path: the helper owns the surface +
# decode in its OWN process; the shim inside carplay only forwards frames over a
# socket and makes NO display call.  A helper fault cannot black the centre
# screen or trip carplay.service's FailureAction=reboot-immediate.
#
#   sh start_cluster_helper.sh [bridge|surface|dry]
#
# bridge (default) = the helper's own Cinemo pipeline (decode+render);
# surface          = the helper's own EGL/Wayland surface (diagnostic, no decode);
# dry              = create nothing, just prove it launches.
#
# Env overrides:
#   MIB3_HELPER_LABEL   SMACK exec label for the helper (default carplay::app;
#                       set to "" to keep the caller's label).  RUNTIME-ONLY
#                       which label may reach Weston/create an ivi surface.
#   MIB3_SHIM_W/H/IVIID geometry + ivi id (must match the shim's advert).
#
# RAM-only: a reboot is the full undo.  Nothing is written to disk.
# ============================================================================
set -u
MODE="${1:-bridge}"
SHIM="${MIB3_HELPER_SHIM:-/dev/shm/mib3shim.so}"
LABEL="${MIB3_HELPER_LABEL-carplay::app}"
STAGE=/dev/shm/mib3clusterhelper
SOCK=/dev/shm/mib3cluster.sock
LOG=/dev/shm/mib3helper.log

[ -f "$SHIM" ] || { echo "[!] $SHIM missing -- run run_inject.sh first"; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=""
for c in "$HERE/mib3clusterhelper" /dev/shm/mib3clusterhelper "$HERE/../mib3clusterhelper"; do
  [ -f "$c" ] && { SRC="$c"; break; }
done
[ -n "$SRC" ] || { echo "[!] mib3clusterhelper not found -- copy it beside this script (or to /dev/shm)"; exit 1; }

# breadcrumb on the stick (survives a reset); else /dev/shm (best-effort)
BC=""
for d in /tmp/USB/PORT1 /tmp/USB/PORT2 /tmp/USB/PORT3 /tmp/USB/PORT4; do
  [ -d "$d/PART1" ] && { BC="$d/PART1/mib3helper.bc"; break; }
done
[ -n "$BC" ] || BC=/dev/shm/mib3helper.bc

# stage the helper on tmpfs (xattr-capable) and give it the SMACK exec label
cp -f "$SRC" "$STAGE" || { echo "[!] cannot stage helper"; exit 1; }
chmod 755 "$STAGE"
if [ -n "$LABEL" ]; then
  chsmack -e "$LABEL" "$STAGE" >/dev/null 2>&1
  echo "[i] helper exec label: $(chsmack "$STAGE" 2>&1)"
fi

rm -f "$LOG" "$SOCK"
# NB: do NOT pre-create $LOG as root if the helper's label is carplay::app -- a
# root-created file carries the wrong label and the helper's writes then vanish
# (THE LABEL TRAP).  Let the helper create it; the shell redirect is done by the
# helper's own label after exec, so it is created correctly.
# NB (2026-10-16, cost a whole attempt): WITHOUT XDG_RUNTIME_DIR the Cinemo/EGL bring-up fails
# and the helper spins on "error: XDG_RUNTIME_DIR not set in the environment" and dies.  The
# framework's value is /run/platform/weston.  WAYLAND_DISPLAY is wayland-0.
# NB: MIB3_SHIM_IVIID defaults to 3000 here -- that is the FREE id in the compiled surfaces
# table (navi->19, test->3000, navi-kombi-renderingsurface->33).  Path A delivers by owning 3000
# and registering an element named "test".  Override with MIB3_SHIM_IVIID=<n>.
IVIID="${MIB3_SHIM_IVIID:-3000}"
nohup env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/platform/weston}" \
      WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
      MIB3_SHIM_IVIID="$IVIID" \
      MIB3_HELPER_SHIM="$SHIM" MIB3_HELPER_SOCK="$SOCK" MIB3_HELPER_SINK="$MODE" \
      MIB3_HELPER_BC="$BC" "$STAGE" >> "$LOG" 2>&1 &
echo "[i] helper started (mode=$MODE) pid=$!"
echo "    shim:  $SHIM"
echo "    sock:  $SOCK"
echo "    log:   $LOG"
echo "    bc:    $BC"
sleep 1
echo "--- helper log (head) ---"; head -20 "$LOG" 2>/dev/null
echo
echo "[i] NOW: enable the helper sink in the shim before the next phone plug:"
echo "      /usr/bin/python target_env.py \$(pidof carplay) MIB3_SHIM_SINK=helper"
echo "      /usr/bin/python target_env.py \$(pidof carplay) MIB3_SHIM_HELPER=$SOCK"
echo "    then plug the iPhone and watch $LOG and the stick breadcrumbs."
echo "[i] stop: kill \$(pidof mib3clusterhelper)  (a reboot is the full undo)"
