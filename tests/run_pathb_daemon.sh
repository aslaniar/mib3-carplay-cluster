#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# ============================================================================
# run_pathb_daemon.sh -- desk bench for pathb_daemon.sh + pathbctl (2026-11-02)
#
# No car, no unit, no injection: the sender, the RSI handover and the kernel port
# check are all replaced by seam scripts that keep a LOG OF WHAT HAPPENED AND IN
# WHAT ORDER.  That order is the thing under test -- the two invariants paid for on
# the car are "park before bind" and "unbind before hand back", and an assertion on
# a state name cannot see either of them.
#
#   D0   both scripts are syntactically valid for /bin/sh
#   D1   no phone -> nothing is touched (no park, no bind)
#   D2   a 200 ms blip of frames does not trigger a takeover (the debounce)
#   D3   frames flowing -> TAKEOVER -> ACTIVE, park BEFORE bind
#   D4   with the car's nav still streaming, bind happens only after park
#   D5   frames stop -> RELEASE, unbind BEFORE hand back, nav gets the port
#   D6   `pathbctl release` while ACTIVE hands the cluster back
#   D7   park fails after we bound -> we give the port straight back
#   D8   the pause marker stops any takeover
#   D9   a second daemon refuses to run (single instance)
#   D10  restart with a sender that still holds the port -> reconcile releases
#   D11  the sender dying while ACTIVE -> release, then a new sender
#   D12  the state file carries the fields the CLI needs
#   D13  the CLI reports the daemon and its state
#   D14  a hand-back that fails must NOT report IDLE (it degrades, then recovers)
#   D15  a takeover that fails before we bind leaves IDLE with the nav restored
#
# Usage: sh tests/run_pathb_daemon.sh
# ============================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
KIT="$(cd "$HERE/../src/control" && pwd)"
DAEMON="$KIT/pathb_daemon.sh"
CLI="$KIT/pathbctl"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/pathbd.XXXXXX") || exit 1

P=0; F=0
ok(){ echo "DAEMON $1 exp=$2 got=$2 PASS"; P=$((P+1)); }
no(){ echo "DAEMON $1 exp=$2 got=$3 FAIL"; F=$((F+1)); }
chk(){ if [ "$2" = "1" ]; then ok "$1" "$3"; else no "$1" "$3" "${4:-no}"; fi; }
has(){ grep -q "$2" "$3" 2>/dev/null && echo 1 || echo 0; }
first_line(){ grep -n "$1" "$2" 2>/dev/null | head -n 1 | cut -d: -f1; }

cleanup(){
  sh "$DAEMON" stop >/dev/null 2>&1
  [ -f "$TMP/pathb_sender.pid" ] && kill "$(cat "$TMP/pathb_sender.pid")" 2>/dev/null
  pkill -f "$TMP/fake_sender.sh" 2>/dev/null
  rm -rf "$TMP"
}
trap 'cleanup; exit 1' INT TERM HUP

# ---------------------------------------------------------------- the fakes
cat >"$TMP/fake_sender.sh" <<'FAKE'
#!/bin/sh
RAM=${PATHB_RAM:?PATHB_RAM must be set for the fake sender}
S=$RAM/pathb_conn.state
C=$RAM/pathb_sender.ctl
H=$RAM/port_held
P=$RAM/fake_phone
O=$RAM/order.log
conn=1; ready=0; bound=0; au=0; out=0; last=""
if [ -n "${FAKE_BOUND:-}" ]; then bound=$FAKE_BOUND; echo us >"$H"; fi
while :; do
  _c=""; [ -r "$C" ] && _c=$(cat "$C" 2>/dev/null)
  if [ "$_c" != "$last" ]; then
    last=$_c
    case "$_c" in
      bind*)   _p=${_c#bind }; _p=${_p# }
               if [ "$bound" = "0" ] && [ -n "$_p" ]; then
                 bound=$_p; echo us >"$H"; echo "bind $bound" >>"$O"
               fi ;;
      unbind*) if [ "$bound" != "0" ]; then
                 bound=0; [ "$(cat "$H" 2>/dev/null)" = "us" ] && rm -f "$H"
                 echo unbind >>"$O"
               fi ;;
    esac
  fi
  if [ -f "$P" ]; then
    au=$((au+1)); ready=1
    [ "$bound" != "0" ] && out=$((out+1))
  else
    ready=0
  fi
  printf 'conn=%s ready=%s bound=%s au=%s out=%s ms=0 seq=1\n' \
         "$conn" "$ready" "$bound" "$au" "$out" >"$S"
  sleep 0.1
