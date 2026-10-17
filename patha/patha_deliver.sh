#!/bin/sh
# ============================================================================
# patha_deliver.sh -- PATH A: make the car's encoder stream OUR surface.
#
#   *** LIVE-ONLY. Runs ON THE UNIT over the live foothold. ***
#
# WHY THIS ORDER (red-team verdict 2026-10-16, LANE_PATH_A_SURFACES_MAP S7e):
#   * multi-screen-server (MS-S) streams whatever ivi surface id is written on a
#     `renderingSurfaces` element's `internalIdentifier`. It has NO name filter,
#     NO renderer input and NO process check -- it grabs the id from Weston via
#     ivi_share, cross-process. (The maprenderer/`renderingSurface` graph is the
#     NAV's input, NOT MS-S's. Do not repoint it; it does nothing for MS-S.)
#   * The id is SERVER-ASSIGNED at element create from a table compiled into
#     rsi-display-management:  navi->19, test->3000, navi-kombi-renderingsurface->33.
#     19 and 33 are held by the nav for its lifetime; 3000 is FREE.
#   * TRAP 1: a FAILED stream start is never retried (the entry compares equal to
#     itself forever). The ONLY retry is the `resync` flag.
#   * TRAP 2: elements live in rsi-display-management RAM -- if that service
#     restarts, every client element (incl. ours) is wiped. Check existence fresh.
#   * The cluster ECU re-acquires on a stream RESTART -> that is what `resync`
#     gives us (and, as a last resort, a visibleRenderers toggle).
#
# REQUIRED ORDER: the helper must already own ivi 3000 with a committed buffer
# BEFORE the element is created, or MS-S's grab fails and will never retry.
#
# USAGE (on the unit):
#   sh patha_deliver.sh discover     # read live ids + helper state (read-only)
#   sh patha_deliver.sh apply        # park MS-S, register ours, resync  <-- the test
#   sh patha_deliver.sh retry        # resync again (retry after a failed grab)
#   sh patha_deliver.sh toggle       # ECU-side session restart (visibleRenderer)
#   sh patha_deliver.sh restore      # put the car's nav back
#   sh patha_deliver.sh log          # show the MS-S log tail (the oracle)
#
# SAFETY: RSI create/update/delete only + our own binaries. No service is ever
# restarted. RAM-only; a reboot is the full undo. Never MIB3_SHIM_SINK=local.
# ============================================================================
set -u
RSI=127.0.0.1
DMP=49600          # displaymanagement
MRP=49500          # maprenderer
LOGPORT=54321      # MS-S live log stream  (the oracle)
SPNAME=navi-kombi-streamparameters
NAVNAME=navi-kombi-renderingsurface
OURNAME=test                 # -> internalIdentifier "3000" in the compiled table
OURID=3000
# NB: the kombi renderer's UUID CHANGES ON EVERY REBOOT (and on a display-service restart).
# Never hardcode it -- resolve it live from the visibleRenderers entry (see kombi_renderer_uri).
# NB: the evidence MUST land on the stick.  /dev/shm is wiped by the reset we are trying to
# diagnose (a fault in carplay/multi-display = reboot-immediate), so a RAM log leaves nothing behind.
stick_path() {
  for d in /tmp/USB/PORT1 /tmp/USB/PORT2 /tmp/USB/PORT3 /tmp/USB/PORT4; do
    [ -d "$d/PART1" ] && { printf '%s/PART1' "$d"; return 0; }
  done
  return 1
}
if STICKDIR="$(stick_path)"; then MSSLOG="$STICKDIR/mss.log"; SAVE="$STICKDIR/patha_nav.json"
else MSSLOG=/dev/shm/mss.log; SAVE=/dev/shm/patha_nav.json; fi

kombi_renderer_uri() {
  _u="$(curl -sS --max-time 8 "http://$RSI:$MRP/maprenderer/visibleRenderers" \
        | sed 's/},{/}\n{/g' | grep 'navi-kombi-visibleRenderer' \
        | sed -n 's/.*"renderer":{[^}]*"uri":"\([^"]*\)".*/\1/p' | head -n 1)"
  [ -n "$_u" ] || _u="$(curl -sS --max-time 8 "http://$RSI:$MRP/maprenderer/renderers" \
        | sed 's/},{/}\n{/g' | grep '"discreteInput":{[^}]*"name":"kombi"' \
        | sed -n 's/.*"uri":"\([^"]*\)".*/\1/p' | head -n 1)"
  printf '%s' "$_u"
}

