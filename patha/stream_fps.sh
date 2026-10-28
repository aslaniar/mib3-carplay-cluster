#!/bin/sh
# ============================================================================
# stream_fps.sh -- read (and, deliberately, MAYBE change) the frame rate at which
# multi-screen-server encodes our surface for the cluster ECU.
#
#   sh stream_fps.sh show             # READ-ONLY: everything MS-S could be using
#   sh stream_fps.sh set N            # snapshot, post framerate=N, VERIFY by read-back
#   sh stream_fps.sh restore          # post the snapshot back, verify
#
# WHAT THIS IS FOR
#   The cluster does not get Apple's H.264.  It gets OUR surface, re-encoded by the
#   car's own multi-screen-server at the `framerate` recorded in the
#   displaymanagement `streamParameters` element (10 for the car's own nav), and
#   that is the rate the ECU sees.  If the phone sends 30 and MS-S encodes 10, the
#   extra frames are discarded by the car and the cluster cannot be made smoother
#   by anything upstream.  fps_probe.sh tells you whether that is happening; THIS
#   tool is the only lever that could change it.
#
# WHY IT IS WRITTEN SO CAREFULLY (read this before running `set`)
#   It writes to the LIVE display RSI.  That area has form: improvising on it once
#   restarted multi-screen-server and rsi-display-management, and element ids
#   changed (HANDOFF_DELIVERY_2026-10-16 S3).  So:
#     * `show` touches nothing.
#     * `set` snapshots the element VERBATIM to /dev/shm/streamparams.bak.json
#       (and to the stick if one is mounted) BEFORE any write;
#     * it then re-reads the element and tells you whether the value actually
#       changed -- the RSI's update verb for this collection is not documented in
#       the image, so the tool tries the element URI and then the collection and
#       reports which one took (a silent no-op is the failure mode we must not
#       hide);
#     * it never triggers a resync itself: that is the step that can bounce the
#       cluster, and you should do it knowingly (see the advice it prints).
#
# KNOWN GOOD FALLBACK: `restore` puts the exact original JSON back.  A reboot is
# the full undo regardless (RAM-only element state lives in rsi-display-management).
# ============================================================================
RSI="${MIB3_DLV_RSI:-127.0.0.1}"
DMP="${MIB3_DLV_DMP:-49600}"
OURNAME="${MIB3_SF_OURNAME:-test}"
BAK="${MIB3_SF_BAK:-/dev/shm/streamparams.bak.json}"
CMD="${1:-show}"
ARG="${2:-}"

command -v python >/dev/null 2>&1 || { echo "no python on this unit (expected /usr/bin/python)"; exit 2; }

python - "$CMD" "$ARG" "$RSI" "$DMP" "$OURNAME" "$BAK" <<'PY'
import sys, json, os, time, io

cmd, arg, RSI, DMP, OURNAME, BAK = sys.argv[1:7]
BASE = "http://%s:%s/displaymanagement" % (RSI, DMP)

try:                                  # py3
    from urllib.request import Request, urlopen
    from urllib.error import HTTPError
except ImportError:                   # py2.7 (the unit)
    from urllib2 import Request, urlopen, HTTPError

def http(method, path, obj=None):
    url = BASE + path
    data = json.dumps(obj).encode("utf-8") if obj is not None else None
    req = Request(url, data=data)
    # py2's urllib2.Request has no `method=` kwarg; overriding get_method works on
    # both (py3 shadows the bound method with an instance attribute).
    req.get_method = lambda: method
    if data: req.add_header("Content-Type", "application/json")
    try:
        r = urlopen(req, timeout=8)
        body = r.read()
        return r.getcode(), body
    except HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return 0, str(e).encode("utf-8", "replace") if not isinstance(e, bytes) else e

def get_json(path):
    code, body = http("GET", path)
    if code != 200: return None, "HTTP %s: %s" % (code, body[:200])
    try:    return json.loads(body), None
    except Exception as e: return None, "not JSON (%s): %s" % (e, body[:200])

def elements(path):
    d, err = get_json(path)
    if d is None: return [], err
    if isinstance(d, dict): return d.get("data", []) or [], None
    return d, None

def find(l, name):
    for e in l:
        if e.get("name") == name: return e
    return None

def our_streamparam():
    """the streamParameters URI our own element points at (string on the unit, an
    object in the mock RSI -- normalise both)"""
    els, err = elements("/renderingSurfaces")
    if err: return None, err
    ours = find(els, OURNAME)
    if not ours: return None, "our element '%s' is not registered (delivery not attached?)" % OURNAME
    sp = ours.get("streamParameters")
    if isinstance(sp, dict):
        sp = sp.get("uri") or ("/displaymanagement/streamParameters/" + str(sp.get("id")))
    if not sp: return None, "our element has no streamParameters"
    return sp.replace("\\/", "/"), None


def our_streamparam_or_only():
    """our element's streamParameters, or -- when our element does not exist yet
    (a reset, or before the first share) -- the image's ONLY streamParameters.
    Without this fallback, `set` refuses precisely when it is safest to run: with
    nothing on screen and no live stream to disturb (measured 2026-10-20)."""
    uri, err = our_streamparam()
    if uri:
        return uri, err
    sps, err2 = elements("/streamParameters")
    if sps:
        s0 = sps[0]
        u = s0.get("uri") or ("/displaymanagement/streamParameters/" + str(s0.get("id")))
        return u.replace("\\/", "/"), (err or "")
    return None, err or err2

