#!/bin/bash
# ============================================================================
# run_viewarea.sh -- THE VIEW-AREA BENCH (2026-10-19).
#
# Proves, on the desk, the three things the view-area lane ships:
#
#   A. the /info ADVERT shape:
#        default (MIB3_SHIM_VIEWAREAS unset) -> ONE area, and NONE of the new
#        shape lines at all (the "unchanged by default" contract);
#        MIB3_SHIM_VIEWAREAS=1 -> N areas, area 0 full panel, areas >0 carrying a
#        different NESTED safeArea with the full visible frame (the rgi-proven
#        shape), and MIB3_SHIM_VA_VISIBLE=1 as the deliberate alternative.
#
#   B. the COMMAND shape, captured back from the real dictionary by
#      libstubcmd.so (the stub that caught the missing `type` key):
#        {type:"updateViewArea",
#         params:{uuid, viewAreaIndex, animationDurationMillis,
#                 adjacentViewAreas}}   <-- adjacentViewAreas ALWAYS present:
#        [] for index 0, [0] for any index > 0.
#
#   C. the GATES and the COMPLETION:
#        knob off / VA_CMD=0 / out-of-range index -> ZERO commands shipped;
#        a status-0 completion is logged and moves g_va_current;
#        a non-zero completion is logged as REFUSED and does NOT move it.
#
# What a green does NOT mean: that the phone accepts the switch, that the gauge
# geometry is right, and that anything on the head unit tells us the layout
# changed.  All three are car-only (see LANE_CLUSTER_VIEWAREA_2026-10-19.md).
#
# Usage:  bash run_viewarea.sh        (VA_SKIP_CONTAINER=1 skips the container parts)
# ============================================================================
set -u
export LC_ALL=C LANG=C
HERE="$(cd "$(dirname "$0")" && pwd)"
SHIMDIR="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$SHIMDIR/../.." && pwd)"
ROOT="$REPO/R8830L_ROOTFS"
CC=/opt/homebrew/opt/llvm/bin/clang
LLD=/opt/homebrew/opt/lld/bin/ld.lld
PASS=0; FAIL=0

chk(){ # chk NAME expected actual
  if [ "$2" = "$3" ]; then echo "VA $1 exp=$2 got=$3 PASS"; PASS=$((PASS+1));
  else echo "VA $1 exp=$2 got=$3 FAIL"; FAIL=$((FAIL+1)); fi
}
chkg(){ # chkg NAME pattern string
  if printf '%s' "$3" | grep -q "$2"; then echo "VA $1 exp=/.../ got=match PASS"; PASS=$((PASS+1));
  else echo "VA $1 exp=/.../ got=nomatch FAIL"; FAIL=$((FAIL+1)); fi
}

echo "############ PART 0: source self-checks (no run needed) ############"
SRC="$(cat "$SHIMDIR/mib3shim.c")"
chkg ships_updateViewArea 'updateViewArea' "$SRC"
chkg ships_adjacent_key 'adjacentViewAreas' "$SRC"
chkg ships_viewarea_knob 'MIB3_SHIM_VIEWAREAS' "$SRC"
chkg ships_cmdack_knob 'MIB3_SHIM_CMDACK' "$SRC"
chkg ships_appearance 'mapAppearanceUpdate' "$SRC"
chkg ships_uiappearance 'uiAppearanceUpdate' "$SRC"
chkg ships_opcode_naming 'cluster/ctl/meaning' "$SRC"
# the EXISTING receiver log lines must be untouched in meaning
for ln in 'cluster/ctl","opcode' 'cluster/ctl","bodySize' 'cluster/ctl/hdr-head' \
          'cluster/ctl/body-head' 'hdr read ended' 'other-opcodes' 'viewAreas added' \
          'FIRST IDR at frame'; do
  chkg "existing_log_$(echo "$ln" | tr -cd 'a-z' | cut -c1-18)" "$(printf '%s' "$ln" | sed 's/[][\\.*^$]/\\&/g')" "$SRC"
done
# the default advert path must still be the ORIGINAL branch
chkg default_is_else_branch 'env_on("MIB3_SHIM_VIEWAREAS",0)' "$SRC"
# the new completion must be switchable off, and off must be the OLD call
chkg cmdack_ablatable 'MIB3_SHIM_CMDACK",1' "$SRC"
chkg cmdack_old_call 'p_SendCommand(session, cmd, 0, 0)' "$SRC"

