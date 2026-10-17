#!/bin/sh
# ============================================================================
# deliver_ctl.sh -- the ONE deterministic, idempotent, reversible controller for
#                   Path-A cluster delivery.  Runs ON THE UNIT only.
#
#   *** LIVE-ONLY. RAM-only. It NEVER touches a service (carplay / multi-display /
#       rsi-display-management / displaymgr); it only POSTs/DELETEs RSI elements
#       and starts/stops OUR OWN processes.  A reboot is the full undo. ***
#
# WHAT IT DOES (mechanism -- do not re-derive; LANE_PATH_A_SURFACES_MAP_2026-10-16 S7e):
#   multi-screen-server (MS-S) streams whatever ivi surface id is written on a
#   `displaymanagement.renderingSurfaces` element's `internalIdentifier`.  That id is
#   server-assigned at CREATE from a table COMPILED INTO rsi-display-management:
#       navi -> 19   test -> 3000   navi-kombi-renderingsurface -> 33
#   19 and 33 are held by the nav engine for its lifetime; 3000 is FREE.  So we own
#   ivi 3000 (with our presenter) and register an element named "test".
#   THREATS HANDLED: rsi-display-management holds elements in RAM (a restart wipes
#   ours -- we re-check, never assume); a FAILED stream start is NEVER retried, so the
#   only retry is `{"resync":true}`; the ECU re-acquires on a stream restart = resync.
#
# ORDER IS LOAD-BEARING: the presenter must own ivi 3000 with a committed buffer
# BEFORE the "test" element is created, or MS-S's grab fails and will not retry.
#
# USAGE (on the unit):
#   sh deliver_ctl.sh status            # read-only: ids, helper, presenter, MS-S fds
#   sh deliver_ctl.sh apply             # park nav -> register ours -> resync   (idempotent)
#   sh deliver_ctl.sh verify            # assert the post-conditions, PASS/FAIL
#   sh deliver_ctl.sh restore           # put the car's nav back               (idempotent)
#   sh deliver_ctl.sh watch [secs]      # live table while you start a route
#
# SAFETY: on every mutating step a one-line breadcrumb (write + sync) lands on the
#   STICK at <stick>/PART1/deliver.bc -- a fault here can reset the unit and wipe
#   /dev/shm, so the stick is the only surviving record.  (We do NOT touch the shim's
#   own mib3shim.bc -- a root-created file carries the wrong SMACK label and would
#   silently break the shim's logging: THE LABEL TRAP.)
# ============================================================================
set -u
# All of these are overridable so the controller can be exercised against a MOCK RSI
# on the desk (harness/run_delivery.sh).  On the unit the defaults are the live values.
RSI="${MIB3_DLV_RSI:-127.0.0.1}"
DMP="${MIB3_DLV_DMP:-49600}"      # displaymanagement
MRP="${MIB3_DLV_MRP:-49500}"      # maprenderer
LOGPORT="${MIB3_DLV_LOGPORT:-54321}"  # MS-S live log (the oracle)
SPNAME=navi-kombi-streamparameters
NAVNAME=navi-kombi-renderingsurface
OURNAME=test
OURID=3000

stick_path() {
  [ -n "${MIB3_DLV_STICK:-}" ] && { printf '%s' "$MIB3_DLV_STICK"; return 0; }
  for d in /tmp/USB/PORT1 /tmp/USB/PORT2 /tmp/USB/PORT3 /tmp/USB/PORT4; do
    [ -d "$d/PART1" ] && { printf '%s/PART1' "$d"; return 0; }
  done
  return 1
}
if STICK="$(stick_path)"; then
  mkdir -p "$STICK" 2>/dev/null
  BCF="$STICK/deliver.bc"; MSSLOG="$STICK/mss.log"; SAVE="$STICK/deliver_nav.json"; LOCK="$STICK/deliver.lock"
else
  BCF="${MIB3_DLV_RAM:-/dev/shm}/deliver.bc"; MSSLOG="${MIB3_DLV_RAM:-/dev/shm}/mss.log"; SAVE="${MIB3_DLV_RAM:-/dev/shm}/deliver_nav.json"; LOCK="${MIB3_DLV_RAM:-/dev/shm}/deliver.lock"
fi

bc() { printf '[%s] %s\n' "$(date -u +%Y%m%dT%H%M%SZ)" "$*" >> "$BCF" 2>/dev/null; sync 2>/dev/null; \
       printf '[bc] %s\n' "$*"; }
say() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }

