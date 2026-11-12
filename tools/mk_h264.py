#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""mk_h264.py -- emit a *valid* Annex-B H.264 baseline stream for the desk bench.

Every access unit is SPS + PPS + an IDR I-slice whose macroblocks are all I_PCM
(raw pixel samples).  I_PCM is the one intra mode whose payload is unambiguously
correct if it is the right size, so the file is genuinely decodable H.264 (not
random bytes with a NAL header) -- the container proof then rests on a real
bitstream, not a stub.

usage: mk_h264.py [nAUs] [width] [height] > out.264
"""
import sys

class BW:
    def __init__(self): self.bits = []
    def u(self, n, v):
        for i in range(n - 1, -1, -1): self.bits.append((v >> i) & 1)
    def ue(self, v):
        v += 1
        n = v.bit_length() - 1
        self.bits += [0] * n
        self.u(n + 1, v)
    def se(self, v): self.ue(-2 * v if v <= 0 else 2 * v - 1)
    def rbsp_trailing(self):
        self.bits.append(1)
        while len(self.bits) % 8: self.bits.append(0)
    def bytes(self):
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            b = 0
            for bit in self.bits[i:i + 8]: b = (b << 1) | bit
            out.append(b)
        return bytes(out)

def epb(rbsp):
    """insert emulation-prevention bytes"""
    out = bytearray(); z = 0
    for b in rbsp:
        if z >= 2 and b <= 3:
            out.append(3); z = 0
        out.append(b)
        z = z + 1 if b == 0 else 0
    return bytes(out)

def nal(hdr, rbsp):
    return b"\x00\x00\x00\x01" + bytes([hdr]) + epb(rbsp)

def make_sps(w, h):
    bw = BW()
    bw.u(8, 66); bw.u(8, 0xC0); bw.u(8, 30)   # baseline, level 3.0
    bw.ue(0)                                   # sps_id
    bw.ue(0)                                   # log2_max_frame_num_minus4 -> 16
    bw.ue(2)                                   # pic_order_cnt_type = 2
    bw.ue(1)                                   # max_num_ref_frames
    bw.u(1, 0)                                 # gaps_in_frame_num
    bw.ue(w // 16 - 1)
    bw.ue(h // 16 - 1)
    bw.u(1, 1)                                 # frame_mbs_only_flag
    bw.u(1, 1)                                 # direct_8x8_inference
    bw.u(1, 0)                                 # frame_cropping
    bw.u(1, 0)                                 # vui
    bw.rbsp_trailing()
    return nal(0x67, bw.bytes())

def make_pps():
    bw = BW()
    bw.ue(0); bw.ue(0)                         # pps_id, sps_id
    bw.u(1, 0)                                 # entropy_coding_mode (CAVLC)
    bw.u(1, 0)                                 # bottom_field_pic_order
    bw.ue(0)                                   # num_slice_groups_minus1
    bw.ue(0); bw.ue(0)                         # num_ref_idx defaults
    bw.u(1, 0); bw.u(2, 0)                     # weighted_pred, weighted_bipred
    bw.se(0); bw.se(0); bw.se(0)               # qp offsets
    bw.u(1, 1)                                 # deblocking_filter_control_present
    bw.u(1, 0); bw.u(1, 0)                     # constrained_intra, redundant_pic
    bw.rbsp_trailing()
    return nal(0x68, bw.bytes())

def make_idr(w, h, luma):
    bw = BW()
    bw.ue(0)          # first_mb_in_slice
    bw.ue(2)          # slice_type = I
    bw.ue(0)          # pps_id
    bw.u(4, 0)        # frame_num (log2_max_frame_num = 4)
    bw.ue(0)          # idr_pic_id
    bw.u(1, 0)        # dec_ref_pic_marking: no_output_of_prior_pics_flag   (IDR, nal_ref_idc!=0)
    bw.u(1, 0)        # dec_ref_pic_marking: long_term_reference_flag
    bw.se(0)          # slice_qp_delta
    bw.ue(1)          # disable_deblocking_filter_idc = 1 (disabled)
    mbw, mbh = w // 16, h // 16
    for my in range(mbh):
        for mx in range(mbw):
            bw.ue(25)                                   # I_PCM
            while len(bw.bits) % 8: bw.bits.append(0)   # pcm_alignment
            tl = (mx * 16, my * 16)                     # a moving gradient
            for y in range(16):
                for x in range(16):
                    bw.u(8, (tl[0] + x) & 0xFF)
            for _ in range(2 * 64):                     # chroma (u+v), neutral-ish
                bw.u(8, (tl[1]) & 0xFF)
    bw.rbsp_trailing()
    return nal(0x65, bw.bytes())

def make_idr_i16(w, h):
    """IDR I-slice using I_16x16 intra prediction with DC mode and NO residual.

    I_PCM (above) is legal H.264 but many hardware decoders do not implement it.
    I_16x16 is universal.  mb_type 1..24 = 1 + intra16x16PredMode(0..3)
    + 4*CodedBlockPatternChroma(0..2) + 12*CodedBlockPatternLuma(0..1);
    with no residual the coded-block-pattern is 0 for both -> mb_type = 1+predMode.
    DC prediction with no available neighbours reconstructs to 128 (mid grey),
    so the decoded frame is a clean uniform grey -- unmistakable against 'no data'.
    """
    bw = BW()
    bw.ue(0)          # first_mb_in_slice
    bw.ue(2)          # slice_type = I
    bw.ue(0)          # pps_id
    bw.u(4, 0)        # frame_num
    bw.ue(0)          # idr_pic_id
    bw.u(1, 0)        # dec_ref_pic_marking: no_output_of_prior_pics_flag   (IDR, nal_ref_idc!=0)
    bw.u(1, 0)        # dec_ref_pic_marking: long_term_reference_flag
    bw.se(0)          # slice_qp_delta
    bw.ue(1)          # disable_deblocking_filter_idc
    mbw, mbh = w // 16, h // 16
    for _ in range(mbh * mbw):
        bw.ue(1 + 2)  # mb_type: I_16x16, intra16x16PredMode=2 (DC), CBP 0/0
        bw.ue(0)      # intra_chroma_pred_mode = DC
        bw.se(0)      # mb_qp_delta (ALWAYS present for I_16x16)
        bw.u(1, 1)    # Intra16x16DCLevel coeff_token: TotalCoeff=0, nC<2 -> '1'
        # (CBP luma/chroma are 0: no AC blocks, no chroma residual)
    bw.rbsp_trailing()
    return nal(0x65, bw.bytes())

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 320
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 240
    mode = sys.argv[4] if len(sys.argv) > 4 else "pcm"
    out = sys.stdout.buffer
    sps, pps = make_sps(w, h), make_pps()
    for i in range(n):
        out.write(sps)
        out.write(pps)
        out.write(make_idr_i16(w, h) if mode == "i16" else make_idr(w, h, i))
    out.flush()

if __name__ == "__main__":
    main()