if [ "${VA_SKIP_CONTAINER:-0}" = "1" ] || ! command -v container >/dev/null 2>&1; then
  echo "VA container parts SKIPPED"
  echo "TOTAL: PASS=$PASS FAIL=$FAIL"
  [ "$FAIL" -eq 0 ]
  exit $?
fi

echo
echo "############ PART 0b: the one-shot control tool (host build, --dry) ############"
D="$(mktemp -d "${TMPDIR:-/tmp}/va.XXXXXX")"
CTLBIN="$D/mib3ctl_host"
$CC -O1 -DHOST_TEST -Wall -Wextra -Wno-unused-parameter -o "$CTLBIN" "$SHIMDIR/tools/mib3ctl.c" \
  2>"$D/ctl.err" && chk ctl_host_build 0 0 || { chk ctl_host_build 0 1; sed -n 1,12p "$D/ctl.err"; }
dry(){ "$CTLBIN" --dry "$@" 2>/dev/null | sed -n 's/^DRY line=//p'; }
chk ctl_view_idx "$(dry view 1)" "v1"
chk ctl_view_next "$(dry view next)" "next"
chk ctl_bare_next "$(dry next)" "next"
chk ctl_prev "$(dry prev)" "prev"
chk ctl_zoom_out "$(dry zoom out)" "-1"
chk ctl_zoom_in "$(dry zoom in)" "+1"
chk ctl_appear "$(dry appear map 1 2)" "a map 1 2"
chk ctl_raw_line "$(dry line v3)" "v3"
rm -rf "$D" 2>/dev/null

echo
echo "############ PART 0c: the /info advert, assembled and read back ############"
# The advert modes above only reach the DISPLAY seam.  This part drives BOTH
# seams over a synthetic /info dict -- AirPlayInfoArrayAddScreenDisplay (which
# appends and annotates the cluster entry) and then mib3shim_complete_info()
# (which adds uid/initialVideoStreams/displayPanels/extendedFeatures) -- and
# READS THE RESULT BACK, so "internally consistent at both 1x and 2x" is a
# measured number, not a log string.  The driver is GENERATED here and built
# into a temp dir that is bind-mounted into the container: nothing is added to
# the shared tree (three lanes work in it).
D2="$(mktemp -d "${TMPDIR:-/tmp}/vainfo.XXXXXX")"
cat > "$D2/driver_info.c" <<'CEOF'
/* generated by run_viewarea.sh -- builds a synthetic /info dict the way the unit
 * does (the display seam, then the /info seam) and lets the SHIM judge it.
 *
 * WHY SO LITTLE CF HERE.  The assertion logic lives in the shim (info_check),
 * because that is the only CF binding set proven on the unit: the desk's
 * libCoreUtils `CFDictionaryGetInt64` turned out NOT to be value-returning the
 * way the shim's resolved symbol is, and a driver that reads the dict itself
 * segfaulted on exactly that call.  So the driver only BUILDS, and the verdict
 * comes back as `infocheck` log lines from code that runs on the car too. */
typedef unsigned long u64;
extern void *dlsym(void *, const char *);
extern int   write(int, const void *, unsigned long);
extern void  _exit(int);
#define RTLD_DEFAULT ((void *)0)
typedef long         CFIndex;
typedef void        *CFMutableArrayRef;
typedef void        *CFMutableDictionaryRef;
typedef const void  *CFStringRef;
typedef unsigned int CFStringEncoding;
#define KUTF8 0x08000100u
typedef CFStringRef (*fn_str)(void *, const char *, CFStringEncoding);
typedef CFMutableArrayRef (*fn_arrnew)(void *, CFIndex, const void *);
typedef CFMutableDictionaryRef (*fn_dictnew)(void *, CFIndex, const void *, const void *);
typedef void        (*fn_dictset)(CFMutableDictionaryRef, const void *, const void *);
extern int AirPlayInfoArrayAddScreenDisplay(void *inArray, void *uuid, int features,
                                            int pid, int fps, int w, int h, int pw, int ph);
