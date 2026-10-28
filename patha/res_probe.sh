#!/bin/sh
# ============================================================================
# res_probe.sh -- THE 2x EXPERIMENT: does the phone render the cluster at a higher
# resolution than the panel, and would that look better?
#
#   sh res_probe.sh show       # read-only: what we advertise vs what the phone chose
#   sh res_probe.sh two        # advertise 2560x960, wait for one reconnect, report, restore
#
# WHY THIS IS A QUESTION AT ALL
#   CarPlay renders the cluster scene at exactly the pixel size we advertise
#   (MIB3_SHIM_W/H, default 1280x480 = the panel).  Upscaling those pixels is
#   pointless -- there is no detail to recover.  But RENDERING AT 2x AND
#   DOWNSAMPLING TO 1x is real anti-aliasing: thin roads, small labels and card
#   edges get supersampled, and it also cancels the few-percent overscan upscale
#   the ECU applies.  The catch: 4x the pixels over a link that already carries
#   the centre screen, and Apple's own rate control may just spend the same
#   bitrate on more pixels, which looks SOFTER.
#
#   So we measure before building anything: advertise 2x with the SAME physical
#   size (292x110 mm) -- which keeps the card's classification identical, because
#   the "wide"/"centred" thresholds are derived from px-per-mm -- then read the
#   size the phone actually declares in its avcC header.  The shim already logs
#   it: `cluster/thread: encoded size w=...`.  If it stays 1280x480, the idea is
#   dead and no decoder work is worth doing.  If it goes 2560x960, the follow-up
#   (SPS-driven V4L2 formats + a box-filter downsample in v4l2_seq) is worth a day.
#
# WHY THE DECODER IS STOPPED FOR THE TEST (do not skip this)
#   v4l2_seq is built around 1280x480 buffers.  Feeding a 2560x960 stream into a
#   decoder configured for 1280x480 is the documented `vidc wedge` class, and that
#   wedged state once reset this unit.  With the controller and decoder stopped and
#   no helper connected, the shim logs `sink: helper absent` and DROPS the frames:
#   we still get the header (which is all we need) and touch no video hardware.
#
# READ-ONLY except for the two knob writes, which are restored before this
# script exits.  No service is touched, nothing under /vw is written, a reboot is
# the full undo.
# ============================================================================
KIT="${MIB3_KIT:-/tmp/kit}"
SHIMLOG="${MIB3_RES_SHIMLOG:-/tmp/mib3shim.log}"
WAIT_S="${MIB3_RES_WAIT:-180}"
WANT_W="${MIB3_RES_W:-2560}"
WANT_H="${MIB3_RES_H:-960}"
BASE_W="${MIB3_RES_BASE_W:-1280}"
BASE_H="${MIB3_RES_BASE_H:-480}"
say(){ printf '%s\n' "$*"; }
die(){ say "ABORT: $*" >&2; exit 1; }

# last non-zero value of a shim-log key
lastnz() { grep -a "$1=" "$SHIMLOG" 2>/dev/null | sed -n "s/.*$1=\([0-9][0-9]*\).*/\1/p" | grep -v '^0$' | tail -n 1; }
lastkv() { grep -a "$1=" "$SHIMLOG" 2>/dev/null | tail -n 1 | sed "s/.*$1=//"; }
setknob() { /usr/bin/python "$KIT/target_env.py" "$(pidof carplay)" "$1=$2" >/dev/null 2>&1; }