done
FAKE
chmod +x "$TMP/fake_sender.sh"

cat >"$TMP/park.sh" <<'PARK'
#!/bin/sh
sleep "${PARK_MS:-0.2}"
echo park >>"$PATHB_RAM/order.log"
[ "${PARK_KEEP_PORT:-0}" = "1" ] || rm -f "$PATHB_RAM/port_held"
exit "${PARK_RC:-0}"
PARK
chmod +x "$TMP/park.sh"

cat >"$TMP/back.sh" <<'BACK'
#!/bin/sh
# The fail flag is a FILE, not an env var: the daemon's environment is fixed at
# exec, so a test that wants to change the RSI's behaviour mid-run must not use
# an exported variable.
if [ -f "$PATHB_RAM/back_fail" ]; then
  echo "back-failed" >>"$PATHB_RAM/order.log"
  exit 1
fi
echo back >>"$PATHB_RAM/order.log"
echo mss >"$PATHB_RAM/port_held"
exit 0
BACK
chmod +x "$TMP/back.sh"

export PATHB_RAM="$TMP"
export PATHB_BENCH=1
export PATHB_PORT=42989
export PATHB_MUXER="$TMP/fake_sender.sh"
export PATHB_D_SENDER_COMM=sh
export PATHB_D_PARK_CMD="sh $TMP/park.sh"
export PATHB_D_BACK_CMD="sh $TMP/back.sh"
export PATHB_D_PORTFREE_CMD="[ ! -f $TMP/port_held ]"
export PATHB_DELIVER=/nonexistent
export PATHB_POLL_MS=100
export PATHB_READY_STABLE_MS=600
export PATHB_GRACE_MS=700
export PATHB_RETRY_MS=600
export PATHB_TAKEOVER_MS=4000
export PATHB_RELEASE_MS=4000

phone_on(){  : >"$TMP/fake_phone"; }
phone_off(){ rm -f "$TMP/fake_phone"; }
reset_all(){
  sh "$DAEMON" stop >/dev/null 2>&1
  [ -f "$TMP/pathb_sender.pid" ] && kill "$(cat "$TMP/pathb_sender.pid")" 2>/dev/null
  pkill -f "$TMP/fake_sender.sh" 2>/dev/null
  sleep 0.2
  rm -f "$TMP"/pathb_sender.pid "$TMP"/pathb_conn.state "$TMP"/pathb_sender.ctl \
        "$TMP"/pathb_daemon.state "$TMP"/pathb_daemon.log "$TMP"/pathb_daemon.cmd \
        "$TMP"/pathb_daemon.pause "$TMP"/pathb_daemon.pid "$TMP"/order.log \
        "$TMP"/port_held "$TMP"/fake_phone "$TMP"/back_fail
}
dstart(){ nohup sh "$DAEMON" run >"$TMP/daemon.stdout" 2>&1 & sleep 0.8; }
dstate(){ sed -n 's/^state=\([A-Z]*\).*/\1/p' "$TMP/pathb_daemon.state" 2>/dev/null; }
dfield(){ sed -n "s/.*$1=\\([^ ]*\\).*/\\1/p" "$TMP/pathb_daemon.state" 2>/dev/null; }
wait_state(){ _n=$(( $2 / 50 )); while [ "$_n" -gt 0 ]; do
    [ "$(dstate)" = "$1" ] && return 0; sleep 0.05; _n=$((_n-1)); done; return 1; }

echo "############ D0: syntax ############"
sh -n "$DAEMON" 2>/dev/null && chk d0_daemon_syntax 1 "sh -n" || chk d0_daemon_syntax 0 "sh -n failed"
sh -n "$CLI" 2>/dev/null && chk d0_cli_syntax 1 "sh -n" || chk d0_cli_syntax 0 "sh -n failed"