static void P(const char *s){ u64 n = 0; while (s[n]) n++; write(1, s, n); }
static void Pn(const char *tag, long v){
  char b[96]; int k = 0; char t[16]; int j = 0; u64 u;
  b[k++] = '['; while (*tag) b[k++] = *tag++; b[k++] = '=';
  u = (v < 0) ? (u64)(-v) : (u64)v; if (v < 0) b[k++] = '-';
  if (!u) t[j++] = '0'; while (u){ t[j++] = (char)('0' + (u % 10)); u /= 10; }
  while (j) b[k++] = t[--j];
  b[k++] = ']'; b[k++] = '\n'; write(1, b, (u64)k);
}
void _start(void)
{
  fn_str     S  = (fn_str)     dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
  fn_arrnew  AN = (fn_arrnew)  dlsym(RTLD_DEFAULT, "CFArrayCreateMutable");
  fn_dictnew DN = (fn_dictnew) dlsym(RTLD_DEFAULT, "CFDictionaryCreateMutable");
  fn_dictset DS = (fn_dictset) dlsym(RTLD_DEFAULT, "CFDictionarySetValue");
  int (*cmp)(void *) = (int (*)(void *))dlsym(RTLD_DEFAULT, "mib3shim_complete_info");
  int (*chk)(void *) = (int (*)(void *))dlsym(RTLD_DEFAULT, "mib3shim_info_check");
  const void *KCB = (const void *)dlsym(RTLD_DEFAULT, "kCFLDictionaryKeyCallBacksCFLTypes");
  const void *VCB = (const void *)dlsym(RTLD_DEFAULT, "kCFLDictionaryValueCallBacksCFLTypes");
  CFMutableArrayRef displays = 0;
  CFMutableDictionaryRef info;
  if (!S || !AN || !DN || !DS){ P("[drvinfo] missing CF\n"); _exit(2); }
  /* 1) the display seam: appends the main display, then the annotated cluster */
  { CFStringRef mu = S(0, "9d44494a-a25e-47c9-9326-e592666b255a", KUTF8);
    Pn("add rc", AirPlayInfoArrayAddScreenDisplay(&displays, (void *)mu, 8, 1, 60, 1264, 626, 206, 102)); }
  if (!displays){ P("[drvinfo] no displays array\n"); _exit(2); }
  /* 2) the /info seam, then the self-check */
  info = DN(0, 0, KCB, VCB);
  if (!info){ P("[drvinfo] no dict\n"); _exit(2); }
  DS(info, S(0, "displays", KUTF8), displays);
  Pn("complete rc", cmp ? cmp((void *)info) : -1);
  Pn("check rc", chk ? chk((void *)info) : -1);
  P("[drvinfo] done\n");
  _exit(0);
}
CEOF
$CC --target=aarch64-linux-gnu -O2 -nostdlib -fPIE --ld-path=$LLD -pie -o "$D2/driver_info" \
    "$D2/driver_info.c" -L"$ROOT/lge/app_rw/lib64" -Wl,--no-as-needed -lAirPlay \
    "$ROOT/lib64/libdl-2.23.so" "$ROOT/lib64/libc.so.6" -Wl,-e,_start 2>"$D2/info.err" \
    && chk info_driver_build 0 0 || { chk info_driver_build 0 1; sed -n 1,20p "$D2/info.err"; }

