#!/bin/sh
# ============================================================================
# fps_probe.sh -- WHO limits the cluster's frame rate?  Measure every candidate
# in one window, side by side, so the answer is a number and not a guess.
#
#   sh fps_probe.sh [seconds]        # default 20
#   sh fps_probe.sh selftest         # desk-only: proves the parsing on fake logs
#
# THE CHAIN HAS THREE RATES, and until 2026-10-20 only two of them were measured:
#
#   1. THE PHONE    -- what Apple encodes for the cluster stream (type 111).  We
#                      advertise maxFPS=30 (MIB3_SHIM_FPS) but Apple decides.
#   2. OUR DECODER  -- what we decode and hand to the presenter.
#   3. THE CAR'S OWN ENCODER (multi-screen-server) -- what the ECU actually
#                      receives.  It re-encodes our surface at the
#                      `streamParameters` framerate (10 for the car's own nav),
#                      so it is a hard ceiling on smoothness no matter what
#                      arrives from the phone.  Read from MS-S's own log.
#
# WHAT WAS WRONG BEFORE (fixed 2026-10-20, decoder pin f4d244fa): the phone's
# rate was inferred from `parse=N`, the count of RECORDS on the AU socket.  The
# shim writes TWO records per phone frame -- the SPS/PPS record it stamps ahead
# of every AU plus the AU itself -- so parse is ~2x frames and parse/frames was
# once misread as a 50% loss.  The decoder now counts `ausz`: AU records that
# CARRY A SLICE, which is exactly one phone frame.  THIS SCRIPT USES ausz.
#
# AND THE THING THAT MAKES THE ANSWER ACTIONABLE: a single mean cannot tell a
# steady 15 fps from a bursty 22 fps, and those need opposite fixes.  So the
# decoder also reports the arrival-gap histogram, the bitstream bytes (a bitrate
# budget is a flat bytes/s while the frame count moves), whether the DECODED
# pictures actually change (a scene nobody is redrawing cannot be made to send
# faster by any advert), and the queue depth it saw at send time (burst vs a
# slow presenter -- two different defects with the same symptom).
#
# Everything here is READ-ONLY: it greps three logs and sleeps.
# ============================================================================
SECS="${1:-20}"
DECLOG="${MIB3_FPS_DECLOG:-/tmp/dec.log}"
SHIMLOG="${MIB3_FPS_SHIMLOG:-/tmp/mib3shim.log}"
MSSLOG="${MIB3_FPS_MSSLOG:-/dev/shm/mss.log}"
EGLLOG="${MIB3_FPS_EGLLOG:-/tmp/egl.log}"

say(){ printf '%s\n' "$*"; }
die(){ say "ABORT: $*" >&2; exit 1; }