# ---------------------------------------------------------------------------
# THE OTHER HALF OF THE RESOLUTION QUESTION (fps lane, 2026-10-20).
# 4x the pixels is only free if the phone's encoder does not pay for it.  The
# decoder now counts the phone's frames honestly (`ausz` = slice AUs = phone
# frames) and its bitstream bytes, so the 1x baseline is one measurement.
#   phone_rate [secs]  ->  "15.4 fps, 3.29 Mbit/s (mean AU 27000 B)"
# The 2x number CANNOT be taken: the decoder has to be stopped for the test (a
# 2560x960 stream into a 1280x480 decoder is the documented vidc wedge), and
# with no helper connected the shim drops the frames -- so we get the header we
# need and no per-AU sizes.  Judge the 2x fps only after it is implemented.
# ---------------------------------------------------------------------------
DECLOG="${MIB3_RES_DECLOG:-/tmp/dec.log}"
ratelast() { grep -a '  RATE ' "$DECLOG" 2>/dev/null | tail -n 1 | sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p"; }
phone_rate() {
  _s="${1:-3}"
  [ -f "$DECLOG" ] || { printf 'no decoder log at %s' "$DECLOG"; return; }
  _a0="$(ratelast ausz)"; _b0="$(ratelast abytes)"
  [ -n "$_a0" ] || { printf 'no RATE counters (decoder predates pin f4d244fa)'; return; }
  sleep "$_s"
  _a1="$(ratelast ausz)"; _b1="$(ratelast abytes)"
  _da=$(( _a1 - _a0 )); _db=$(( _b1 - _b0 ))
  [ "$_da" -ge 0 ] || { printf 'counters rotated during the window'; return; }
  [ "$_da" -gt 0 ] || { printf 'no new phone frames in %ss (idle scene, no session, or a stale log)' "$_s"; return; }
  printf '%s.%s fps, %s.%s Mbit/s (mean AU %s B)' \
    "$(( _da / _s ))" "$(( _da * 10 / _s % 10 ))" \
    "$(( _db * 8 / _s / 1000000 ))" "$(( _db * 8 / _s / 1000 / 10 % 100 ))" \
    "$(( _db / (_da > 0 ? _da : 1) ))"
}

show(){
  _phy="$(lastkv 'cluster widthPhysical')"
  [ -n "$_phy" ] || _phy="292 (shim default -- not logged per-cluster in this build)"
  say "================ cluster resolution ================"
  say "  advertised (last advert): $(lastkv 'cluster widthPixels')x$(lastkv 'cluster heightPixels')  maxFPS=$(lastkv 'cluster maxFPS')"
  say "  physical (mm)           : ${_phy}  <- drives the card classification, not the pixels"
  say "  phone's declared encode : $(lastnz 'encoded size w')x$(lastnz 'encoded size h')   (0/empty = no avcC this session)"
  say "  panel / presenter       : 1280x480 (the ECU composites our frame 1:1 with a small overscan crop)"
  say "  phone rate right now    : $(phone_rate 3)"
  say "                            (this is the number a 2x advert puts at risk: same"
  say "                             bitrate over 4x the pixels is either lower fps or softer)"
  _a="$(lastkv 'cluster widthPixels')"
  if [ -n "$_a" ] && [ "$_a" != "$BASE_W" ]; then
    say ""
    say "  !! the CURRENT session was advertised at ${_a}px -- this session is the experiment."
    say "     The knobs only take effect on a NEW session (anything in /info does)."
  fi
}

two(){
  [ -n "$(pidof carplay)" ] || die "carplay is not running"
  [ -f "$SHIMLOG" ] || die "no shim log at $SHIMLOG -- is the shim injected?"
  say "== 2x probe =="
  say "0. baseline at 1x, before anything changes:"
  say "   phone: $(phone_rate 4)   <- THIS is what a 2x advert must not cost"
  say "1. stopping our own consumers so NOTHING tries to decode a 2x stream"
  sh "$KIT/transition_ctl.sh" stop >/dev/null 2>&1
  [ -n "$(pidof v4l2_seq)" ] && { kill "$(pidof v4l2_seq)" 2>/dev/null; sleep 2; }
  _trp="$(cat /dev/shm/transition.lock/pid 2>/dev/null)"
  say "   controller=$( [ -n "$_trp" ] && kill -0 "$_trp" 2>/dev/null && echo "still running (pid $_trp)" || echo stopped )  decoder=$(pidof v4l2_seq || echo stopped)"
  say "2. advertising ${WANT_W}x${WANT_H} with the same physical size (292x110 mm)"
  setknob MIB3_SHIM_W "$WANT_W"
  setknob MIB3_SHIM_H "$WANT_H"
  _mark="$(wc -l < "$SHIMLOG" 2>/dev/null | tr -d ' ')"
  say ""
  say "   >>> RECONNECT THE PHONE NOW (a new session is required; ~3 s to first frame) <<<"
  say "   waiting up to ${WAIT_S}s for the new advert and the phone's declared size ..."
  _i=0; _gotw=""; _goth=""
  while [ "$_i" -lt "$WAIT_S" ]; do
    _gotw="$(tail -n +$((_mark + 1)) "$SHIMLOG" 2>/dev/null | sed -n 's/.*encoded size w=\([0-9][0-9]*\).*/\1/p' | grep -v '^0$' | tail -n 1)"
    _goth="$(tail -n +$((_mark + 1)) "$SHIMLOG" 2>/dev/null | sed -n 's/.*encoded size h=\([0-9][0-9]*\).*/\1/p' | grep -v '^0$' | tail -n 1)"
    [ -n "$_gotw" ] && [ -n "$_goth" ] && break
    sleep 2; _i=$((_i + 2))
  done
  say ""
  say "3. restoring the knobs to ${BASE_W}x${BASE_H} (this session keeps its size until you reconnect)"
  setknob MIB3_SHIM_W "$BASE_W"
  setknob MIB3_SHIM_H "$BASE_H"
  say ""
  say "================ RESULT ================"
  _adv="$(tail -n +$((_mark + 1)) "$SHIMLOG" 2>/dev/null | grep -a 'cluster widthPixels=' | tail -n 1 | sed 's/.*=//')"
  say "  we advertised  : ${_adv:-?}x$(tail -n +$((_mark + 1)) "$SHIMLOG" 2>/dev/null | grep -a 'cluster heightPixels=' | tail -n 1 | sed 's/.*=//')"
  say "  phone declared : ${_gotw:-none}x${_goth:-none}"
  if [ -z "$_gotw" ]; then
    say ""
    say "  NO MEASUREMENT -- the phone did not open a stream inside the window."
    say "  Either it was not reconnected, or CarPlay refused the bigger display."
    say "  (Check: grep 'ADDING cluster display' and 'cluster rc=' in $SHIMLOG.)"
  elif [ "$_gotw" = "$BASE_W" ] && [ "$_goth" = "$BASE_H" ]; then
    say ""
    say "  VERDICT: the phone IGNORED the bigger advert and still encodes ${BASE_W}x${BASE_H}."
    say "  -> the 2x idea is DEAD on this unit.  Do not spend decoder work on it."
    say "  (A fallback worth one thought only: iOS may cap the cluster stream at the panel"
    say "   size the LG framework reports elsewhere.  That would be a different lane.)"
  else
    say ""
    say "  VERDICT: the phone HONOURED it -- it is encoding ${_gotw}x${_goth}."
    say "  -> supersampling is available.  Next step (needs code, not knobs):"
    say "     1. v4l2_seq: take width/height from the SPS instead of the hardcoded"
    say "        1280x480 (S_FMT, the capture sizeimage, the RGBA buffer, send size);"
    say "     2. box-downsample 2x2 to 1280x480 in send_rgba (4 -> 1 pixels, ~+1 ms/frame);"
    say "     3. keep the presenter, the surface and the ECU exactly as they are;"
    say "     4. judge on the car with the SAME card, one A/B: the win is text and thin"
    say "        roads, and the risk is that Apple's fixed bitrate spends itself on 4x"
    say "        the pixels and looks softer.  fps_probe.sh settles that in 20 s."
    say ""
    say "  NOTE ON THE 2x FRAME RATE (fps lane, 2026-10-20): it cannot be measured"
    say "  during THIS test, because the decoder has to be stopped for it.  The 1x"
    say "  baseline printed at step 0 is the number to compare against later: after"
    say "  supersampling is implemented, fps_probe.sh at 2x vs at 1x is the A/B, and a"
    say "  drop in ausz with the SAME Mbit/s is the encoder spending its budget on"
    say "  pixels -- the outcome that makes this idea not worth it."
  fi
  say ""
  say "NEXT: reconnect the phone once more (it will advertise ${BASE_W}x${BASE_H} again), then:"
  say "      sh /tmp/kit/bring_up.sh all"
}

case "${1:-show}" in
  show) show ;;
  two)  two ;;
  *)    echo "usage: sh res_probe.sh [show|two]" >&2; exit 2 ;;
esac
