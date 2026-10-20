#!/usr/bin/env python
# ============================================================================
# calib_grid.py -- draw the CLUSTER CALIBRATION FRAME as raw RGBA.
#
#   python calib_grid.py [WIDTH] [HEIGHT] [OUTFILE]
#   defaults: 1280 480 /dev/shm/calib.rgba
#
# WHY (2026-10-19).  Every view-area safe box in this tree is either borrowed
# from an Audi or invented.  A photo of the cluster showing a MAP cannot be
# measured; a photo showing THIS frame can.  It gives, from one picture per
# layout:
#   * which part of our 1280x480 frame is visible (the ECU may crop or scale);
#   * where the frame's centre lands (the magenta box) -- i.e. whether the car
#     marker's natural position is even on screen in that layout;
#   * the coordinates of the clear window between the gauges, read off the
#     number labels -- which is what MIB3_SHIM_VA<i> needs.
#
# WHAT IS DRAWN
#   * dark grey background;
#   * a grid: vertical lines every 128 px, horizontal every 120 px;
#   * 10 px edge bars -- TOP red, RIGHT green, BOTTOM blue, LEFT yellow
#     (so a crop, a flip or a rotation is obvious at a glance);
#   * a magenta 64x64 box with a white cross at the exact centre (640,240);
#   * column numbers along the top, row y-values down the left, and the centre
#     coordinates under the box.  Digits are a built-in 3x5 font, scaled.
#
# Runs on the unit (python 2.7), no third-party modules.
# ============================================================================
import sys

W = 1280
H = 480
OUT = "/dev/shm/calib.rgba"

# 3x5 digits, one string per row
DIG = {
    "0": ("111", "101", "101", "101", "111"),
    "1": ("010", "110", "010", "010", "111"),
    "2": ("111", "001", "111", "100", "111"),
    "3": ("111", "001", "111", "001", "111"),
    "4": ("101", "101", "111", "001", "001"),
    "5": ("111", "100", "111", "001", "111"),
    "6": ("111", "100", "111", "101", "111"),
    "7": ("111", "001", "001", "001", "001"),
    "8": ("111", "101", "111", "101", "111"),
    "9": ("111", "101", "111", "001", "111"),
    ",": ("000", "000", "000", "010", "100"),
}


def main():
    w, h, out = W, H, OUT
    if len(sys.argv) > 1:
        w = int(sys.argv[1])
    if len(sys.argv) > 2:
        h = int(sys.argv[2])
    if len(sys.argv) > 3:
        out = sys.argv[3]

    buf = bytearray([32, 32, 32, 255]) * (w * h)

    def px(x, y, r, g, b):
        if 0 <= x < w and 0 <= y < h:
            i = (y * w + x) * 4
            buf[i] = r
            buf[i + 1] = g
            buf[i + 2] = b
            buf[i + 3] = 255

    def rect(x, y, rw, rh, c):
        for yy in range(y, y + rh):
            for xx in range(x, x + rw):
                px(xx, yy, c[0], c[1], c[2])

    def text(s, x, y, scale, c):
        cx = x
        for ch in s:
            pat = DIG.get(ch)
            if pat:
                for row in range(5):
                    for col in range(3):
                        if pat[row][col] == "1":
                            rect(cx + col * scale, y + row * scale, scale, scale, c)
            cx += 4 * scale

    # grid
    GRID = (170, 170, 170)
    for x in range(0, w, 128):
        rect(x, 0, 2, h, GRID)
    for y in range(0, h, 120):
        rect(0, y, w, 2, GRID)

    # edge bars (10 px): top left-to-right, right, bottom, left
    rect(0, 0, w, 10, (255, 0, 0))
    rect(w - 10, 0, 10, h, (0, 255, 0))
    rect(0, h - 10, w, 10, (0, 0, 255))
    rect(0, 0, 10, h, (255, 255, 0))

    # centre: magenta box + white cross
    cxc, cyc = w // 2, h // 2
    rect(cxc - 32, cyc - 32, 64, 64, (255, 0, 255))
    rect(cxc - 34, cyc - 34, 68, 4, (255, 255, 255))
    rect(cxc - 34, cyc + 30, 68, 4, (255, 255, 255))
    rect(cxc - 34, cyc - 34, 4, 68, (255, 255, 255))
    rect(cxc + 30, cyc - 34, 4, 68, (255, 255, 255))
    rect(cxc - 90, cyc - 1, 180, 2, (255, 255, 255))
    rect(cxc - 1, cyc - 90, 2, 180, (255, 255, 255))
    text(str(cxc) + "," + str(cyc), cxc - 60, cyc + 40, 5, (255, 255, 255))

    # column numbers along the top, row y-values down the left
    for i, x in enumerate(range(0, w, 128)):
        text(str(x), x + 14, 16, 6, (255, 255, 255))
    for y in range(0, h, 120):
        text(str(y), 16, y + 40, 6, (255, 255, 255))

    f = open(out, "wb")
    f.write(bytes(buf) if not isinstance(buf, bytes) else buf)
    f.close()
    sys.stdout.write("calib grid %dx%d -> %s (%d bytes)\n" % (w, h, out, w * h * 4))


if __name__ == "__main__":
    main()