echo
echo "############ D1: no phone, no action ############"
reset_all; dstart
sleep 1.2
chk d1_stays_idle "$( [ "$(dstate)" = "IDLE" ] && echo 1 || echo 0 )" "IDLE"
chk d1_no_park "$( [ -z "$(first_line park "$TMP/order.log")" ] && echo 1 || echo 0 )" "no park"
chk d1_no_bind "$( [ -z "$(first_line bind "$TMP/order.log")" ] && echo 1 || echo 0 )" "no bind"
reset_all

echo
echo "############ D2: a blip is not a session (debounce) ############"
dstart; phone_on; sleep 0.2; phone_off
sleep 1.2
chk d2_no_takeover "$( [ -z "$(first_line bind "$TMP/order.log")" ] && echo 1 || echo 0 )" "no bind"
chk d2_still_idle "$( [ "$(dstate)" = "IDLE" ] && echo 1 || echo 0 )" "IDLE"
reset_all

echo
echo "############ D3/D4: takeover, park BEFORE bind ############"
printf 'mss\n' >"$TMP/port_held"        # the car's nav owns the port
dstart; phone_on
if wait_state ACTIVE 5000; then chk d3_reaches_active 1 "ACTIVE"; else chk d3_reaches_active 0 "$(dstate)"; fi
_p=$(first_line park "$TMP/order.log"); _b=$(first_line bind "$TMP/order.log")
chk d3_park_happened "$( [ -n "$_p" ] && echo 1 || echo 0 )" "park in the order log"
chk d3_bind_happened "$( [ -n "$_b" ] && echo 1 || echo 0 )" "bind in the order log"
chk d4_park_before_bind "$( [ -n "$_p" ] && [ -n "$_b" ] && [ "$_p" -lt "$_b" ] && echo 1 || echo 0 )" "park ($_p) < bind ($_b)"
chk d3_state_bound "$( [ "$(dfield bound)" = "42989" ] && echo 1 || echo 0 )" "bound=42989"
chk d4_port_ours "$( [ "$(cat "$TMP/port_held" 2>/dev/null)" = "us" ] && echo 1 || echo 0 )" "port held by us"

echo
echo "############ D5: frames stop -> release, unbind BEFORE hand back ############"
phone_off
sleep 0.3
if wait_state IDLE 6000; then chk d5_back_to_idle 1 "IDLE"; else chk d5_back_to_idle 0 "$(dstate)"; fi
_u=$(first_line unbind "$TMP/order.log"); _k=$(first_line back "$TMP/order.log")
chk d5_unbind_before_back "$( [ -n "$_u" ] && [ -n "$_k" ] && [ "$_u" -lt "$_k" ] && echo 1 || echo 0 )" "unbind ($_u) < back ($_k)"
chk d5_nav_has_the_port "$( [ "$(cat "$TMP/port_held" 2>/dev/null)" = "mss" ] && echo 1 || echo 0 )" "port back to MS-S"
reset_all

echo
echo "############ D6: pathbctl release while ACTIVE ############"
dstart; phone_on
wait_state ACTIVE 5000 >/dev/null 2>&1
PATHB_RAM="$TMP" sh "$CLI" release >/dev/null 2>&1
if wait_state IDLE 6000; then chk d6_operator_release 1 "IDLE after release"; else chk d6_operator_release 0 "$(dstate)"; fi
chk d6_back_called "$( [ -n "$(first_line back "$TMP/order.log")" ] && echo 1 || echo 0 )" "hand-back ran"
chk d6_reason_logged "$( has x 'operator' "$TMP/pathb_daemon.log" )" "logged as operator"
reset_all

