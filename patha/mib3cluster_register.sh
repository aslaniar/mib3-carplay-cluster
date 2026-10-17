#!/bin/sh
# ============================================================================
# mib3cluster_register.sh -- PATH A: get the car to stream an ivi surface WE own.
#
#   *** LIVE-ONLY. Runs ON THE UNIT over the live foothold. ***
#
# WHAT IT DOES (see MIB3_RE/LANE_DISPLAYMGR_PATH_A_2026-10-16.md S3).
#   The instrument-cluster video is produced by `multi-screen-server` (MS-S)
#   grabbing, via the Weston `ivi_share` protocol, the ivi surface whose id
#   equals the RSI `displaymanagement.renderingSurfaces` element's
#   `internalIdentifier`. That field is SERVER-ASSIGNED (from `surfaces.map`)
#   and cannot be set. So to make the car stream OUR frames:
#     1. our helper creates an ivi surface with a chosen id (MIB3_SHIM_IVIID);
#     2. we POST a NEW renderingSurface element whose NAME is one that
#        `surfaces.map` already maps to that id;
#     3. MS-S ingests the new element dynamically (no restart) and starts
#        grabbing our id;
#     4. `apply --take-over` moves the cluster stream off the nav's element so
#        the two encoders do not feed the same multicast group.
#
# SAFETY. Never restarts carplay / rsi-display-management / multi-display. The
# only effects are RSI element create/update/delete. `restore` undoes them. A
# reboot is the full undo. RAM-only.
#
# RSI transport: TCP 127.0.0.1:49600 by default, or a UDS if RSI_SOCK is set.
# The exact RSI route/addressing is RUNTIME-ONLY -- this tool prints what it
# sees and is parameterisable (see the NOTE at the bottom).
#
# USAGE (on the unit):
#   sh mib3cluster_register.sh discover
#   sh mib3cluster_register.sh apply --name <spareName> --id <iviId> [--take-over]
#   sh mib3cluster_register.sh restore
#
# ENV: RSI_HOST(127.0.0.1) RSI_PORT(49600) RSI_SOCK(=UDS path) API(/api/v1)
#      SAVE(/dev/shm/patha_save.json)
# ============================================================================
set -u

RSI_HOST="${RSI_HOST:-127.0.0.1}"
RSI_PORT="${RSI_PORT:-49600}"
RSI_SOCK="${RSI_SOCK:-}"
API="${API:-/api/v1}"
SAVE="${SAVE:-/dev/shm/patha_save.json}"
PY="${PY:-python}"

# ---- HTTP shim -------------------------------------------------------------
http() { # http METHOD PATH [JSON]
  _m="$1"; _p="$2"; _b="${3:-}"
  if [ -n "$RSI_SOCK" ]; then
    curl -sS --max-time 10 --unix-socket "$RSI_SOCK" -X "$_m" \
         ${_b:+-H 'Content-Type: application/json' --data "$_b"} \
         "http://localhost$API$_p"
  else
    curl -sS --max-time 10 -X "$_m" \
         ${_b:+-H 'Content-Type: application/json' --data "$_b"} \
         "http://$RSI_HOST:$RSI_PORT$API$_p"
  fi
}

# ---- element extractor: prints "name<TAB>uri<TAB>internalIdentifier<TAB>displayUri<TAB>spUri"
#      accepts a bare array or {data|items|elements:[...]}. $2 = optional name filter.
extract() {
  printf '%s' "$1" | NAME_FILTER="${2:-}" "$PY" -c '
import sys,json,os
try: d=json.load(sys.stdin)
except Exception as e: sys.stderr.write("json error: %s\n"%e); sys.exit(3)
els = d if isinstance(d,list) else d.get("data", d.get("items", d.get("elements",[d])))
if isinstance(els,dict): els=[els]
flt=os.environ.get("NAME_FILTER","")
for e in els:
    if not isinstance(e,dict): continue
    def s(v): return v.get("uri") if isinstance(v,dict) else (v or "")
    nm=e.get("name","")
    if flt and nm!=flt: continue
    print("%s\t%s\t%s\t%s\t%s"%(nm, e.get("uri") or e.get("id") or "",
        e.get("internalIdentifier",""), s(e.get("display")), s(e.get("streamParameters"))))
'
}
field() { printf '%s\n' "$1" | cut -f"$2" ; }   # field <tsv-line> <n>