echo
echo "############ PART 0b: the shim pin agrees in all FOUR places ############"
# A stale pin here has aborted a staging run at the worst moment before.  The
# binary, run_inject's WANT_MD5, push_kit's SHIM_WANT and STICK_ADDONS must agree.
md5of(){ if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'; else md5 -q "$1"; fi; }
BIN_PIN="$(md5of "$SHIMDIR/mib3shim.so" 2>/dev/null)"
INJ_PIN="$(grep -oE '^WANT_MD5=[0-9a-f]+' "$REPO/MIB3_RE/shim/inject/run_inject.sh" | cut -d= -f2)"
PK_PIN="$(grep -oE '^SHIM_WANT=[0-9a-f]+' "$REPO/MIB3_RE/shim/live_kit/push_kit.sh" | cut -d= -f2)"
SA_PIN="$(grep -oE 'md5 [0-9a-f]{32}' "$REPO/MIB3_RE/shim/live_kit/STICK_ADDONS.txt" | head -n 1 | awk '{print $2}')"
chk pin_binary_nonempty 1 "$(if [ -n "$BIN_PIN" ]; then echo 1; else echo 0; fi)"
chk pin_inject_eq_binary "$BIN_PIN" "$INJ_PIN"
chk pin_pushkit_eq_binary "$BIN_PIN" "$PK_PIN"
chk pin_stickaddons_eq_binary "$BIN_PIN" "$SA_PIN"

echo "############ build ############"
[ -f "$SHIMDIR/mib3shim.so" ] || sh "$SHIMDIR/build.sh" >/dev/null 2>&1
$CC --target=aarch64-linux-gnu -O2 -fPIC -fvisibility=default -nostdlib \
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -fuse-ld=$LLD -shared \
    -o "$HERE/libstubcmd.so" "$HERE/libstubcmd.c" -L"$ROOT/lge/app_rw/lib64" -lCoreUtils \
    "$ROOT/lib64/libc.so.6" || chk stubcmd_build 0 1
$CC --target=aarch64-linux-gnu -O2 -nostdlib -fPIE -fuse-ld=$LLD -pie -o "$HERE/driver_viewarea" \
    "$HERE/driver_viewarea.c" -L"$ROOT/lge/app_rw/lib64" -Wl,--no-as-needed -lAirPlay \
    "$ROOT/lib64/libdl-2.23.so" "$ROOT/lib64/libc.so.6" -Wl,-e,_start || chk drvva_build 0 1

# one container run, every mode, labelled sections
RUNNER='
LD=/mnt/rootfs/lib64/ld-2.23.so
LP=/mnt/rootfs/lge/app_rw/lib64:/mnt/rootfs/lge/app_ro/lib64:/mnt/rootfs/lib64:/mnt/rootfs/usr/lib64:/mnt/rootfs/lib:/mnt/rootfs/usr/lib
SHIM=/work/MIB3_RE/shim/mib3shim.so
STUB=/work/MIB3_RE/shim/harness/libstubcmd.so
D=/work/MIB3_RE/shim/harness
one(){ L="$1"; M="$2"; shift 2
  rm -f /tmp/mib3shim.log /tmp/zoomcmd.log
  env -i VA_TEST_MODE="$M" LD_PRELOAD="$SHIM $STUB" MIB3_SHIM_GATE=1 MIB3_SHIM_ADD=1 "$@" \
     $LD --library-path $LP $D/driver_viewarea >/dev/null 2>&1
  echo "===CMDLOD $L"
  cat /tmp/zoomcmd.log 2>/dev/null
  echo "===SHIMLOG $L"
  cat /tmp/mib3shim.log 2>/dev/null
}
one advert_off advert_off
one advert_on  advert_on
one advert_vis advert_on MIB3_SHIM_VA_VISIBLE=1
# explicit VA1 below both cliffs -> CarPlay layout 0 = blank cluster
one advert_blank advert_on MIB3_SHIM_VA1=0,0,1280,480,600,240,200,200
# ---- THE SCALE LADDER (2026-10-20, the 2x lane) -------------------------
# 1.5x and 2x must produce an advert of the SAME shape: every safe box inside
# the frame, and the SAME class sequence, because the classifier cliffs are a
# fraction of the frame while the advertised mm is unchanged.
one advert_15x advert_on MIB3_SHIM_SCALE=150
one advert_2x  advert_on MIB3_SHIM_SCALE=200
# the density-preserving variant (pixels AND mm scaled): the knob for the
# RATIO hypothesis.  Its consequence is real and is pinned: the card becomes
# WIDE instead of centred, because the panel is now declared bigger in points.
one advert_2xphy advert_on MIB3_SHIM_SCALE=200 MIB3_SHIM_SCALE_PHY=1
# an EXPLICIT box that leaves the frame: the clamp must fire and pull it inside
one advert_oob advert_on MIB3_SHIM_SCALE=200 MIB3_SHIM_VA1=0,0,2560,960,878,900,794,750
one set0 set0
one set1 set1
one bounds bounds
one off off
one nopcmd nopcmd
one ctl ctl
one next next
one appear appear
one refused set1 STUBCMD_ACK_STATUS=7
one noack noack
# ---- BOTH SEAMS, READ BACK (the /info dict itself) ----------------------
info(){ L="$1"; shift
  rm -f /tmp/mib3shim.log
  echo "===INFO $L"
  env -i LD_PRELOAD="$SHIM" MIB3_SHIM_GATE=1 MIB3_SHIM_ADD=1 MIB3_SHIM_VIEWAREAS=1 \
     MIB3_SHIM_VA_COUNT=3 "$@" $LD --library-path $LP /drv/driver_info 2>&1
  echo "===SHIMLOG $L"
  cat /tmp/mib3shim.log 2>/dev/null
}
info info_1x
info info_15x MIB3_SHIM_SCALE=150
info info_2x  MIB3_SHIM_SCALE=200
info info_2xphy MIB3_SHIM_SCALE=200 MIB3_SHIM_SCALE_PHY=1
'
R=$(container run --rm \
      --mount type=bind,source="$ROOT",target=/mnt/rootfs,readonly \
      --mount type=bind,source="$REPO",target=/work,readonly \
      --mount type=bind,source="$D2",target=/drv,readonly \
      ubuntu:24.04 /bin/bash -c "$RUNNER" 2>&1)
echo "$R" | sed 's/^/    /'

seg(){ printf '%s' "$R" | awk -v k=CMDLOD -v l="$1" '$0=="==="k" "l{f=1;next} /^===/{f=0} f'; }
slog(){ printf '%s' "$R" | awk -v k=SHIMLOG -v l="$1" '$0=="==="k" "l{f=1;next} /^===/{f=0} f'; }
inf(){ printf '%s' "$R" | awk -v k=INFO -v l="$1" '$0=="==="k" "l{f=1;next} /^===/{f=0} f'; }
ival(){ printf '%s' "$2" | tr -d '\r' | grep -F "[$1=" | tail -n 1 | sed 's/.*=//; s/\]$//'; }
# kval: the SHIM's own "tag: key=value" log lines
kval(){ printf '%s' "$2" | tr -d '\r' | grep -F "$1=" | tail -n 1 | sed 's/.*=//'; }

echo
echo "############ PART A: the /info advert ############"
AOFF=$(slog advert_off); AON=$(slog advert_on); AVIS=$(slog advert_vis)
chkg advert_off_added 'viewAreas added' "$AOFF"
chk  advert_off_no_newarea 0 "$(printf '%s' "$AOFF" | grep -c 'cluster/va/rect')"
chk  advert_off_no_count   0 "$(printf '%s' "$AOFF" | grep -c 'count declared')"
chkg advert_on_count 'cluster/va: count declared=3' "$AON"
chk  advert_on_rects 3 "$(printf '%s' "$AON" | grep -c 'cluster/va/rect')"
# the preset derives the boxes from CarPlay's own classifier thresholds
chkg advert_on_area0_wide 'cluster/va/rect: visible=0,0,1280,480 safe=240,100,800,280' "$AON"
chkg advert_on_area1_centred 'cluster/va/rect: visible=0,0,1280,480 safe=439,50,397,375' "$AON"
chk  advert_on_class_wide    2 "$(printf '%s' "$AON" | grep -c 'class (0=BLANK 1=centred 2=wide)=2')"
chk  advert_on_class_centred 1 "$(printf '%s' "$AON" | grep -c 'class (0=BLANK 1=centred 2=wide)=1')"
chk  advert_on_class_blank   0 "$(printf '%s' "$AON" | grep -c 'class (0=BLANK 1=centred 2=wide)=0')"
chk  advert_on_eta_ok        3 "$(printf '%s' "$AON" | grep -c 'etaOk(safeH>202)=1')"
chk  advert_on_no_cliff      0 "$(printf '%s' "$AON" | grep -c 'nearCliff(<=10%)=1')"
# which area a session STARTS in: default 1 (the centred card + ETA, owner's
# choice, verified on the car 2026-10-20), clamped to the declared count
chk  advert_on_initial_default 1 "$(printf '%s' "$AON" | grep -c 'initialViewArea=1')"
chkg advert_on_knobs 'VIEWAREAS=1' "$AON"
# VA_VISIBLE=1 shrinks the VISIBLE frame of areas > 0 too (the rgi-refuted shape)
chkg advert_vis_visible_shrunk 'cluster/va/rect: visible=439,50,397,375 safe=439,50,397,375' "$AVIS"
# a box below BOTH cliffs is CarPlay layout 0: the cluster draws NOTHING (no
# graceful degradation), so the bench pins the classification of that shape too.
ABLK=$(slog advert_blank)
chk  advert_blank_class_blank 1 "$(printf '%s' "$ABLK" | grep -c 'class (0=BLANK 1=centred 2=wide)=0')"
# only area 1 is below the cliffs in this advert -> 2 of 3 still carry an ETA
chk  advert_blank_eta_others 2 "$(printf '%s' "$ABLK" | grep -c 'etaOk(safeH>202)=1')"

echo
echo "############ PART A2: the advert at 1x, 1.5x and 2x ############"
# What this pins (2026-10-20): the SHAPE of the advert must be identical at every
# pixel size -- every safe box INSIDE the frame, and the same card classes --
# because that is what makes a 2x advert acceptable to the phone.  The build this
# replaced shipped a safe area that ended 95 px BELOW the frame at 2x (and 45 px
# at 1.5x), which is the class of defect that costs the whole CarPlay session.
A1X=$(slog advert_on); A15=$(slog advert_15x); A2X=$(slog advert_2x); A2P=$(slog advert_2xphy)
AOOB=$(slog advert_oob)

# --- 1x: unchanged, and now provably in-frame ---
chk  ad1x_safefit 3 "$(printf '%s' "$A1X" | grep -c 'safefit(1=inside)=1')"
chk  ad1x_clamped 1 "$(printf '%s' "$A1X" | grep -c 'out-of-frame boxes clamped=0')"
chkg ad1x_classes 'cluster/va/classes: 2,1,2' "$A1X"
chkg ad1x_phys 'cluster widthPhysical=292' "$A1X"
chkg ad1x_density 'cluster px per mm [(]milli[)]=4383' "$A1X"

# --- 1.5x ---
chkg ad15_dims 'cluster widthPixels=1920' "$A15"
chkg ad15_dims_h 'cluster heightPixels=720' "$A15"
chkg ad15_area1 'safe=658,75,595,562' "$A15"
chk  ad15_safefit 3 "$(printf '%s' "$A15" | grep -c 'safefit(1=inside)=1')"
chk  ad15_clamped 1 "$(printf '%s' "$A15" | grep -c 'out-of-frame boxes clamped=0')"
chkg ad15_classes 'cluster/va/classes: 2,1,2' "$A15"
chkg ad15_density 'cluster px per mm [(]milli[)]=6575' "$A15"

# --- 2x (the supersampled stream, real panel size) ---
chkg ad2x_dims 'cluster widthPixels=2560' "$A2X"
chkg ad2x_dims_h 'cluster heightPixels=960' "$A2X"
chkg ad2x_area0 'safe=480,200,1600,560' "$A2X"
chkg ad2x_area1 'safe=878,100,794,750' "$A2X"
chkg ad2x_area2 'safe=128,182,1203,595' "$A2X"
chk  ad2x_safefit 3 "$(printf '%s' "$A2X" | grep -c 'safefit(1=inside)=1')"
chk  ad2x_clamped 1 "$(printf '%s' "$A2X" | grep -c 'out-of-frame boxes clamped=0')"
chkg ad2x_phys 'cluster widthPhysical=292' "$A2X"
chkg ad2x_density 'cluster px per mm [(]milli[)]=8767' "$A2X"
chkg ad2x_eta  'etaOk[(]safeH>202[)]=1' "$A2X"
chk  ad2x_nocliff 0 "$(printf '%s' "$A2X" | grep -c 'nearCliff(<=10%)=1')"
# THE INVARIANT: same class sequence as 1x, i.e. the card the owner tuned
chkg ad2x_classes 'cluster/va/classes: 2,1,2' "$A2X"

# --- 2x with the physical size scaled too (the density/ratio hypothesis) ---
chkg ad2p_phys 'cluster widthPhysical=584' "$A2P"
chkg ad2p_phys_h 'cluster heightPhysical=219' "$A2P"
chkg ad2p_density 'cluster px per mm [(]milli[)]=4383' "$A2P"
chk  ad2p_safefit 3 "$(printf '%s' "$A2P" | grep -c 'safefit(1=inside)=1')"
chk  ad2p_clamped 1 "$(printf '%s' "$A2P" | grep -c 'out-of-frame boxes clamped=0')"
# consequences, pinned so nobody is surprised: the same box in a bigger panel is
# in the WIDE class, not the centred one
chkg ad2p_classes 'cluster/va/classes: 2,2,2' "$A2P"

# --- an explicit box that leaves the frame must be CLAMPED, not shipped ---
chkg aoob_clamped 'out-of-frame boxes clamped=1' "$AOOB"
chkg aoob_warn 'WARN clamped' "$AOOB"
chkg aoob_pulled_back 'visible=0,0,2560,960 safe=878,900,794,60' "$AOOB"
chk  aoob_safefit 3 "$(printf '%s' "$AOOB" | grep -c 'safefit(1=inside)=1')"

echo
echo "############ PART A3: BOTH seams, and the shim's own verdict ############"
# The same invariants, judged by `info_check()` INSIDE the shim -- the code that
# also runs on the car on every /info -- against the real dictionary built by the
# two seams (displays + viewAreas + initialVideoStreams + displayPanels +
# extendedFeatures).  `VERDICT mismatch=0` is the whole point: a single malformed
# display entry costs the entire CarPlay session, and that is what the 2x
# experiment could not see.
for LBL in info_1x info_15x info_2x info_2xphy; do
  I=$(inf $LBL)
  S=$(slog $LBL)
  case $LBL in
    info_1x)    EW=1280; EH=480;  EPW=292; EPH=110 ;;
    info_15x)   EW=1920; EH=720;  EPW=292; EPH=110 ;;
    info_2x)    EW=2560; EH=960;  EPW=292; EPH=110 ;;
    info_2xphy) EW=2560; EH=960;  EPW=584; EPH=219 ;;
  esac
  chk  "${LBL}_complete" 1 "$(ival 'complete rc' "$I")"
  chk  "${LBL}_check_rc" 1 "$(ival 'check rc' "$I")"
  # the verdict, straight from the shim
  chk  "${LBL}_verdict"      0 "$(kval 'VERDICT mismatch' "$S")"
  chk  "${LBL}_displays"     2 "$(kval 'displays' "$S")"
  chk  "${LBL}_cluster_type" 111 "$(kval 'cluster type' "$S")"
  chk  "${LBL}_w"           "$EW" "$(kval 'cluster w' "$S")"
  chk  "${LBL}_h"           "$EH" "$(kval 'cluster h' "$S")"
  chk  "${LBL}_wphys"      "$EPW" "$(kval 'cluster wPhys' "$S")"
  chk  "${LBL}_hphys"      "$EPH" "$(kval 'cluster hPhys' "$S")"
  # initialVideoStreams: present AND non-empty (an empty array gave a 0 s session)
  chk  "${LBL}_ivs"            1 "$(kval 'cluster initialVideoStreams' "$S")"
  # every declared view area, safe box INSIDE the frame; start index in range
  chk  "${LBL}_viewareas"      3 "$(kval 'viewAreas declared' "$S")"
  chk  "${LBL}_va_inside"      3 "$(kval 'viewAreas inside frame' "$S")"
  chk  "${LBL}_initial"        1 "$(kval 'initialViewArea' "$S")"
  # displayPanels: one panel per display, SAME pixel and physical geometry, and
  # the cluster panel carries showsInstruments
  chk  "${LBL}_panels"         2 "$(kval 'panels' "$S")"
  chk  "${LBL}_panel_size"     2 "$(kval 'panels size match' "$S")"
  chk  "${LBL}_panel_phys"     2 "$(kval 'panels phys match' "$S")"
  chk  "${LBL}_panel_marks"    1 "$(kval 'panels showsInstruments' "$S")"
  # the feature advertisement and the display-count invariant, both ways
  chk  "${LBL}_alt"            1 "$(kval 'extFeatures altScreen' "$S")"
  chk  "${LBL}_va_feat"        1 "$(kval 'extFeatures viewAreas' "$S")"