# --- lockfile (busybox has no flock; mkdir is atomic) --------------------------
acquire() {
  if mkdir "$LOCK" 2>/dev/null; then echo "$$" > "$LOCK/pid"; trap 'release' EXIT INT TERM; return 0; fi
  _p="$(cat "$LOCK/pid" 2>/dev/null)"
  if [ -n "$_p" ] && kill -0 "$_p" 2>/dev/null; then
    say "ABORT: another deliver_ctl is running (pid $_p). Refusing to double-apply."
    return 1
  fi
  say "stale lock (pid '${_p:-?}' gone) -- reclaiming"
  rm -rf "$LOCK"; mkdir "$LOCK" 2>/dev/null && { echo "$$" > "$LOCK/pid"; trap 'release' EXIT INT TERM; return 0; }
  say "ABORT: cannot take the lock"; return 1
}
release() { [ -n "${LOCK:-}" ] && [ -f "$LOCK/pid" ] && [ "$(cat "$LOCK/pid" 2>/dev/null)" = "$$" ] && rm -rf "$LOCK" 2>/dev/null; }

# --- tiny RSI helpers ----------------------------------------------------------
have() { command -v "$1" >/dev/null 2>&1; }
pids_of() { if have pidof; then pidof "$1" 2>/dev/null; elif have pgrep; then pgrep -x "$1" 2>/dev/null; fi; }
h() { _m="$1"; _u="$2"; _b="${3:-}"
  if [ -n "$_b" ]; then curl -sS --max-time 8 -X "$_m" -H 'Content-Type: application/json' --data "$_b" "$_u"
  else curl -sS --max-time 8 -X "$_m" "$_u"; fi; }