# last numeric value of <key>= in the newest STATUS line of the decoder log
num() { grep -a STATUS "$2" 2>/dev/null | tail -n 1 | sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p"; }
# last numeric value of <key>= in the newest RATE line (the 2026-10-20 counters)
rnum(){ grep -a "  RATE " "$2" 2>/dev/null | tail -n 1 | sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p"; }
# the 5-field arrival-gap histogram, "a,b,c,d,e", from the newest RATE line
rgaps(){ grep -a "  RATE " "$1" 2>/dev/null | tail -n 1 | sed -n 's/.* gaps=\([0-9][0-9,]*\).*/\1/p'; }
gapi(){ printf '%s' "$1" | cut -d, -f"$2" 2>/dev/null; }
# last non-zero "encoded size w=" the PHONE declared for this stream
encsize() { grep -a "encoded size $1=" "$2" 2>/dev/null | sed -n "s/.*encoded size $1=\([0-9][0-9]*\)/\1/p" | grep -v '^0$' | tail -n 1; }
fmt10() { printf '%s.%s' "$(( $1 / 10 ))" "$(( $1 % 10 ))"; }
# rate in tenths of a frame per second, from two counter samples
rate10() { _d=$(( $2 - $1 )); [ "$_d" -lt 0 ] && { echo "-1"; return; }; printf '%s' "$(( _d * 10 / $3 ))"; }
# percentage with one decimal: pct <part> <whole>
pct1() { if [ "${2:-0}" -le 0 ]; then printf '  - '; else printf '%s.%s' "$(( $1 * 100 / $2 ))" "$(( $1 * 1000 / $2 % 10 ))"; fi; }
dlt()  { printf '%s' "$(( $2 - $1 ))"; }

selftest() {
  _t="${TMPDIR:-/tmp}/fpsprobe.$$"; mkdir -p "$_t" || exit 1
  printf '  STATUS parse=200 feed=100 qfail=0 capret=100 kdrop=0 qdrop=0 frames=100 nfree=8 alen=0\n' > "$_t/dec.log"
  printf '  RATE ausz=150 abytes=4123456 amax=90000 gaps=1,60,30,6,3 sent=140 drop=10 qh0=130 qh1=8 qh2=2 rgap=9 changed=145 same=5 nframes=150\n' >> "$_t/dec.log"
  printf '[mib3shim] cluster/thread: encoded size w=0\n[mib3shim] cluster/thread: encoded size h=0\n[mib3shim] cluster/thread: encoded size w=2560\n[mib3shim] cluster/thread: encoded size h=960\n[mib3shim] cluster/thread: cluster widthPixels=1280\n[mib3shim] cluster/thread: cluster heightPixels=480\n' > "$_t/shim.log"
  printf '[egl] cadence (fps)=30\n[egl] idle-blank (ms, 0=off)=8000\n' > "$_t/egl.log"
  printf '[MS-S v.1.4] encoder task : avg time = 100.3ms [ 93.8..105.7] cnt =  50 FPS: 10.0 [ 9.5 .. 10.7 ]\n' > "$_t/mss.log"
  _f=0
  [ "$(rnum ausz   "$_t/dec.log")" = "150" ]     || { say "SELFTEST ausz FAIL ($(rnum ausz "$_t/dec.log"))"; _f=1; }
  [ "$(rnum abytes "$_t/dec.log")" = "4123456" ] || { say "SELFTEST abytes FAIL"; _f=1; }
  [ "$(rnum sent   "$_t/dec.log")" = "140" ]     || { say "SELFTEST sent FAIL"; _f=1; }
  [ "$(rnum rgap   "$_t/dec.log")" = "9" ]       || { say "SELFTEST rgap FAIL"; _f=1; }
  _g="$(rgaps "$_t/dec.log")"
  [ "$_g" = "1,60,30,6,3" ] || { say "SELFTEST gaps string FAIL ($_g)"; _f=1; }
  [ "$(gapi "$_g" 1)" = "1" ] && [ "$(gapi "$_g" 2)" = "60" ] && [ "$(gapi "$_g" 5)" = "3" ] \
    || { say "SELFTEST gap fields FAIL"; _f=1; }
  # 150 ausz / 10 s = 15.0 fps ; 140 sent / 10 s = 14.0 fps ; drop 10 of 150 = 6.6 %
  [ "$(fmt10 "$(rate10 0 150 10)")" = "15.0" ] || { say "SELFTEST ausz rate FAIL"; _f=1; }
  [ "$(fmt10 "$(rate10 100 240 10)")" = "14.0" ] || { say "SELFTEST sent rate FAIL"; _f=1; }
  [ "$(pct1 10 150)" = "6.6" ] || { say "SELFTEST pct FAIL ($(pct1 10 150))"; _f=1; }
  [ "$(pct1 0 0)" = "  - " ] || { say "SELFTEST pct-zero FAIL"; _f=1; }
  [ "$(pct1 1 3)" = "33.3" ] || { say "SELFTEST pct-round FAIL ($(pct1 1 3))"; _f=1; }
  # 4123456 B over 10 s = 3298764 bit/s = 3298 kbit/s -> 3.29 Mbit/s
  [ "$(printf '%s.%s' "$(( 4123456 * 8 / 10 / 1000 / 1000 ))" "$(( 4123456 * 8 / 10 / 1000 / 10 % 100 ))")" = "3.29" ] \
    || { say "SELFTEST bitrate FAIL"; _f=1; }
  [ "$(encsize w "$_t/shim.log")" = "2560" ] || { say "SELFTEST encoded-W FAIL"; _f=1; }
  [ "$(num frames "$_t/dec.log")" = "100" ]  || { say "SELFTEST old-counter FAIL"; _f=1; }
  [ "$(rate10 250 100 10)" = "-1" ] || { say "SELFTEST rotation guard FAIL"; _f=1; }
  rm -rf "$_t"
  [ "$_f" = "0" ] && { say "SELFTEST 14/14 PASS"; return 0; }
  say "SELFTEST FAILED"; return 1
}

[ "$SECS" = "selftest" ] && { selftest; exit $?; }

case "$SECS" in ''|*[!0-9]*) die "usage: sh fps_probe.sh [seconds|selftest]";; esac
[ "$SECS" -ge 2 ] || die "window too short to measure (need >= 2 s)"
[ -f "$DECLOG" ] || die "no decoder log at $DECLOG -- is v4l2_seq running?"

HAVE_RATE=0
[ -n "$(rnum ausz "$DECLOG")" ] && HAVE_RATE=1

# ---- sample 1 -------------------------------------------------------------
A0="$(rnum ausz   "$DECLOG")"; B0="$(rnum abytes "$DECLOG")"; S0="$(rnum sent "$DECLOG")"
D0="$(rnum drop   "$DECLOG")"; H00="$(rnum qh0 "$DECLOG")"; H10="$(rnum qh1 "$DECLOG")"
H20="$(rnum qh2    "$DECLOG")"; C0="$(rnum changed "$DECLOG")"; M0="$(rnum same "$DECLOG")"
GA0="$(rgaps "$DECLOG")"
P0="$(num parse    "$DECLOG")"; F0="$(num frames "$DECLOG")"; K0="$(grep -a FRAME "$DECLOG" 2>/dev/null | tail -n 1 | sed -n 's/.*auskip=\([0-9]*\).*/\1/p')"; K0="${K0:-0}"
[ -n "$P0" ] || [ -n "$A0" ] || die "no STATUS/RATE line in $DECLOG yet -- the decoder has not started decoding"

say "sampling ${SECS}s ..."
sleep "$SECS"

# ---- sample 2 -------------------------------------------------------------
A1="$(rnum ausz   "$DECLOG")"; B1="$(rnum abytes "$DECLOG")"; S1="$(rnum sent "$DECLOG")"
D1="$(rnum drop   "$DECLOG")"; H01="$(rnum qh0 "$DECLOG")"; H11="$(rnum qh1 "$DECLOG")"
H21="$(rnum qh2    "$DECLOG")"; C1="$(rnum changed "$DECLOG")"; M1="$(rnum same "$DECLOG")"
GA1="$(rgaps "$DECLOG")"; RG="$(rnum rgap "$DECLOG")"
P1="$(num parse    "$DECLOG")"; F1="$(num frames "$DECLOG")"; K1="$(grep -a FRAME "$DECLOG" 2>/dev/null | tail -n 1 | sed -n 's/.*auskip=\([0-9]*\).*/\1/p')"; K1="${K1:-0}"
[ -n "$P1" ] || die "the decoder produced no STATUS line during the window"

say ""
say "============= cluster frame-rate probe (window ${SECS}s) ============="

# =============================================================================
# 1. THE PHONE -- the only rate we do not control.
# =============================================================================
if [ "$HAVE_RATE" = "1" ] && [ -n "$A1" ]; then
  DA="$(dlt "$A0" "$A1")"
  if [ "$DA" -lt 0 ]; then
    say "  1. THE PHONE   counters went BACKWARDS (the log rotated or the decoder"
    say "                 restarted mid-window).  Rerun against one decoder process."
  else
    RA="$(rate10 "$A0" "$A1" "$SECS")"
    say "  1. THE PHONE  (frames it actually ENCODED: slice AUs, not records)"
    say "       ausz             $A0 -> $A1   = $(fmt10 "$RA") fps"
    if [ -n "$B0" ] && [ -n "$B1" ]; then
      _db=$(( B1 - B0 ))
      if [ "$_db" -gt 0 ]; then
        say "       bitstream        $(( _db * 8 / SECS / 1000000 )).$(( _db * 8 / SECS / 1000 / 10 % 100 )) Mbit/s  \
(mean AU $(( _db / (DA > 0 ? DA : 1) )) B, max $(rnum amax "$DECLOG") B)"
        # a BUDGET shows up as a flat bytes/s while fps moves; CONTENT as fps
        # moving with the scene while bytes/s follows it.
        say "       per-frame budget $(( _db / (DA > 0 ? DA : 1) * 8 / 1000 )) kbit  \
(at $(fmt10 "$RA") fps; a hard budget pins this, not the frame count)"
      fi
    fi
    if [ -n "$GA0" ] && [ -n "$GA1" ]; then
      _g1="$(gapi "$GA0" 1)"; _g2="$(gapi "$GA0" 2)"; _g3="$(gapi "$GA0" 3)"; _g4="$(gapi "$GA0" 4)"; _g5="$(gapi "$GA0" 5)"
      _h1="$(gapi "$GA1" 1)"; _h2="$(gapi "$GA1" 2)"; _h3="$(gapi "$GA1" 3)"; _h4="$(gapi "$GA1" 4)"; _h5="$(gapi "$GA1" 5)"
      _n1=$(( ${_h1:-0} - ${_g1:-0} )); _n2=$(( ${_h2:-0} - ${_g2:-0} ))
      _n3=$(( ${_h3:-0} - ${_g3:-0} )); _n4=$(( ${_h4:-0} - ${_g4:-0} ))
      _n5=$(( ${_h5:-0} - ${_g5:-0} ))
      _nt=$(( _n1 + _n2 + _n3 + _n4 + _n5 ))
      say "       arrival gaps     <20ms $(pct1 "$_n1" "$_nt")% | 20-40 $(pct1 "$_n2" "$_nt")% | \
40-80 $(pct1 "$_n3" "$_nt")% | 80-160 $(pct1 "$_n4" "$_nt")% | 160+ $(pct1 "$_n5" "$_nt")%"
      say "                        (a clean clock puts everything in ONE bucket; the same"
      say "                         mean spread over three is a burst, and a burst needs"
      say "                         queue depth, not a faster send)"
    fi
    _dc=$(( ${C1:-0} - ${C0:-0} )); _dm=$(( ${M1:-0} - ${M0:-0} ))
    _dt=$(( _dc + _dm ))
    say "       picture changed  $(pct1 "$_dc" "$_dt")% new / $(pct1 "$_dm" "$_dt")% repeat   \
(a high REPEAT share means the scene is not being redrawn: content, not a cap)"
  fi
else
  say "  1. THE PHONE   NO RATE INSTRUMENTATION in this decoder log."
  say "                 That decoder predates pin f4d244fa.  Its only phone counter is"
  say "                 parse= (AU RECORDS, ~2 per frame, so parse/2 is a framing"
  say "                 artefact).  Push the current decoder, or read the old numbers"
  say "                 below and treat them as indicative only."
fi

# =============================================================================
# 2. OURS -- decode, then what we actually hand to the presenter.
# =============================================================================
RF="$(rate10 "$F0" "$F1" "$SECS")"
say ""
if [ "$RF" = "-1" ]; then
  say "  2. OURS        frames counter rotated during the window -- rerun."
else
  say "  2. OURS        decoded frames   $F0 -> $F1   = $(fmt10 "$RF") fps"
fi
if [ "$HAVE_RATE" = "1" ] && [ -n "$S1" ]; then
  DS=$(( S1 - S0 )); DD=$(( D1 - D0 ))
  RS="$(rate10 "$S0" "$S1" "$SECS")"
  say "                 handed to the presenter   $S0 -> $S1 = $(fmt10 "$RS") fps   \
dropped $DD of $(( DS + DD )) = $(pct1 "$DD" $(( DS + DD )))%"
  _k0=$(( ${H01:-0} - ${H00:-0} )); _k1=$(( ${H11:-0} - ${H10:-0} )); _k2=$(( ${H21:-0} - ${H20:-0} ))
  say "                 queue depth at send    empty $_k0 | ~1 frame $_k1 | >1 frame $_k2"
  say "                        (a ~1-frame depth is a BURST -- two frames decoded inside"
  say "                         one presenter tick.  A >1-frame depth is the presenter"
  say "                         genuinely behind.  V4L2_RGBA_QLIM=2 covers the first,"
  say "                         and only that one is worth covering.  The buckets use the"
  say "                         same 1/8-frame slack as the policy, because SIOCOUTQ"
  say "                         counts skb overhead: one queued frame reads ~1.6 % high.)"
  say "                 our longest gap between AU-socket reads   ${RG:-?} ms"
  if [ -n "$RG" ] && [ "$RG" -ge 67 ]; then
    say "                 !! >= 67 ms: the PHONE drops frames once its own outgoing queue"
    say "                    holds 67 ms of media (vdsink_ShouldDropFrame).  Our stall is"
    say "                    being charged to the phone.  Fix our read cadence first."
  fi
fi
say "                 (qdrop $(num qdrop "$DECLOG") auskip $K0 -> $K1: both must stay FLAT)"
say "                 parse          $P0 -> $P1 AU RECORDS  <- NOT a frame count"

# =============================================================================
# 3. THE CAR'S OWN ENCODER -- the real ceiling on what the ECU can show.
# =============================================================================
say ""
say "  3. THE CAR'S ENCODER (what the ECU actually receives):"
if [ -s "$MSSLOG" ]; then
  # the oracle grows ~8 MB/min; never sed the whole file on the unit (busybox sed
  # over a 100 MB log blocks for minutes -- measured 2026-10-20).
  tail -n 4000 "$MSSLOG" 2>/dev/null \
    | sed -n 's/.*encoder task.*FPS: \([0-9.]*\).*/       multi-screen-server encode FPS = \1 (from the oracle)/p' \
    | tail -n 1
else
  say "       (no oracle log at $MSSLOG -- start it: sh deliver_ctl.sh oracle start)"
fi

say ""
say "  what we advertised to it     : $(grep -a 'cluster widthPixels=' "$SHIMLOG" 2>/dev/null | tail -n 1 | sed 's/.*=//')x$(grep -a 'cluster heightPixels=' "$SHIMLOG" 2>/dev/null | tail -n 1 | sed 's/.*=//') @ $(grep -a 'cluster maxFPS=' "$SHIMLOG" 2>/dev/null | tail -n 1 | sed 's/.*=//') fps  (a reconnect is needed after changing it)"
say "  phone's declared encode size : $(encsize w "$SHIMLOG")x$(encsize h "$SHIMLOG")   (0/empty = no avcC seen yet)"
say "  presenter cadence            : $(grep -a 'cadence (fps)=' "$EGLLOG" 2>/dev/null | tail -n 1 | sed 's/.*=//') fps, idle-blank $(grep -a 'idle-blank (ms' "$EGLLOG" 2>/dev/null | tail -n 1 | sed 's/.*=//') ms"

say ""
say "READ IT LIKE THIS:"
say "  * qdrop/auskip FLAT            -> the bitstream is intact; nothing of ours is lost"
say "  * (1) ~15 with changed ~100%   -> the phone is encoding as fast as its scene changes."
say "                                    Try the distinguishing tests in LANE_FPS_2026-10-20.md"
say "                                    (maxFPS=5 authority test, motion window, thermal)."
say "  * (1) low with changed low     -> the SCENE is static.  No advert can raise this."
say "  * flat Mbit/s while (1) moves  -> a bitrate budget, not a frame-rate cap."
say "  * (3) << (2)                   -> the CAR's encoder is the ceiling; stream_fps.sh is"
say "                                    the only lever, and no upstream work helps."
say "  * (2) < (1) with qh1/qh2 > 0   -> OUR loss; land of V4L2_RGBA_QLIM."
say "  * (2) < (1) with qh0 alone     -> the presenter drains slower than the phone supplies."
