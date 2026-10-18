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
#   sh deliver_ctl.sh oracle start      # start the MS-S live-log capture (the ORACLE; idempotent, append)
#   sh deliver_ctl.sh oracle stop       # stop the capture
#   sh deliver_ctl.sh oracle tail       # show the last share/stream decisions
# (start_mss_log.sh is a thin wrapper for the same thing, for the day-of card.)
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
  BCF="$STICK/deliver.bc"; MSSLOG="$STICK/mss.log"; SAVE="$STICK/deliver_nav.json"; LOCK="$STICK/deliver.lock"; OPID="$STICK/oracle.pid"
else
  BCF="${MIB3_DLV_RAM:-/dev/shm}/deliver.bc"; MSSLOG="${MIB3_DLV_RAM:-/dev/shm}/mss.log"; SAVE="${MIB3_DLV_RAM:-/dev/shm}/deliver_nav.json"; LOCK="${MIB3_DLV_RAM:-/dev/shm}/deliver.lock"; OPID="${MIB3_DLV_RAM:-/dev/shm}/oracle.pid"
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
h() { _hm="$1"; _hu="$2"; _hb="${3:-}"
  if [ -n "$_hb" ]; then curl -sS --max-time 8 -X "$_hm" -H 'Content-Type: application/json' --data "$_hb" "$_hu"
  else curl -sS --max-time 8 -X "$_hm" "$_hu"; fi; }