h() { # h METHOD URL [JSON]
  _m="$1"; _u="$2"; _b="${3:-}"
  if [ -n "$_b" ]; then
    curl -sS --max-time 8 -X "$_m" -H 'Content-Type: application/json' --data "$_b" "$_u"
  else
    curl -sS --max-time 8 -X "$_m" "$_u"
  fi
}
# JSON comes back with escaped slashes; strip backslashes for use in non-JSON contexts.
de() { tr -d '\\'; }
field_of() { # field_of <json> <name> -> its uri
  printf '%s' "$1" | sed 's/},{/}\n{/g' | grep "\"name\":\"$2\"" \
    | sed -n 's/.*"uri":"\([^"]*\)".*/\1/p' | de | head -n 1
}
id_of() { # id_of <json> <name> -> its internalIdentifier
  printf '%s' "$1" | sed 's/},{/}\n{/g' | grep "\"name\":\"$2\"" \
    | sed -n 's/.*"internalIdentifier":"\([^"]*\)".*/\1/p' | head -n 1
}
uris() { curl -sS --max-time 8 "http://$RSI:$DMP/displaymanagement/renderingSurfaces"; }
disps() { curl -sS --max-time 8 "http://$RSI:$DMP/displaymanagement/displays"; }
sps()  { curl -sS --max-time 8 "http://$RSI:$DMP/displaymanagement/streamParameters"; }

oracle_on() {
  # the log is an endless stream, so a successful read "fails" with curl rc=28 (timeout) --
  # never gate on the exit code; gate on whether bytes actually land.
  for p in $(pidof curl 2>/dev/null); do
    grep -qa "54321" /proc/$p/cmdline 2>/dev/null && kill "$p" 2>/dev/null
  done
  : > "$MSSLOG" 2>/dev/null
  nohup sh -c "curl -sN --max-time 3600 http://$RSI:$LOGPORT/ > $MSSLOG 2>/dev/null" >/dev/null 2>&1 &
  sleep 2
  if [ -s "$MSSLOG" ]; then
    echo "[i] MS-S log capture live -> $MSSLOG ($(wc -c < "$MSSLOG") bytes)"
  else
    echo "[!] MS-S log capture produced nothing -- port $LOGPORT may be closed;"
    echo "    the run can still proceed (fall back to the fd count + the wire + the cluster)"
  fi
  return 0
}
oracle_tail() {
  echo "--- MS-S log (share/stream decisions) ---"
  grep -aiE "STREAMING|RESYNC|share|Surface|surfaceId|id=|framerate" "$MSSLOG" 2>/dev/null \
    | sed 's/^.*\]//' | grep -avE "avg time" | tail -n 20
  echo "--- (end) ---"
}

helper_ok() {
  P="$(pidof mib3clusterhelper 2>/dev/null)"
  [ -n "$P" ] || { echo "[!] helper NOT running -- start it FIRST:"; echo "      sh start_cluster_helper.sh bridge        # (defaults to MIB3_SHIM_IVIID=$OURID)"; return 1; }
  # The helper log carries the Cinemo bring-up; the SHIM's own log carries 'bridge: ivi_id='.
  if grep -aq "bridge_create rc=0x0" /dev/shm/mib3helper.log 2>/dev/null; then
    if grep -aq "bridge: ivi_id=$OURID" /tmp/mib3shim.log 2>/dev/null; then
      echo "[i] helper pid=$P owns ivi $OURID  (bridge_create rc=0x0; shim log confirms ivi_id=$OURID)"
    else
      echo "[i] helper pid=$P ok (bridge_create rc=0x0); ivi_id line not found in /tmp/mib3shim.log"
    fi
    return 0
  fi
  echo "[!] helper pid=$P does NOT own ivi $OURID."
  echo "    log: $(grep -aE 'bridge_create rc|already assigned' /dev/shm/mib3helper.log 2>/dev/null | tail -n 2)"
  echo "    restart it: kill $P; cd /tmp/kit && sh start_cluster_helper.sh bridge"
  return 1
}