done

echo
echo "############ PART B: the exact updateViewArea shape ############"
S0=$(seg set0); S1=$(seg set1); CTL=$(seg ctl); NXT=$(seg next)
chk set0_one_send 1 "$(printf '%s' "$S0" | grep -c '^type=updateViewArea$')"
chkg set0_uuid '^params.uuid=C1057E12-A0D1-4E20-B0C0-1057E12A0D10$' "$S0"
chkg set0_index0 '^params.viewAreaIndex=num:0$' "$S0"
chkg set0_duration '^params.animationDurationMillis=num:250$' "$S0"
chkg set0_adjacent_array '^params.adjacentViewAreas=array\[0\]$' "$S0"
chk  set0_no_elements 0 "$(printf '%s' "$S0" | grep -c '^params.adjacentViewAreas\[')"
chk set1_one_send 1 "$(printf '%s' "$S1" | grep -c '^type=updateViewArea$')"
chkg set1_index1 '^params.viewAreaIndex=num:1$' "$S1"
chkg set1_adjacent_one '^params.adjacentViewAreas=array\[1\]$' "$S1"
chkg set1_adjacent_zero '^params.adjacentViewAreas\[0\]=num:0$' "$S1"
# the inbox grammar reaches the same shipped shape
chk  ctl_one_send 1 "$(printf '%s' "$CTL" | grep -c '^type=updateViewArea$')"
chkg ctl_index2 '^params.viewAreaIndex=num:2$' "$CTL"
chk  next_one_send 1 "$(printf '%s' "$NXT" | grep -c '^type=updateViewArea$')"
chkg next_index1 '^params.viewAreaIndex=num:1$' "$NXT"
chkg next_adjacent '\[0\]=num:0$' "$NXT"