de() { tr -d '\\'; }
line_of() { printf '%s' "$1" | sed 's/},{/}\
{/g' | grep "\"name\":\"$2\"" | head -n1; }
uri_of()  { line_of "$1" "$2" | sed -n 's/.*"uri":"\([^"]*\)".*/\1/p' | de | head -n1; }
id_of()   { line_of "$1" "$2" | sed -n 's/.*"internalIdentifier":"\([^"]*\)".*/\1/p' | head -n1; }
uris() { curl -sS --max-time 8 "http://$RSI:$DMP/displaymanagement/renderingSurfaces"; }
disps(){ curl -sS --max-time 8 "http://$RSI:$DMP/displaymanagement/displays"; }
sps()  { curl -sS --max-time 8 "http://$RSI:$DMP/displaymanagement/streamParameters"; }
valid_uri() { case "$1" in /displaymanagement/renderingSurfaces/*|/displaymanagement/streamParameters/*|/displaymanagement/displays/*) return 0;; *) return 1;; esac; }

# The oracle is MS-S's own live log (port 54321) -- the ONLY way to read the share
# decision without eyes on the dash. It is started by apply/restore AND can be run
# earlier via `oracle start` / start_mss_log.sh. It is IDEMPOTENT and APPEND-only:
# pre-starting it is safe, and `apply` no longer truncates it (a pre-started capture
# used to be wiped by apply -- fixed 2026-10-16).
oracle_pid() {
  [ -f "$OPID" ] || return 1
  _p="$(cat "$OPID" 2>/dev/null)"
  [ -n "$_p" ] && kill -0 "$_p" 2>/dev/null && { printf '%s' "$_p"; return 0; }
  return 1
}
oracle_on() {
  P="$(oracle_pid)"
  if [ -n "$P" ]; then say "oracle already live (pid $P) -> $MSSLOG"; return 0; fi
  mkdir -p "$(dirname "$MSSLOG")" 2>/dev/null
  # curl writes the file itself, so $! is the curl pid (killable) and the harness'
  # pkill pattern ("curl -sN --max-time 3600 http://127.0.0.1:$LOGPORT") still matches.
  nohup curl -sN --max-time 3600 "http://$RSI:$LOGPORT/" >> "$MSSLOG" 2>/dev/null &
  echo $! > "$OPID" 2>/dev/null
  sleep 2
  if [ -s "$MSSLOG" ]; then say "oracle live (pid $(cat "$OPID" 2>/dev/null)) -> $MSSLOG"
  else say "WARN: oracle produced nothing yet (port $LOGPORT closed?)"; fi
  return 0
}
oracle_off() {
  P="$(oracle_pid)"
  if [ -n "$P" ]; then kill "$P" 2>/dev/null && say "oracle stopped (pid $P)"; else say "oracle not running"; fi
  rm -f "$OPID" 2>/dev/null
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
  _lc="${MIB3_DLV_LC:-LayerManagerControl}"     # overridable for the desk harness
  if command -v "$_lc" >/dev/null 2>&1; then
    XDG_RUNTIME_DIR=/run/platform/weston WAYLAND_DISPLAY=wayland-0 \
      "$_lc" get surface "$OURID" 2>/dev/null | grep -E "original size|source region"
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

# ============================================================================
# HANDOVER -- the event-driven replacement for `apply` (2026-10-18).
#
# WHY IT EXISTS: `apply` (park -> sleep 2 -> register -> resync) assumes MS-S has
# observed the park by the time we register ours.  It never verifies that, so a
# park MS-S did not process (the nav engine re-created its element; a missed 1 Hz
# check) leaves the nav's TransportStream holding (ip,port) and our start dies
# SILENTLY with `Failed to create transportStream` (startShareInternal @0x1175d0).
# See LANE_MS_S_HANDOVER_2026-10-18.md S1.
#
# HANDOVER gates on MS-S's OWN decisions (the oracle), one at a time:
#   check   our presenter owns ivi $OURID with a committed buffer
#   park    delete the nav element, then WAIT for `STOP STREAMING` (the nav's
#           TransportStream died -> (ip,port) is free)
#   attach  register our element, then WAIT for `startShare [id = $OURID]` + `add_listener = 0`
#   arm     resync (STOP->START), then WAIT for `share_surface_id = $OURID` (encoder built)
# Stages are idempotent and separately runnable (one step at a time, safety).
# ============================================================================

oracle_lines() { if [ -f "$MSSLOG" ]; then wc -l < "$MSSLOG" 2>/dev/null | tr -d ' '; else echo 0; fi; }
# wait_oracle <ERE> <secs> [from_line]  -> 0 if a NEW line (after from_line) matched
wait_oracle() {
  _re="$1"; _n="${2:-20}"; _from="${3:-0}"; _i=0
  while [ "$_i" -lt "$_n" ]; do
    if [ -f "$MSSLOG" ]; then
      sed -n "$((_from + 1)),\$p" "$MSSLOG" 2>/dev/null | grep -aqE "$_re" && return 0
    fi
    _i=$((_i + 1)); sleep 1
  done
  return 1
}
oracle_since() { sed -n "$1,\$p" "$MSSLOG" 2>/dev/null; }
oracle_live() { [ -s "$MSSLOG" ] 2>/dev/null; }

# read-only preconditions; echoes the reason on failure
handover_check() {
  presenter_ok || { say "HANDOVER NO-GO: no presenter (start mib3egl $OURID first)"; return 1; }
  _buf="$(surface_has_buffer)"
  echo "$_buf" | grep -qE "original size:[^0-9]*x=[1-9][0-9]*" \
    || { say "HANDOVER NO-GO: ivi $OURID has NO committed buffer (${_buf:-no LayerManagerControl})"; return 1; }
  [ "$(id_of "$(uris)" "$OURNAME")" = "$OURID" ] && say "HANDOVER: our element already attached" || true
  say "HANDOVER CHECK OK: presenter owns ivi $OURID with a committed buffer"
  return 0
}

handover_park() {
  R="$(uris)"; NAVU="$(uri_of "$R" $NAVNAME)"
  _mk="$(oracle_lines)"
  if [ -n "$NAVU" ]; then
    bc "handover:park $NAVU"; h DELETE "http://$RSI:$DMP$NAVU" >/dev/null; echo
  else
    say "nav element already absent (parked)"
  fi
  if [ "${NOOR:-0}" = "1" ]; then
    sleep 3; say "HANDOVER PARK: DEGRADED (no oracle) -- slept 3s instead of waiting for STOP STREAMING"
    return 0
  fi
  # WAIT for MS-S to say it stopped -- this is the (ip,port) being freed
  if wait_oracle "STOP STREAMING|stopShare \[" 20 "$_mk"; then
    say "HANDOVER PARK OK: MS-S logged STOP STREAMING (nav TransportStream destroyed)"
    return 0
  fi
  # if the nav was already gone before we started, there is nothing to stop
  if [ -z "$NAVU" ]; then
    say "HANDOVER PARK: nav was already absent; no stop expected"
    return 0
  fi
  say "HANDOVER PARK WARN: no STOP STREAMING within 20s -- MS-S may not have observed the park"
  say "  (this is exactly the state that makes our start fail silently with"
  say "   'Failed to create transportStream'; do NOT attach yet)"
  return 1
}

handover_attach() {
  R="$(uris)"; D="$(disps)"; S="$(sps)"
  DISP="$(uri_of "$D" navi-kombi-display)"; SP="$(uri_of "$S" $SPNAME)"
  valid_uri "$DISP" || { say "HANDOVER NO-GO: bad display uri '$DISP'"; return 1; }
  valid_uri "$SP"   || { say "HANDOVER NO-GO: bad sp uri '$SP'"; return 1; }
  _mk="$(oracle_lines)"           # marked BEFORE any POST (h() clobbers _m, not _mk)
  EXIST="$(id_of "$R" $OURNAME)"
  if [ "$EXIST" = "$OURID" ]; then
    say "our element already registered (internalIdentifier=$OURID)"
    if grep -aq "startShare \[id = $OURID\]" "$MSSLOG" 2>/dev/null; then
      say "HANDOVER ATTACH OK: already shared (startShare [id = $OURID] in the oracle)"
      return 0
    fi
    # not yet shared (or the oracle restarted): fall through and let resync re-arm it
    say "HANDOVER ATTACH: element present but no startShare yet -> run: $0 handover arm"
    return 0
  fi
  bc "handover:attach"
  h POST "http://$RSI:$DMP/displaymanagement/renderingSurfaces" \
    "{\"name\":\"$OURNAME\",\"display\":\"$DISP\",\"streamParameters\":\"$SP\",\"topLeftX\":0,\"topLeftY\":0,\"width\":1280,\"height\":480}" >/dev/null; echo
  sleep 1
  GOT="$(id_of "$(uris)" "$OURNAME")"
  [ "$GOT" = "$OURID" ] || { say "HANDOVER NO-GO: element id='$GOT' (want $OURID)"; oracle_tail; return 1; }
  if [ "${NOOR:-0}" = "1" ]; then
    sleep 2; say "HANDOVER ATTACH: DEGRADED (no oracle) -- element created; cannot verify startShare"
    return 0
  fi
  if wait_oracle "Failed to create transportStream" 3 "$_mk"; then
    say "HANDOVER FAIL: 'Failed to create transportStream' -- an (ip,port) stream still exists"
    oracle_tail; return 2
  fi
  if wait_oracle "startShareInternal status = " 3 "$_mk"; then
    say "HANDOVER FAIL: holder status wedged ('startShareInternal status = N') -- run: $0 handover arm (resync clears it)"
    oracle_tail; return 2
  fi
  wait_oracle "startShare \[id = $OURID\]" 20 "$_mk" \
    || { say "HANDOVER FAIL: no 'startShare [id = $OURID]' in the oracle"; oracle_tail; return 1; }
  say "HANDOVER ATTACH OK: MS-S startShare [id = $OURID]"
  wait_oracle "add_listener = 0" 8 "$_mk" || say "HANDOVER WARN: no 'add_listener = 0' yet"
  return 0
}

handover_arm() {
  S="$(sps)"; SP="$(uri_of "$S" $SPNAME)"
  valid_uri "$SP" || { say "HANDOVER NO-GO: bad sp uri '$SP'"; return 1; }
  _mk="$(oracle_lines)"
  bc "handover:arm (resync 0->1)"
  # resync is TIMESTAMP-GATED in updateStreamParametersElement @0x131a30: it fires only
  # on resync==true AND a changed timestamp.  Re-POSTing true after it was latched true
  # is a NO-OP (session 6 hit exactly this), so drive a real 0 -> 1 transition.
  if [ "${MIB3_DLV_RESYNC_FLIP:-1}" = "1" ]; then
    h POST "http://$RSI:$DMP$SP" '{"resync":false}' >/dev/null 2>&1; echo
    sleep 1
  fi
  h POST "http://$RSI:$DMP$SP" '{"resync":true}' >/dev/null; echo
  if [ "${NOOR:-0}" = "1" ]; then
    sleep 4; say "HANDOVER ARM: DEGRADED (no oracle) -- resync sent; cannot verify share_surface_id"
    return 0
  fi
  _i=0
  while [ "$_i" -lt 15 ]; do
    wait_oracle "share_surface_id = $OURID" 1 "$_mk" \
      && { say "HANDOVER ARM OK: encoder built for $OURID (share_surface_id = $OURID)"; return 0; }
    wait_oracle "Specified surface \(ID:$OURID\) does not exist" 1 "$_mk" \
      && { say "HANDOVER ARM FAIL: Weston says ivi $OURID does not exist -- the presenter is not committing"; return 1; }
    _i=$((_i + 1))
  done
  say "HANDOVER ARM FAIL: no 'share_surface_id = $OURID' within 30s"; oracle_tail; return 1
}

do_handover() {
  _st="${1:-full}"
  bc "handover:start stage=$_st"
  oracle_on
  if oracle_live; then NOOR=0; else
    NOOR=1
    say "HANDOVER WARN: the MS-S oracle is silent (port $LOGPORT closed?) -- gates DEGRADE to"
    say "  fixed waits and the pass/fail verdicts become unverifiable. Read the dash + MS-S fds."
  fi
  export NOOR
  case "$_st" in
    check)  handover_check; return $? ;;
    park)   handover_park; return $? ;;
    attach) handover_attach; return $? ;;
    arm)    handover_arm; return $? ;;
  esac
  handover_check || return 1
  handover_park  || { say "STOP: park not observed -- not attaching (run again when MS-S logs STOP STREAMING)"; return 1; }
  handover_attach || return $?
  handover_arm    || return $?
  do_verify
  bc "handover:done stage=$_st"
  say "HANDOVER COMPLETE: look at the cluster. If blank: sh patha_deliver.sh toggle (ECU lever)"
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
  handover) shift; acquire || exit 1; do_handover "${1:-full}"; exit $? ;;
  restore) acquire || exit 1; do_restore ;;
  verify)  do_verify ;;
  watch)   shift; watch_live "$@" ;;
  oracle)  shift; case "${1:-start}" in
             start) oracle_on ;;
             stop)  oracle_off ;;
             tail)  oracle_tail ;;
             *) echo "usage: $0 oracle {start|stop|tail}"; exit 2 ;;
           esac ;;
  *) echo "usage: $0 {status|apply|handover [check|park|attach|arm|full]|verify|restore|watch [secs]|oracle {start|stop|tail}}"; exit 2 ;;
esac