discover() {
  R="$(uris)"; D="$(disps)"; S="$(sps)"
  echo "== elements =="
  printf '%s' "$R" | sed 's/},{/}\n{/g' | grep -o '"internalIdentifier":"[^"]*","name":"[^"]*"'
  echo "== wanted uris =="
  echo "  display            : $(field_of "$D" navi-kombi-display)"
  echo "  streamParameters   : $(field_of "$S" $SPNAME)"
  echo "  nav element        : $(field_of "$R" $NAVNAME)   (internalIdentifier $(id_of "$R" $NAVNAME))"
  echo "  our element        : $(field_of "$R" $OURNAME)   (internalIdentifier $(id_of "$R" $OURNAME))"
  echo "== helper =="
  helper_ok
  echo "== visibleRenderers =="
  curl -sS --max-time 8 "http://$RSI:$MRP/maprenderer/visibleRenderers" | sed 's/},{/}\n{/g' | grep -o '"name":"navi-[^"]*"'
}

apply() {
  echo "[0] oracle + preflight"
  oracle_on
  helper_ok || return 1
  R="$(uris)"; D="$(disps)"; S="$(sps)"
  DISP="$(field_of "$D" navi-kombi-display)"; SP="$(field_of "$S" $SPNAME)"
  NAVU="$(field_of "$R" $NAVNAME)"
  [ -n "$DISP" ] || { echo "[!] navi-kombi-display uri not found"; return 1; }
  [ -n "$SP" ]   || { echo "[!] streamParameters uri not found"; return 1; }
  printf '{"nav":"%s","sp":"%s"}\n' "$NAVU" "$SP" > "$SAVE"
  echo "[i] saved nav state -> $SAVE"
  echo "    display=$DISP"; echo "    sp=$SP"; echo "    navElement=$NAVU"

  echo "[1] park MS-S (delete the nav element -> STOP STREAMING)"
  [ -n "$NAVU" ] && { h DELETE "http://$RSI:$DMP$NAVU"; echo; }
  sleep 2

  echo "[2] register OUR element (name=$OURNAME -> internalIdentifier $OURID)"
  h POST "http://$RSI:$DMP/displaymanagement/renderingSurfaces" \
    "{\"name\":\"$OURNAME\",\"display\":\"$DISP\",\"streamParameters\":\"$SP\",\"topLeftX\":0,\"topLeftY\":0,\"width\":1280,\"height\":480}"; echo
  sleep 1
  R2="$(uris)"
  GOT="$(id_of "$R2" $OURNAME)"
  echo "[i] our element internalIdentifier = '$GOT'"
  [ "$GOT" = "$OURID" ] || { echo "[!] expected $OURID -- the element did not take; aborting"; oracle_tail; return 1; }

  echo "[3] resync (STOP -> RESYNC -> START; also the ECU's re-acquire)"
  h POST "http://$RSI:$DMP$SP" '{"resync":true}'; echo
  sleep 4
  oracle_tail
  echo "[i] want: \"START STREAMING\" + \"surfaceId: $OURID\" + \"startShare [id = $OURID]\" non-null"
  echo "[i] if the grab failed: fix the helper, then:  sh $0 retry"
  echo "[i] if the cluster still shows the car's map:  sh $0 toggle"
}

retry() {
  S="$(sps)"; SP="$(field_of "$S" $SPNAME)"
  [ -n "$SP" ] || { echo "[!] no streamParameters"; return 1; }
  echo "[*] resync again (the only retry that exists)"
  h POST "http://$RSI:$DMP$SP" '{"resync":true}'; echo
  sleep 4; oracle_tail
}

