#!/usr/bin/env python
# ============================================================================
# h264ipcm.py -- a *deliberately small* Annex-B H.264 -> raw RGBA decoder.
#
# SCOPE (read this): it decodes EXACTLY the stream shape tools/mk_h264.py emits --
# H.264 **Baseline**, CAVLC, pic_order_cnt_type 2, and every macroblock coded as
# **I_PCM** (raw samples in the bitstream).  That is the only H.264 this repo can
# generate without a hardware codec, so it is the only H.264 the DESK can decode
# without a vendor library.  It is NOT a general H.264 decoder: on non-I_PCM input
# it stops with an explicit error.
#
# WHY it exists: it is the DECODE stage of the decode-to-texture pipeline whose
# DELIVERY stage is tools/mib3egl.c (`--rgbafile` / `--rgba-sock`).  Together they
# are an end-to-end "H.264 bytes -> RGBA -> EGL texture -> committed frame on ivi
# surface 3000" chain, executable entirely on the desk.  On the UNIT the decode is
# done by the vendor decoder (Cinemo/OMX/nvdec); the presenter consumes the same
# RGBA stream over --rgba-sock -- see LANE_CLUSTER_DELIVERY_E2E_2026-10-16.md.
#
# OUTPUT (both are w*h*4, row-major, RGBA, byte order R,G,B,A):
#   --out FILE    write every frame, concatenated (frame k at offset k*w*h*4)
#   --sock PATH   connect to the presenter's AF_UNIX server and stream frames
#                 back-to-back; --loop repeats; --fps paces (default: as fast as
#                 the socket accepts)
#   --info        print frame count / dims / sha256 of frame 0 (for the harness)
#
# Python 2.7 AND 3 compatible (the unit only has python2.7).
# ============================================================================
import hashlib
import os
import socket
import struct
import sys
import time

try:
    xrange
except NameError:                                   # py3
    xrange = range


