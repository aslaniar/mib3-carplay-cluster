#!/usr/bin/env python3
"""sps_dump.py -- print EVERY field of an H.264 SPS, including VUI, so two streams
can be diffed field by field.

WHY (2026-10-27, field attempt 2 §8.3).  `MIB3_RE/tools/sps_check.py` answers one
question (profile / chroma / bit depth / dimensions) and stops before the VUI.  That
was the right tool for the decode wall in October, but the remaining Path B
question is a different one: is there ANY parameter in the phone's sequence header
that the unit's own encoder never emits?  The three candidates named in the field
record are

    log2_max_frame_num_minus4      (frame_num field width)
    max_num_ref_frames             (DPB size the decoder must allocate)
    max_dec_frame_buffering        (VUI-constrained DPB size)
    max_num_reorder_frames         (VUI-constrained reorder depth)
    vui_parameters.timing_info     (fixed_frame_rate_flag, time_scale, num_units_in_tick)
    pic_struct_present_flag / bitstream_restriction_flag

A hardware decoder is sized for the stream it was built to receive; a stream that
asks for a bigger DPB or a reorder depth of zero-with-B-frames can be refused or
mis-latched.  That is cheap to check and it has never been checked, so check it.

Usage:
    sps_dump.py FILE.264 [...]      # first SPS in each file
    sps_dump.py --sel N FILE        # the N-th SPS (0-based)
    sps_dump.py --json FILE         # machine diffable

Exit 0 = at least one SPS parsed, 1 = none found, 2 = bad usage.
"""
import json
import sys

HIGH = (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135)


def find_sps_list(d):
    """Every SPS payload in an Annex-B / AVCC-ish blob, in order."""
    out, i, n = [], 0, len(d)
    while i + 4 < n:
        if d[i] == 0 and d[i + 1] == 0 and d[i + 2] == 1:
            h = i + 3
            if (d[h] & 0x1F) == 7:
                j = h + 1
                while j + 3 <= n:
                    if d[j] == 0 and d[j + 1] == 0 and (d[j + 2] == 1 or
                                                        (d[j + 2] == 0 and d[j + 3] == 1)):
                        break
                    j += 1
                out.append(d[h + 1:j])
                i = j
                continue
            i = h + 1
        else:
            i += 1
    return out