toggle() {
  echo "[*] ECU-side session restart: drop and re-add the visible kombi renderer"
  KOMBI="$(kombi_renderer_uri)"
  [ -n "$KOMBI" ] || { echo "[!] could not resolve the kombi renderer uri"; return 1; }
  echo "    renderer=$KOMBI"
  V="$(curl -sS --max-time 8 "http://$RSI:$MRP/maprenderer/visibleRenderers")"
  for u in $(printf '%s' "$V" | sed 's/},{/}\n{/g' | grep navi-kombi-visibleRenderer | sed -n 's/.*"uri":"\([^"]*\)".*/\1/p'); do
    echo "  del $u"; h DELETE "http://$RSI:$MRP$u"; echo
  done
  sleep 2
  h POST "http://$RSI:$MRP/maprenderer/visibleRenderers" \
    "{\"name\":\"navi-kombi-visibleRenderer\",\"renderer\":\"$KOMBI\"}"; echo
  sleep 3; oracle_tail
}

restore() {
  echo "[*] restore the car's nav"
  R="$(uris)"; D="$(disps)"; S="$(sps)"
  DISP="$(field_of "$D" navi-kombi-display)"; SP="$(field_of "$S" $SPNAME)"
  # 1. delete our element
  U="$(field_of "$R" $OURNAME)"
  [ -n "$U" ] && { echo "  delete ours: $U"; h DELETE "http://$RSI:$DMP$U"; echo; }
  # 2. re-create the nav element (the compiled table maps its name back to 33)
  if [ -z "$(field_of "$(uris)" $NAVNAME)" ]; then
    echo "  re-create the nav element"
    h POST "http://$RSI:$DMP/displaymanagement/renderingSurfaces" \
      "{\"name\":\"$NAVNAME\",\"display\":\"$DISP\",\"streamParameters\":\"$SP\",\"topLeftX\":0,\"topLeftY\":0,\"width\":1280,\"height\":480}"; echo
    sleep 1
  fi
  # 3. resync so MS-S picks 33 up again
  h POST "http://$RSI:$DMP$SP" '{"resync":true}'; echo
  sleep 3
  # 4. if the cluster does not come back, the ECU session lever
  echo "  if the cluster is still blank, run: sh $0 toggle"
  echo "[i] stop the helper: kill \$(pidof mib3clusterhelper)"
}

watch() {  # watch [seconds] -- follow the delivery live; the safe way to run the test
  _n="${1:-60}"; _i=0
  echo "[i] watching the chain for ${_n}s.  Plug the iPhone in and START A ROUTE IN APPLE MAPS"
  echo "    (CarPlay's nav, not the car's).  Ctrl-C to stop."
  printf "  %-6s %-9s %-9s %-10s %s\n" t helper_f dormancy fds mss_decision
  while [ "$_i" -lt "$_n" ]; do
    _f="$(grep -ao "frames total [0-9]*" /dev/shm/mib3helper.log 2>/dev/null | tail -n1 | awk '{print $3}')"
    [ -n "$_f" ] || _f="$(grep -ao "frames=[0-9x]*" /dev/shm/mib3helper.log 2>/dev/null | tail -n1)"
    _d="$(grep -acE "first WriteVideo OK|feed ok count" /tmp/mib3shim.log 2>/dev/null)"
    _s="$(ls -l /proc/$(pidof multi-screen-server)/fd 2>/dev/null | grep -cE 'video|ion|dri')"
    _m="$(grep -aE "STREAMING|surfaceId: [0-9]+|configure|add_listener" "$MSSLOG" 2>/dev/null | sed 's/^.*\[//' | tail -n1 | cut -c1-46)"
    printf "  %-6s %-9s %-9s %-10s %s\n" "${_i}s" "${_f:-?}" "${_d:-0}" "${_s:-?}" "${_m:-<none>}"
    # the moment of truth: our surface alive + the encoder configured?
    if [ "${_s:-0}" = "11" ] && [ "${_d:-0}" -gt 0 ]; then
      echo "[+] encoder armed (fds 11) with frames fed (${_d} feed log lines).  Check the cluster."
    fi
    _i=$((_i+5)); sleep 5
  done
  echo "[i] done.  Evidence on the stick: $MSSLOG , $STICKDIR/mib3shim.bc , $STICKDIR/mib3helper.bc"
}

case "${1:-}" in
  discover) discover ;;
  apply)    apply ;;
  retry)    retry ;;
  toggle)   toggle ;;
  restore)  restore ;;
  log)      oracle_tail ;;
  watch)    shift; watch "$@" ;;
  *) echo "usage: $0 {discover|apply|retry|toggle|restore|log|watch}"; exit 2 ;;
esac