# ---- bit reader -----------------------------------------------------------
class BR(object):
    def __init__(self, data):
        self.d = data
        self.pos = 0                                    # in BITS
    def u(self, n):
        v = 0
        for _ in xrange(n):
            byte = self.d[self.pos >> 3]
            bit = (byte >> (7 - (self.pos & 7))) & 1
            v = (v << 1) | bit
            self.pos += 1
        return v
    def ue(self):
        z = 0
        while self.u(1) == 0:
            z += 1
            if z > 32:
                raise ValueError("ue too long")
        if z == 0:
            return 0
        return (1 << z) - 1 + self.u(z)
    def se(self):
        k = self.ue()
        if k & 1:
            return (k + 1) // 2
        return -(k // 2)
    def align(self):
        while self.pos & 7:
            self.pos += 1
    def bytepos(self):
        return self.pos >> 3


# ---- NAL handling ---------------------------------------------------------
def split_nals(data):
    """Yield (nal_type, rbsp_bytes) for each Annex-B NAL in data."""
    starts = []
    i = 0
    n = len(data)
    while i + 3 <= n:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            starts.append((i, 3))
            i += 3
        elif i + 4 <= n and data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 0 and data[i + 3] == 1:
            starts.append((i, 4))
            i += 4
        else:
            i += 1
    for idx, (off, sc) in enumerate(starts):
        end = starts[idx + 1][0] if idx + 1 < len(starts) else n
        body = data[off + sc:end]
        if not body:
            continue
        yield (body[0] & 0x1f), body[1:]


def unescape(rbsp):
    """Remove emulation-prevention bytes (00 00 03 -> 00 00)."""
    out = bytearray()
    zeros = 0
    for b in bytearray(rbsp):
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


# ---- SPS / PPS ------------------------------------------------------------
def parse_sps(rbsp):
    br = BR(rbsp)
    profile = br.u(8)
    br.u(8)                                     # constraint flags
    br.u(8)                                     # level_idc
    sps_id = br.ue()
    log2_max_frame_num = br.ue() + 4
    poc_type = br.ue()
    if poc_type == 0:
        br.ue()                                 # log2_max_pic_order_cnt_lsb_minus4
    elif poc_type == 1:
        br.u(1)
        br.se(); br.se()
        for _ in xrange(br.ue()):
            br.se()
    max_ref = br.ue()
    br.u(1)                                     # gaps_in_frame_num_value_allowed
    w_mbs = br.ue() + 1
    h_mu = br.ue() + 1
    frame_mbs_only = br.u(1)
    if not frame_mbs_only:
        br.u(1)
    br.u(1)                                     # direct_8x8_inference_flag
    cropping = br.u(1)
    w = w_mbs * 16
    h = h_mu * 16 * (2 - frame_mbs_only)
    if cropping:
        cl = br.ue(); cr = br.ue(); ct = br.ue(); cb = br.ue()
        w -= (cl + cr) * 2
        h -= (ct + cb) * 2
    return dict(profile=profile, sps_id=sps_id, log2_max_frame_num=log2_max_frame_num,
                poc_type=poc_type, w=w, h=h, max_ref=max_ref)


def parse_pps(rbsp):
    br = BR(rbsp)
    pps_id = br.ue()
    sps_id = br.ue()
    cabac = br.u(1)                             # entropy_coding_mode_flag (0 = CAVLC)
    br.u(1)                                     # bottom_field_pic_order_in_frame_present
    num_slice_groups = br.ue() + 1
    if num_slice_groups > 1:
        raise ValueError("slice groups unsupported")
    return dict(pps_id=pps_id, sps_id=sps_id, cabac=cabac)


# ---- IDR / I_PCM slice ----------------------------------------------------
def yuv420_to_rgba(y, u, v, w, h):
    """BT.601 limited-range YUV420 -> RGBA (so the desk test shows a real image)."""
    out = bytearray(w * h * 4)
    cw = w // 2
    for j in xrange(h):
        cj = (j // 2) * cw
        row = j * w
        for i in xrange(w):
            yy = y[row + i]
            ci = cj + (i // 2)
            uu = u[ci]
            vv = v[ci]
            c = yy - 16
            if c < 0:
                c = 0
            d = uu - 128
            e = vv - 128
            r = (298 * c + 409 * e + 128) >> 8
            g = (298 * c - 100 * d - 208 * e + 128) >> 8
            b = (298 * c + 516 * d + 128) >> 8
            o = (row + i) * 4
            out[o] = 255 if r > 255 else (0 if r < 0 else r)
            out[o + 1] = 255 if g > 255 else (0 if g < 0 else g)
            out[o + 2] = 255 if b > 255 else (0 if b < 0 else b)
            out[o + 3] = 255
    return bytes(out)


def decode_idr(rbsp, sps, pps):
    if pps.get("cabac", 1):
        raise ValueError("CABAC unsupported (this stream must be CAVLC)")
    br = BR(rbsp)
    br.ue()                                     # first_mb_in_slice
    slice_type = br.ue()
    br.ue()                                     # pps_id
    br.u(sps["log2_max_frame_num"])             # frame_num
    br.ue()                                     # idr_pic_id
    if sps["poc_type"] == 0:
        br.ue()                                 # pic_order_cnt_lsb
    br.se()                                     # slice_qp_delta
    br.ue()                                     # disable_deblocking_filter_idc
    # slice data: every MB must be I_PCM (mb_type 25) -- that is this tool's contract
    mbw = sps["w"] // 16
    mbh = sps["h"] // 16
    Y = bytearray(sps["w"] * sps["h"])
    U = bytearray((sps["w"] // 2) * (sps["h"] // 2))
    V = bytearray((sps["w"] // 2) * (sps["h"] // 2))
    for my in xrange(mbh):
        for mx in xrange(mbw):
            mbt = br.ue()
            if mbt != 25:
                raise ValueError("non-I_PCM macroblock (%d) -- h264ipcm only decodes "
                                 "the mk_h264.py stream shape" % mbt)
            br.align()
            base = br.bytepos()
            pcm = rbsp[base:base + 256 + 128]
            if len(pcm) < 384:
                raise ValueError("truncated I_PCM samples")
            br.pos += 384 * 8
            luma = pcm[0:256]
            chroma = pcm[256:384]
            ox = mx * 16
            oy = my * 16
            for r in xrange(16):
                s = r * 16
                o = (oy + r) * sps["w"] + ox
                Y[o:o + 16] = luma[s:s + 16]
            cu = chroma[0:64]
            cv = chroma[64:128]
            cox = mx * 8
            coy = my * 8
            ch = sps["w"] // 2
            for r in xrange(8):
                o = (coy + r) * ch + cox
                U[o:o + 8] = cu[r * 8:r * 8 + 8]
                V[o:o + 8] = cv[r * 8:r * 8 + 8]
    return yuv420_to_rgba(Y, U, V, sps["w"], sps["h"])


def decode_file(path):
    data = open(path, "rb").read()
    sps = None
    pps = None
    frames = []
    for ntype, body in split_nals(data):
        rbsp = unescape(body)
        if ntype == 7:
            sps = parse_sps(rbsp)
        elif ntype == 8:
            pps = parse_pps(rbsp)
        elif ntype == 5:
            if not sps or not pps:
                raise ValueError("IDR before SPS/PPS")
            frames.append(decode_idr(rbsp, sps, pps))
        # SPS-extension / SEI / filler are ignored
    return sps, frames


# ---- output ---------------------------------------------------------------
def connect_sock(path, tries=50):
    for _ in range(tries):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except Exception:
            time.sleep(0.1)
    raise SystemExit("h264ipcm: cannot connect to %s" % path)


def main(argv):
    path = None
    out = None
    sock = None
    loop = False
    info = False
    fps = 0
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--out":
            i += 1; out = argv[i]
        elif a == "--sock":
            i += 1; sock = argv[i]
        elif a == "--loop":
            loop = True
        elif a == "--info":
            info = True
        elif a == "--fps":
            i += 1; fps = int(argv[i])
        else:
            path = a
        i += 1
    if not path:
        raise SystemExit("usage: h264ipcm.py <in.264> [--out F | --sock S] [--loop] [--info] [--fps N]")

    sps, frames = decode_file(path)
    if info or not (out or sock):
        sys.stderr.write("[h264ipcm] dims=%dx%d frames=%d sha256(f0)=%s\n" % (
            sps["w"], sps["h"], len(frames),
            hashlib.sha256(frames[0]).hexdigest() if frames else "-"))
    if not frames:
        raise SystemExit("h264ipcm: no IDR frames decoded")

    if out:
        f = open(out, "wb")
        for fr in frames:
            f.write(fr)
        f.close()
        sys.stderr.write("[h264ipcm] wrote %d frames (%d bytes) -> %s\n" %
                         (len(frames), len(frames) * len(frames[0]), out))
    if sock:
        s = connect_sock(sock)
        sys.stderr.write("[h264ipcm] streaming %d frames -> %s\n" % (len(frames), sock))
        sent = 0
        while True:
            for fr in frames:
                s.sendall(fr)
                sent += 1
                if fps > 0:
                    time.sleep(1.0 / fps)
            sys.stderr.write("[h264ipcm] sent=%d\n" % sent)
            if not loop:
                break
        try:
            s.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