class Reader(object):
    """Bit reader.  Handles rbsp_trailing_bits by refusing to read past the end."""

    def __init__(self, payload):
        # strip emulation_prevention_three_byte
        rb, zero = bytearray(), 0
        for b in bytearray(payload):
            if zero >= 2 and b == 3:
                zero = 0
                continue
            rb.append(b)
            zero = zero + 1 if b == 0 else 0
        self.b = "".join("{:08b}".format(x) for x in rb)
        self.p = 0

    def u(self, n):
        if self.p + n > len(self.b):
            raise EOFError("bitstream exhausted")
        v = int(self.b[self.p:self.p + n] or "0", 2)
        self.p += n
        return v

    def ue(self):
        z = 0
        while self.p < len(self.b) and self.b[self.p] == "0":
            z += 1
            self.p += 1
        if self.p >= len(self.b):
            raise EOFError("bitstream exhausted in ue()")
        self.p += 1
        v = (1 << z) - 1
        if z:
            v += int(self.b[self.p:self.p + z], 2)
            self.p += z
        return v

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if (k & 1) else -(k // 2)


def parse(payload):
    r = Reader(payload)
    s = {}
    s["profile_idc"] = r.u(8)
    s["constraint_flags"] = r.u(8)
    s["level_idc"] = r.u(8)
    s["seq_parameter_set_id"] = r.ue()
    chroma, bdl, bdc = 1, 0, 0
    if s["profile_idc"] in HIGH:
        chroma = r.ue()
        if chroma == 3:
            s["separate_colour_plane_flag"] = r.u(1)
        bdl = r.ue()
        bdc = r.ue()
        s["qpprime_y_zero_transform_bypass_flag"] = r.u(1)
        smf = r.u(1)
        s["seq_scaling_matrix_present_flag"] = smf
        if smf:
            n = 8 if chroma != 3 else 12
            for k in range(n):
                if r.u(1):                      # seq_scaling_list_present_flag
                    size = 16 if k < 6 else 64
                    last = nxt = 8
                    for _ in range(size):
                        if nxt != 0:
                            nxt = (last + r.se() + 256) % 256
                        last = nxt if nxt != 0 else last
    s["chroma_format_idc"] = chroma
    s["bit_depth_luma_minus8"] = bdl
    s["bit_depth_chroma_minus8"] = bdc
    s["log2_max_frame_num_minus4"] = r.ue()
    poc = r.ue()
    s["pic_order_cnt_type"] = poc
    if poc == 0:
        s["log2_max_pic_order_cnt_lsb_minus4"] = r.ue()
    elif poc == 1:
        s["delta_pic_order_always_zero_flag"] = r.u(1)
        s["offset_for_non_ref_pic"] = r.se()
        s["offset_for_top_to_bottom_field"] = r.se()
        n = r.ue()
        s["num_ref_frames_in_pic_order_cnt_cycle"] = n
        s["offset_for_ref_frame"] = [r.se() for _ in range(n)]
    s["max_num_ref_frames"] = r.ue()
    s["gaps_in_frame_num_value_allowed_flag"] = r.u(1)
    w_mbs = r.ue() + 1
    h_map = r.ue() + 1
    fmo = r.u(1)
    s["frame_mbs_only_flag"] = fmo
    if not fmo:
        s["mb_adaptive_frame_field_flag"] = r.u(1)
    s["direct_8x8_inference_flag"] = r.u(1)
    crop = r.u(1)
    s["frame_cropping_flag"] = crop
    cl = cr = ct = cb = 0
    if crop:
        cl, cr, ct, cb = r.ue(), r.ue(), r.ue(), r.ue()
        s["crop"] = [cl, cr, ct, cb]
    w = w_mbs * 16 - (cl + cr) * 2
    h = h_map * 16 * (2 - fmo) - (ct + cb) * 2
    s["coded_width"] = w
    s["coded_height"] = h

    vui = r.u(1)
    s["vui_parameters_present_flag"] = vui
    if not vui:
        return s
    v = {}
    if r.u(1):                                  # aspect_ratio_info_present_flag
        idc = r.u(8)
        v["aspect_ratio_idc"] = idc
        if idc == 255:
            v["sar"] = [r.u(16), r.u(16)]
    if r.u(1):                                  # overscan_info_present_flag
        v["overscan_appropriate_flag"] = r.u(1)
    if r.u(1):                                  # video_signal_type_present_flag
        v["video_format"] = r.u(3)
        v["video_full_range_flag"] = r.u(1)
        if r.u(1):
            v["colour_primaries"] = r.u(8)
            v["transfer_characteristics"] = r.u(8)
            v["matrix_coefficients"] = r.u(8)
    if r.u(1):                                  # chroma_loc_info_present_flag
        v["chroma_sample_loc_type_top_field"] = r.ue()
        v["chroma_sample_loc_type_bottom_field"] = r.ue()
    if r.u(1):                                  # timing_info_present_flag
        v["num_units_in_tick"] = r.u(32)
        v["time_scale"] = r.u(32)
        v["fixed_frame_rate_flag"] = r.u(1)
    nal_hrd = r.u(1)
    v["nal_hrd_parameters_present_flag"] = nal_hrd
    if nal_hrd:
        v["nal_hrd"] = hrd(r)
    vcl_hrd = r.u(1)
    v["vcl_hrd_parameters_present_flag"] = vcl_hrd
    if vcl_hrd:
        v["vcl_hrd"] = hrd(r)
    if nal_hrd or vcl_hrd:
        v["low_delay_hrd_flag"] = r.u(1)
    v["pic_struct_present_flag"] = r.u(1)
    if r.u(1):                                  # bitstream_restriction_flag
        v["motion_vectors_over_pic_boundaries_flag"] = r.u(1)
        v["max_bytes_per_pic_denom"] = r.ue()
        v["max_bits_per_mb_denom"] = r.ue()
        v["log2_max_mv_length_horizontal"] = r.ue()
        v["log2_max_mv_length_vertical"] = r.ue()
        v["max_num_reorder_frames"] = r.ue()
        v["max_dec_frame_buffering"] = r.ue()
    s["vui"] = v
    return s


def hrd(r):
    h = {}
    h["cpb_cnt_minus1"] = r.ue()
    h["bit_rate_scale"] = r.u(4)
    h["cpb_size_scale"] = r.u(4)
    for _ in range(h["cpb_cnt_minus1"] + 1):
        r.ue()
        r.ue()
        r.u(1)
    h["initial_cpb_removal_delay_length_minus1"] = r.u(5)
    h["cpb_removal_delay_length_minus1"] = r.u(5)
    h["dpb_output_delay_length_minus1"] = r.u(5)
    h["time_offset_length"] = r.u(5)
    return h


# The fields that decide whether a hardware decoder is sized for the stream.
KEYS = [
    "profile_idc", "level_idc", "chroma_format_idc",
    "log2_max_frame_num_minus4", "log2_max_pic_order_cnt_lsb_minus4",
    "pic_order_cnt_type", "max_num_ref_frames", "frame_mbs_only_flag",
    "coded_width", "coded_height",
]


def flat(s):
    """Flatten to dotted keys so two dumps can be diffed line by line."""
    out = {}

    def walk(pfx, o):
        for k, v in o.items():
            if isinstance(v, dict):
                walk(pfx + k + ".", v)
            elif isinstance(v, list):
                out[pfx + k] = ",".join(str(x) for x in v)
            else:
                out[pfx + k] = v

    walk("", s)
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    want = None
    as_json = "--json" in argv
    args = [a for a in argv[1:] if not a.startswith("--")]
    if "--sel" in argv:
        want = int(argv[argv.index("--sel") + 1])
        args = [a for a in args if a != argv[argv.index("--sel") + 1]]
    if not args:
        print(__doc__)
        return 2
    rc = 1
    for path in args:
        data = open(path, "rb").read()
        spss = find_sps_list(data)
        if not spss:
            sys.stderr.write("no SPS in %s\n" % path)
            continue
        idx = want if want is not None else 0
        if idx >= len(spss):
            sys.stderr.write("%s has %d SPS, wanted #%d\n" % (path, len(spss), idx))
            continue
        s = parse(spss[idx])
        rc = 0
        if as_json:
            print(json.dumps(flat(s), indent=1, sort_keys=True))
            continue
        print("%s  (#%d of %d SPS, %d bytes)" % (path, idx, len(spss), len(spss[idx])))
        for k in KEYS:
            if k in s:
                print("  %-32s %s" % (k, s[k]))
        v = s.get("vui", {})
        if not v:
            print("  vui_parameters_present_flag     0")
        else:
            for k in sorted(v):
                print("  vui.%-28s %s" % (k, v[k]))
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