echo
echo "############ D7: park fails after we bound -> give the port back ############"
export PARK_RC=1
dstart; phone_on
sleep 3
chk d7_bind_happened "$( [ -n "$(first_line bind "$TMP/order.log")" ] && echo 1 || echo 0 )" "bound first"
chk d7_unbound_again "$( [ -n "$(first_line unbind "$TMP/order.log")" ] && echo 1 || echo 0 )" "released the port"
chk d7_handed_back "$( [ -n "$(first_line back "$TMP/order.log")" ] && echo 1 || echo 0 )" "handed the cluster back"
chk d7_not_active "$( [ "$(dstate)" != "ACTIVE" ] && echo 1 || echo 0 )" "not ACTIVE ($(dstate))"
unset PARK_RC
reset_all

echo
echo "############ D8: the pause marker ############"
: >"$TMP/pathb_daemon.pause"
dstart; phone_on
sleep 2
chk d8_no_takeover "$( [ -z "$(first_line bind "$TMP/order.log")" ] && echo 1 || echo 0 )" "no bind"
chk d8_idle "$( [ "$(dstate)" = "IDLE" ] && echo 1 || echo 0 )" "IDLE"
# THE DAEMON MUST STILL BE ALIVE.  An early version left the tick with `break`,
# which exits the loop this function is called from: pausing would silently kill
# the supervisor, and "stayed IDLE" would still have looked correct.
chk d8_daemon_alive "$( kill -0 "$(cat "$TMP/pathb_daemon.pid" 2>/dev/null)" 2>/dev/null && echo 1 || echo 0 )" "still running"
rm -f "$TMP/pathb_daemon.pause"
if wait_state ACTIVE 5000; then chk d8_resumes 1 "ACTIVE after resume"; else chk d8_resumes 0 "$(dstate)"; fi
reset_all

echo
echo "############ D9: single instance ############"
dstart
sh "$DAEMON" run >"$TMP/second.out" 2>&1 &
_sec=$!
sleep 0.8
kill -9 "$_sec" 2>/dev/null
chk d9_refuses_second "$( has x 'already running' "$TMP/second.out" )" "the second instance refused"
chk d9_first_alive "$( kill -0 "$(cat "$TMP/pathb_daemon.pid" 2>/dev/null)" 2>/dev/null && echo 1 || echo 0 )" "first still running"
reset_all

echo
echo "############ D10: restart with a sender still holding the port ############"
FAKE_BOUND=42989 nohup sh "$TMP/fake_sender.sh" >/dev/null 2>&1 &
_fpid=$!
sleep 0.4
printf '%s\n' "$_fpid" >"$TMP/pathb_sender.pid"
dstart
_i=0
while [ "$_i" -lt 80 ]; do
  grep -q back "$TMP/order.log" 2>/dev/null && break
  sleep 0.05; _i=$((_i+1))
done
# D10 asserts the INVARIANT, not the mechanism.  The supervisor stops the
# orphaned sender first (a restarted daemon must never leave a pre-fix sender
# running), and stopping it frees the port, so there is no live process left to
# receive an "unbind" -- an assertion on that line was asserting a mechanism the
# code no longer uses.  What must be true: the orphan is gone, the cluster is
# handed back, and the decision is logged as a reconcile.
chk d10_orphan_stopped "$( kill -0 "$_fpid" 2>/dev/null && echo 0 || echo 1 )" "stopped the orphaned sender"
chk d10_handed_back "$( [ -n "$(first_line back "$TMP/order.log")" ] && echo 1 || echo 0 )" "handed the cluster back"
chk d10_reconciled "$( has x 'reconcile' "$TMP/pathb_daemon.log" )" "logged as reconcile"
reset_all

echo
echo "############ D11: the sender dying while ACTIVE ############"
dstart; phone_on
wait_state ACTIVE 5000 >/dev/null 2>&1
_spid=$(cat "$TMP/pathb_sender.pid" 2>/dev/null)
kill -9 "$_spid" 2>/dev/null
sleep 2
chk d11_not_active "$( [ "$(dstate)" != "ACTIVE" ] && echo 1 || echo 0 )" "left ACTIVE ($(dstate))"
chk d11_handed_back "$( [ -n "$(first_line back "$TMP/order.log")" ] && echo 1 || echo 0 )" "handed the cluster back"
_s2=$(cat "$TMP/pathb_sender.pid" 2>/dev/null)
chk d11_sender_restarted "$( [ -n "$_s2" ] && [ "$_s2" != "$_spid" ] && kill -0 "$_s2" 2>/dev/null && echo 1 || echo 0 )" "a new sender ($_s2)"
reset_all