# ---- 0. discovery ----------------------------------------------------------
discover() {
  echo "== [0] surfaces.map (the linchpin: name -> ivi id; read ONCE by rsi-display-management) =="
  found=""
  for p in /surfaces.map; do
    [ -f "$p" ] && { echo "--- $p ---"; cat "$p"; found=1; }
  done
  pid="$(pidof rsi-display-management 2>/dev/null)"
  if [ -n "$pid" ]; then
    for base in "/proc/$pid/cwd" "/proc/$pid/root"; do
      [ -f "$base/surfaces.map" ] && { echo "--- $base/surfaces.map ---"; cat "$base/surfaces.map"; found=1; }
    done
    echo "[i] rsi-display-management pid=$pid cwd=$(readlink /proc/$pid/cwd 2>/dev/null)"
  fi
  [ -n "$found" ] || echo "[!] surfaces.map not found at the obvious paths -- locate it (find / -name surfaces.map) and read it live."

  echo
  echo "== [1] renderingSurfaces (internalIdentifier = the ivi id MS-S grabs) =="
  RS="$(http GET /displaymanagement/renderingSurfaces)"
  printf '%s\n' "$RS"
  echo "  -- parsed --"
  extract "$RS"

  echo
  echo "== [2] displays =="
  http GET /displaymanagement/displays; echo
  echo "== [3] streamParameters =="
  http GET /displaymanagement/streamParameters; echo
  echo "[i] decision gate: does surfaces.map have a name -> id with id != the live cluster internalIdentifier?"
  echo "    YES -> apply --name <thatName> --id <thatId>          (PATH A-1)"
  echo "    NO  -> A-2 takeover (lane doc S6 Phase 4) or Path B"
}

apply() {
  NAME=""; ID=""; TAKEOVER=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --name) NAME="$2"; shift 2 ;;
      --id)   ID="$2";   shift 2 ;;
      --take-over) TAKEOVER=1; shift ;;
      *) echo "unknown arg: $1"; exit 2 ;;
    esac
  done
  [ -n "$NAME" ] && [ -n "$ID" ] || { echo "need --name <spareName> --id <iviId>"; exit 2; }

  RS="$(http GET /displaymanagement/renderingSurfaces)"
  DR="$(http GET /displaymanagement/displays)"

  NAVLINE="$(extract "$RS" navi-kombi-renderingsurface | head -1)"
  DISPTAB="$(extract "$DR" navi-kombi-display | head -1)"
  DISPURI="$(field "$DISPTAB" 2)"
  SPURI="$(field "$NAVLINE" 5)"
  NAVURI="$(field "$NAVLINE" 2)"
  if [ -z "$SPURI" ]; then
    SPR="$(http GET /displaymanagement/streamParameters)"
    SPURI="$(extract "$SPR" navi-kombi-streamparameters | head -1 | cut -f2)"
  fi
  [ -n "$DISPURI" ] || { echo "[!] navi-kombi-display uri not found"; exit 1; }
  [ -n "$SPURI" ]   || { echo "[!] navi-kombi-streamparameters uri not found"; exit 1; }
  echo "[i] display=$DISPURI  streamParameters=$SPURI  navElement=$NAVURI"
  echo "{\"uri\":\"$NAVURI\",\"streamParameters\":\"$SPURI\"}" > "$SAVE"
  echo "[i] saved original nav element state -> $SAVE"

  echo "[*] POST new renderingSurface name=$NAME display=$DISPURI streamParameters=$SPURI (id $ID must be owned by our helper)"
  BODY="{\"name\":\"$NAME\",\"display\":\"$DISPURI\",\"streamParameters\":\"$SPURI\",\"topLeftX\":0,\"topLeftY\":0,\"width\":1280,\"height\":480}"
  http POST /displaymanagement/renderingSurfaces "$BODY"; echo

  if [ "$TAKEOVER" = "1" ] && [ -n "$NAVURI" ]; then
    echo "[*] take-over: clearing the nav element's streamParameters ($NAVURI)"
    # FIELD RESULT 2026-10-16: the unit REJECTS an empty streamParameters (invalid value) -- this POST
    # returns 200 but does NOT change the element, and multi-screen-server keeps streaming. A dead-port
    # streamParameters (create one; repoint the element to its uri) also did NOT stop ms-s. See
    # HANDOFF_DELIVERY_2026-10-16.md S3/S6. This block is a placeholder only; the real silence
    # mechanism is UNSOLVED -- verify with a --probe that the car's PT=33 stream is gone before relying
    # on it.
    http POST "$NAVURI" '{"streamParameters":""}'; echo
  fi
  echo "[i] watch: MS-S log startShare [id = $ID]; then check the dash."
  echo "[i] restore:  sh $0 restore"
}

restore() {
  [ -f "$SAVE" ] || { echo "[!] no saved state ($SAVE)"; exit 1; }
  NAVURI="$(sed -n 's/.*"uri":"\([^"]*\)".*/\1/p' "$SAVE")"
  ORIGSP="$(sed -n 's/.*"streamParameters":"\([^"]*\)".*/\1/p' "$SAVE")"
  echo "[*] restoring nav element $NAVURI streamParameters=$ORIGSP"
  [ -n "$NAVURI" ] && [ -n "$ORIGSP" ] && { http POST "$NAVURI" "{\"streamParameters\":\"$ORIGSP\"}"; echo; }
  echo "[i] also DELETE our element if still present: run 'discover', then curl -X DELETE <our element uri>"
  rm -f "$SAVE"
}

case "${1:-}" in
  discover) discover ;;
  apply)    shift; apply "$@" ;;
  restore)  restore ;;
  *) echo "usage: $0 {discover | apply --name N --id I [--take-over] | restore}"; exit 2 ;;
esac

# NOTE (runtime-only): the exact RSI route/addressing (bare array vs {data:[]},
# element uri shape, whether an empty streamParameters clears it) is confirmed
# live with `discover`. Everything here is read/POST/DELETE only -- no service
# is restarted.