de() { tr -d '\\'; }
line_of() { printf '%s' "$1" | sed 's/},{/}\
{/g' | grep "\"name\":\"$2\"" | head -n1; }
uri_of()  { line_of "$1" "$2" | sed -n 's/.*"uri":"\([^"]*\)".*/\1/p' | de | head -n1; }
id_of()   { line_of "$1" "$2" | sed -n 's/.*"internalIdentifier":"\([^"]*\)".*/\1/p' | head -n1; }
uris() { curl -sS --max-time 8 "http://$RSI:$DMP/displaymanagement/renderingSurfaces"; }
disps(){ curl -sS --max-time 8 "http://$RSI:$DMP/displaymanagement/displays"; }
sps()  { curl -sS --max-time 8 "http://$RSI:$DMP/displaymanagement/streamParameters"; }
valid_uri() { case "$1" in /displaymanagement/renderingSurfaces/*|/displaymanagement/streamParameters/*|/displaymanagement/displays/*) return 0;; *) return 1;; esac; }

oracle_on() {
  for p in $(pids_of curl); do grep -qa "54321" /proc/$p/cmdline 2>/dev/null && kill "$p" 2>/dev/null; done
  : > "$MSSLOG" 2>/dev/null
  nohup sh -c "curl -sN --max-time 3600 http://$RSI:$LOGPORT/ > $MSSLOG 2>/dev/null" >/dev/null 2>&1 &
  sleep 2
  [ -s "$MSSLOG" ] && say "oracle live -> $MSSLOG" || say "WARN: oracle produced nothing (port $LOGPORT closed?)"
  return 0
}
oracle_tail() {
  echo "--- MS-S log (share/stream decisions) ---"
  grep -aiE "STREAMING|RESYNC|startShare|stopShare|configure|input_caps|surface_state|surfaceId|does not exist|framerate" "$MSSLOG" 2>/dev/null \
    | sed 's/^.*\]//' | grep -avE "avg time" | tail -n 24
  echo "--- (end) ---"
}
msf_fds() { ls -l /proc/"$(pids_of multi-screen-server)"/fd 2>/dev/null | grep -cE 'video|ion|dri'; }

presenter_ok() {
  P="$(pids_of mib3egl2)"; [ -n "$P" ] || P="$(pids_of mib3egl)"
  [ -n "$P" ] || P="$(pids_of mib3surface)"
  [ -n "$P" ] || P="$(pids_of mib3clusterhelper)"
  if [ -n "$P" ]; then say "presenter/helper pid=$P"; return 0; fi
  say "WARN: no presenter (mib3egl/mib3egl2/mib3surface/mib3clusterhelper) is running."
  say "      Start the PERSISTENT presenter FIRST, e.g.:"
  say "        XDG_RUNTIME_DIR=/run/platform/weston WAYLAND_DISPLAY=wayland-0 \\"
  say "          nohup /tmp/kit/mib3egl $OURID 1280 480 --persist >/dev/null 2>&1 &"
  say "      (mib3egl owns ivi $OURID and never exits; for a bounded diagnostic run use"
  say "       mib3egl2 $OURID 1280 480 3600 00ff00 --fps 10 instead.)"
  return 1
}
surface_has_buffer() {   # read-only check: does ivi 3000 carry a buffer?
  if command -v LayerManagerControl >/dev/null 2>&1; then
    XDG_RUNTIME_DIR=/run/platform/weston WAYLAND_DISPLAY=wayland-0 \
      LayerManagerControl get surface "$OURID" 2>/dev/null | grep -E "original size|source region"
  fi
}

status() {
  R="$(uris)"; D="$(disps)"; S="$(sps)"
  echo "== stick / lock =="; echo "  stick    : ${STICK:-<none - RAM fallback>}"; echo "  lock     : $LOCK $([ -d "$LOCK" ] && echo '(HELD pid'"$(cat "$LOCK/pid" 2>/dev/null)"')')"
  echo "== elements =="; printf '%s' "$R" | sed 's/},{/}\
{/g' | grep -o '"internalIdentifier":"[^"]*","name":"[^"]*"'
  echo "== wanted uris =="
  echo "  display          : $(uri_of "$D" navi-kombi-display)"
  echo "  streamParameters : $(uri_of "$S" $SPNAME)"
  echo "  nav element      : $(uri_of "$R" $NAVNAME)  (internalIdentifier $(id_of "$R" $NAVNAME))"
  echo "  our element      : $(uri_of "$R" $OURNAME)  (internalIdentifier $(id_of "$R" $OURNAME))"
  echo "== presenter / helper =="; presenter_ok || true
  echo "== our ivi surface buffer (want original size 1280x480) =="; surface_has_buffer || echo "  (LayerManagerControl unavailable -- check the ILM scene another way)"
  echo "== MS-S encoder fds (want 11 when streaming, 6 parked) =="; echo "  fds=${_fds:=$(msf_fds)}"
  echo "== last MS-S decision =="; grep -aE "STREAMING|surfaceId: [0-9]+|startShare \[id" "$MSSLOG" 2>/dev/null | sed 's/^.*\[//' | tail -n2
}

do_apply() {
  bc "apply:start (helper/presenter, stick=${STICK:-RAM})"
  oracle_on
  presenter_ok || { bc "apply:abort:no-presenter"; return 1; }
  R="$(uris)"; D="$(disps)"; S="$(sps)"
  DISP="$(uri_of "$D" navi-kombi-display)"
  SP="$(uri_of "$S" $SPNAME)"
  NAVU="$(uri_of "$R" $NAVNAME)"
  # validate BEFORE any POST (the old race produced {"code":400,"Invalid JSON"})
  [ -n "$DISP" ] || { bc "apply:abort:no-display-uri"; return 1; }
  [ -n "$SP" ]   || { bc "apply:abort:no-sp-uri"; return 1; }
  valid_uri "$DISP" || { bc "apply:abort:bad-display-uri:$DISP"; return 1; }
  valid_uri "$SP"   || { bc "apply:abort:bad-sp-uri:$SP"; return 1; }
  printf '{"nav":"%s","sp":"%s"}\n' "$NAVU" "$SP" > "$SAVE"
  say "saved nav state -> $SAVE (nav=$NAVU sp=$SP)"
  bc "apply:preflight-ok disp=$DISP sp=$SP"

  # [1] PARK: delete the nav element (idempotent -- absent is fine)
  if [ -n "$NAVU" ]; then
    bc "apply:park-nav $NAVU"
    h DELETE "http://$RSI:$DMP$NAVU" >/dev/null; echo
  else
    say "nav element already absent (already parked)"
  fi
  sleep 2

  # [2] REGISTER ours (idempotent -- skip if it already exists with the right id)
  R="$(uris)"; EXIST="$(id_of "$R" $OURNAME)"
  if [ "$EXIST" = "$OURID" ]; then
    say "our element already registered (internalIdentifier=$OURID)"
  else
    bc "apply:create-our-element"
    h POST "http://$RSI:$DMP/displaymanagement/renderingSurfaces" \
      "{\"name\":\"$OURNAME\",\"display\":\"$DISP\",\"streamParameters\":\"$SP\",\"topLeftX\":0,\"topLeftY\":0,\"width\":1280,\"height\":480}"; echo
    sleep 1
    GOT="$(id_of "$(uris)" "$OURNAME")"
    say "our internalIdentifier='$GOT' (want $OURID)"
    [ "$GOT" = "$OURID" ] || { bc "apply:abort:elem-not-3000"; oracle_tail; return 1; }
    bc "apply:element-ok id=$GOT"
  fi

  # [3] RESYNC: STOP -> RESYNC -> START (the only retry, and the ECU re-acquire)
  bc "apply:resync"
  h POST "http://$RSI:$DMP$SP" '{"resync":true}' >/dev/null; echo
  sleep 4
  oracle_tail
  bc "apply:done"
  do_verify
}

do_restore() {
  bc "restore:start"
  R="$(uris)"; D="$(disps)"; S="$(sps)"
  DISP="$(uri_of "$D" navi-kombi-display)"; SP="$(uri_of "$S" $SPNAME)"
  U="$(uri_of "$R" $OURNAME)"
  if [ -n "$U" ]; then bc "restore:delete-ours $U"; h DELETE "http://$RSI:$DMP$U" >/dev/null; echo; else say "our element already absent"; fi
  R="$(uris)"
  if [ -z "$(uri_of "$R" "$NAVNAME")" ]; then
    [ -n "$DISP" ] && [ -n "$SP" ] || { bc "restore:abort:no-uris"; return 1; }
    bc "restore:recreate-nav"
    h POST "http://$RSI:$DMP/displaymanagement/renderingSurfaces" \
      "{\"name\":\"$NAVNAME\",\"display\":\"$DISP\",\"streamParameters\":\"$SP\",\"topLeftX\":0,\"topLeftY\":0,\"width\":1280,\"height\":480}"; echo
    sleep 1
  else
    say "nav element already present (internalIdentifier $(id_of "$R" $NAVNAME))"
  fi
  [ -n "$SP" ] && { bc "restore:resync"; h POST "http://$RSI:$DMP$SP" '{"resync":true}' >/dev/null; echo; sleep 3; }
  oracle_tail
  bc "restore:done (helper/presenter still running: kill \$(pidof mib3egl2) if you want)"
  say "if the cluster is still blank, the ECU session lever:  sh patha_deliver.sh toggle"
}

do_verify() {
  _ok=1
  R="$(uris)"; GOT="$(id_of "$R" $OURNAME)"; [ "$GOT" = "$OURID" ] || { say "VERIFY FAIL: our element id='$GOT' (want $OURID)"; _ok=0; }
  [ -n "$(pids_of multi-screen-server)" ] || { say "VERIFY FAIL: multi-screen-server not running"; _ok=0; }
  grep -aq "startShare \[id = $OURID\]" "$MSSLOG" 2>/dev/null && say "VERIFY OK: MS-S did startShare [id = $OURID]" || say "VERIFY ?: no 'startShare [id = $OURID]' in the oracle yet"
  _f="$(msf_fds)"; [ "${_f:-0}" -ge 10 ] && say "VERIFY OK: MS-S fds=$_f (streaming)" || say "VERIFY ?: MS-S fds=$_f (6=parked, 11=streaming)"
  [ "$_ok" = "1" ] && say "verify: PASS (element owned)" || say "verify: FAIL"
  [ "$_ok" = "1" ]
}

watch_live() {
  _n="${1:-60}"; _i=0
  echo "[i] watch ${_n}s -- plug the iPhone and START A ROUTE IN APPLE MAPS (CarPlay nav)"
  printf "  %-6s %-10s %-9s %s\n" t frames msf_fds mss_last
  while [ "$_i" -lt "$_n" ]; do
    _f="$(grep -ao 'frames=[0-9x]*' /dev/shm/mib3helper.log 2>/dev/null | tail -n1)"
    _s="$(msf_fds)"
    _m="$(grep -aE 'STREAMING|surfaceId: [0-9]+|configure|add_listener' "$MSSLOG" 2>/dev/null | sed 's/^.*\[//' | tail -n1 | cut -c1-44)"
    printf "  %-6s %-10s %-9s %s\n" "${_i}s" "${_f:-?}" "${_s:-?}" "${_m:-<none>}"
    _i=$((_i+5)); sleep 5
  done
}

case "${1:-}" in
  status)  status ;;
  apply)   acquire || exit 1; do_apply ;;
  restore) acquire || exit 1; do_restore ;;
  verify)  do_verify ;;
  watch)   shift; watch_live "$@" ;;
  *) echo "usage: $0 {status|apply|verify|restore|watch [secs]}"; exit 2 ;;
esac