echo
echo "############ D14: a hand-back that fails must NOT look like IDLE ############"
: >"$TMP/back_fail"
dstart; phone_on
wait_state ACTIVE 5000 >/dev/null 2>&1
phone_off
sleep 0.3
_i=0; while [ "$_i" -lt 100 ]; do
  [ "$(dstate)" = "DEGRADED" ] && break; sleep 0.05; _i=$((_i+1)); done
chk d14_degrades "$( [ "$(dstate)" = "DEGRADED" ] && echo 1 || echo 0 )" "DEGRADED ($(dstate))"
chk d14_not_idle "$( [ "$(dstate)" != "IDLE" ] && echo 1 || echo 0 )" "not IDLE while bound"
# and it recovers by itself once the RSI answers again
rm -f "$TMP/back_fail"
_i=0; while [ "$_i" -lt 200 ]; do
  [ "$(dstate)" = "IDLE" ] && break; sleep 0.05; _i=$((_i+1)); done
chk d14_recovers "$( [ "$(dstate)" = "IDLE" ] && echo 1 || echo 0 )" "IDLE after the retry ($(dstate))"
reset_all

echo
echo "############ D15: a takeover that fails BEFORE we bind ############"
# The park fails and the port is still MS-S's.  A busy port is then CORRECT, and
# this must end in IDLE (nav intact), not DEGRADED.
export PARK_RC=1 PARK_KEEP_PORT=1
printf 'mss\n' >"$TMP/port_held"
dstart; phone_on
_i=0; while [ "$_i" -lt 200 ]; do
  [ "$(dstate)" = "IDLE" ] && [ -n "$(first_line park "$TMP/order.log")" ] && break
  sleep 0.05; _i=$((_i+1))
done
chk d15_never_bound "$( [ -z "$(first_line bind "$TMP/order.log")" ] && echo 1 || echo 0 )" "no bind"
chk d15_stays_idle "$( [ "$(dstate)" = "IDLE" ] && echo 1 || echo 0 )" "IDLE ($(dstate))"
chk d15_handed_back "$( [ -n "$(first_line back "$TMP/order.log")" ] && echo 1 || echo 0 )" "nav restored anyway"
unset PARK_RC PARK_KEEP_PORT
reset_all

echo
echo "############ D12/D13: the state file and the CLI ############"
dstart; phone_on
wait_state ACTIVE 5000 >/dev/null 2>&1
_line=$(cat "$TMP/pathb_daemon.state" 2>/dev/null)
chk d12_state_field "$( printf '%s' "$_line" | grep -q '^state=' && echo 1 || echo 0 )" "state="
chk d12_since_field "$( printf '%s' "$_line" | grep -q 'since_ms=' && echo 1 || echo 0 )" "since_ms="
chk d12_port_field "$( printf '%s' "$_line" | grep -q 'port=' && echo 1 || echo 0 )" "port="
chk d12_sender_field "$( printf '%s' "$_line" | grep -q 'sender=' && echo 1 || echo 0 )" "sender="
_out=$(PATHB_RAM="$TMP" sh "$CLI" status 2>&1)
chk d13_cli_runs "$( printf '%s' "$_out" | grep -q 'Path B' && echo 1 || echo 0 )" "CLI printed a report"
chk d13_cli_shows_state "$( printf '%s' "$_out" | grep -q 'ACTIVE' && echo 1 || echo 0 )" "CLI shows ACTIVE"
chk d13_cli_shows_ours "$( printf '%s' "$_out" | grep -q 'OURS' && echo 1 || echo 0 )" "CLI says the cluster is ours"
reset_all

echo
echo "============================================================"
echo "Path B daemon bench: PASS=$P FAIL=$F   (temp: $TMP)"
echo "============================================================"
[ "$F" = "0" ] && { rm -rf "$TMP"; trap - INT TERM HUP; exit 0; }
exit 1