echo
echo "############ PART C: the gates ############"
BND=$(seg bounds); OFF=$(seg off); NOP=$(seg nopcmd)
chk  bounds_no_send 0 "$(printf '%s' "$BND" | grep -c '^SEND #')"
chkg bounds_refused_locally 'cluster/va/cmd: refused locally' "$(slog bounds)"
chkg bounds_oor 'index out of range' "$(slog bounds)"
chk  off_no_send 0 "$(printf '%s' "$OFF" | grep -c '^SEND #')"
chkg off_logged 'skipped [(]MIB3_SHIM_VIEWAREAS=0[)]' "$(slog off)"
chk  nopcmd_no_send 0 "$(printf '%s' "$NOP" | grep -c '^SEND #')"
chkg nopcmd_logged 'skipped [(]MIB3_SHIM_VA_CMD=0[)]' "$(slog nopcmd)"

echo
echo "############ PART C2: the completion callback ############"
chkg ack_called 'ack=yes' "$S1"
chkg ack_status0 'cluster/cmd/ack: status [(]0=accepted[)]=0' "$(slog set1)"
chkg ack_type 'cluster/cmd/ack/type: updateViewArea' "$(slog set1)"
chkg ack_applied 'cluster/va/applied: index=1' "$(slog set1)"
# knob off: the callback is NOT passed at all (the old, blind SendCommand call)
chkg cmdack_off_no_callback '^SEND #1 shape=dict ack=no$' "$(seg noack)"
REF=$(slog refused)
chkg refused_status 'cluster/cmd/ack: status [(]0=accepted[)]=7' "$REF"
chkg refused_logged 'cluster/cmd/ack/refused: updateViewArea' "$REF"
chkg refused_va 'cluster/va/refused: index=1' "$REF"
chk  refused_no_applied 0 "$(printf '%s' "$REF" | grep -c 'cluster/va/applied')"

echo
echo "############ PART D: item 3 -- appearance ############"
AP=$(seg appear)
chk  appear_one 1 "$(printf '%s' "$AP" | grep -c '^type=mapAppearanceUpdate$')"
chkg appear_mode '^params.appearanceMode=num:1$' "$AP"
chkg appear_setting '^params.appearanceSetting=num:2$' "$AP"
chkg appear_uuid '^params.uuid=C1057E12-A0D1-4E20-B0C0-1057E12A0D10$' "$AP"

echo
echo "TOTAL: PASS=$PASS FAIL=$FAIL"
rm -rf "$D2" 2>/dev/null
[ "$FAIL" -eq 0 ]