def show():
    print("== renderingSurfaces ==")
    els, err = elements("/renderingSurfaces")
    if err: print("  " + err)
    for e in els:
        print("  %-34s internalIdentifier=%-6s streamParameters=%s" % (
            e.get("name", "?"), str(e.get("internalIdentifier")), e.get("streamParameters", "")))
    print("== streamParameters ==")
    sps, err = elements("/streamParameters")
    if err: print("  " + err)
    for s in sps:
        print("  %-32s framerate=%-4s ip=%-42s port=%-6s uri=%s" % (
            s.get("name", "?"), s.get("framerate"), s.get("ip"), s.get("port"), s.get("uri", "")))
    sp_uri, err = our_streamparam()
    print("== our element points at ==")
    print("  %s%s" % (sp_uri or "(none)", ("   <- " + err) if err else ""))
    print("== what multi-screen-server says it is using ==")
    shown = False
    for p in ("/dev/shm/mss.log", "/tmp/USB/PORT1/PART1/mss.log"):
        if not os.path.exists(p): continue
        hits = []
        try:
            for line in io.open(p, "r", errors="replace"):
                if "framerate=" in line and "surfaceId" in line:
                    hits.append(line.rstrip()[-160:])
        except Exception:
            pass
        for h in hits[-4:]:
            print("  " + h)
        if hits: shown = True
        break
    if not shown:
        print("  (no oracle log -- start it: sh deliver_ctl.sh oracle start)")
    print("== the car's own encoder rate ==")
    for p in ("/dev/shm/mss.log",):
        if os.path.exists(p):
            last = None
            try:
                for line in io.open(p, "r", errors="replace"):
                    if "encoder task" in line and "FPS:" in line: last = line.strip()
            except Exception:
                pass
            print("  " + (last or "(none yet)"))

def snapshot(sp):
    try:
        f = open(BAK, "w")
        json.dump(sp, f)
        f.close()
        print("  snapshot -> %s  (%s)" % (BAK, json.dumps(sp)))
    except Exception as e:
        print("  SNAPSHOT FAILED (%s) -- refusing to write" % e)
        return False
    return True

def post_element(sp, target):
    """try the element URI first, then the collection; report which one changed it"""
    body = dict(sp)
    for path in ("/streamParameters/" + str(sp.get("id")), "/streamParameters"):
        code, resp = http("POST", path, body)
        print("  POST %-52s -> HTTP %s %s" % (path, code, resp[:120]))
        time.sleep(1.0)
        got, err = elements("/streamParameters")
        if err: continue
        cur = None
        for s in got:
            if s.get("id") == sp.get("id") or s.get("name") == sp.get("name"): cur = s
        if cur is None: continue
        if str(cur.get("framerate")) == str(body.get("framerate")):
            print("  VERIFIED: %s now reads framerate=%s (via %s)" % (cur.get("name"), cur.get("framerate"), path))
            return True
    print("  NOT VERIFIED: the collection still does not show framerate=%s." % body.get("framerate"))
    print("  Do not retry blindly -- read `sh stream_fps.sh show` and report what it says.")
    return False

def set_fps(n):
    try: n = int(n)
    except Exception:
        print("usage: sh stream_fps.sh set <N>   (N > 0; MS-S requires framerate > 0)"); return 1
    if n < 1 or n > 60:
        print("refusing framerate=%d (MS-S requires > 0; the ECU/nav path is built for 10)" % n); return 1
    sp_uri, err = our_streamparam_or_only()
    print("== target ==")
    print("  our element -> streamParameters %s%s" % (sp_uri or "(none)", ("  (" + err + ")") if err else ""))
    if not sp_uri:
        return 1
    sps, err = elements("/streamParameters")
    if err: print("  " + err); return 1
    target = None
    for s in sps:
        s_uri = str(s.get("uri", "")).replace("\\/", "/")
        if s_uri == sp_uri or (s.get("id") and sp_uri.endswith(str(s.get("id")))):
            target = s
    if target is None:
        print("  could not resolve %s in the collection" % sp_uri); return 1
    print("  current: %s" % json.dumps(target))
    if str(target.get("framerate")) == str(n):
        print("  already %d -- nothing to do" % n); return 0
    if not snapshot(target): return 1
    body = dict(target)
    body["framerate"] = n
    print("== writing framerate=%d ==" % n)
    ok = post_element(body, sp_uri)
    print("")
    print("NEXT (only if VERIFIED above):")
    print("  * MS-S reads this when it builds a stream, so a resync may be needed:")
    print("      sh deliver_ctl.sh handover arm      (it resyncs and waits on the oracle)")
    print("  * then watch the ECU rate, not ours:   sh fps_probe.sh 20")
    print("  * if the cluster stutters or the picture breaks, undo immediately:")
    print("      sh stream_fps.sh restore")
    return 0 if ok else 1

def restore():
    if not os.path.exists(BAK):
        print("no snapshot at %s -- nothing to restore" % BAK); return 1
    try:
        sp = json.load(open(BAK))
    except Exception as e:
        print("cannot read the snapshot: %s" % e); return 1
    print("== restoring ==")
    print("  %s" % json.dumps(sp))
    ok = post_element(sp, None)
    print("")
    print("  done.  A resync (`sh deliver_ctl.sh handover arm`) makes MS-S pick it up.")
    return 0 if ok else 1

if cmd == "show":
    show(); sys.exit(0)
elif cmd == "set":
    sys.exit(set_fps(arg))
elif cmd == "restore":
    sys.exit(restore())
else:
    print("usage: sh stream_fps.sh [show|set N|restore]"); sys.exit(2)
PY
