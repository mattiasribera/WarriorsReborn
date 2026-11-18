#!/usr/bin/env python3
"""Regenerates runtime/tests/ktx2/*.ktx2, the synthetic fixtures of tests/ktx2_test.c.

Usage: ktx2_fixtures.py BASISU [OUT_DIR]   (BASISU: the basisu encoder of Basis Universal v2.50,
https://github.com/BinomialLLC/basis_universal, built with cmake; not part of this repository)

Every image is 16 x 16, made from the formulas below (tests/ktx2_test.c computes the same texels to
check what the runtime transcodes); nothing is game-derived. The mip chains are the pack
converter's (tools/texture_basis.py encode_ktx2 over texture_compress.py mip_chain; the cutouts with
alpha-coverage preservation), so the C test checks what a converted pack holds. The files are small
(a few hundred bytes) and committed, so the test needs no encoder. Needs numpy.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "tools"))
import texture_basis as tb     # noqa: E402
import texture_compress as tc  # noqa: E402

N = 16


def texel(kind, x, y):
    """(R, G, B, A) of the kind's pattern at (x, y); keep in step with ktx2_test.c pattern()."""
    if kind in ("rgb", "cutout"):
        b = max(0, min(255, 128 + 8 * (x - y)))
        a = 255 if kind == "rgb" or (2 * x - 15) ** 2 + (2 * y - 15) ** 2 < 144 else 0
        return 16 * x + 8, 16 * y + 8, b, a
    if kind == "normal":   # tangent-space normal leaning with x and y, height in alpha
        return 128 + 6 * (x - 8), 128 + 6 * (8 - y), 240, 255 - 10 * abs(x - 8)
    if kind == "shine":    # four independent channels (relief packs' _s maps use R, G; B and A must survive too)
        return 16 * x + 8, 248 - 16 * y, 0 if x < 8 else 255, 64 if y < 8 else 192
    raise ValueError(kind)


def write_png(path, kind):
    rows = b"".join(b"\0" + bytes(c for x in range(N) for c in texel(kind, x, y)) for y in range(N))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def image(kind):
    return np.array([[texel(kind, x, y) for x in range(N)] for y in range(N)], dtype=np.uint8)


OPTS = {"etc1s_quality": 100, "effort": 6, "uastc_level": 2, "uastc_rdo": 0, "xubc7_rdo": 0, "xubc7_bc7e": -1}
# name: (pattern, codec, linear metrics like the converter's maps, alpha-coverage reference or None)
FIXTURES = {
    "rgb_etc1s": ("rgb", "etc1s", False, None),
    "rgb_uastc": ("rgb", "uastc", False, None),
    "cutout_etc1s": ("cutout", "etc1s", False, 128),
    "cutout_uastc": ("cutout", "uastc", False, 128),
    "cutout_xubc7": ("cutout", "xubc7", False, 128),
    "normal_uastc": ("normal", "uastc", True, None),
    "shine_uastc": ("shine", "uastc", True, None),
    "shine_xubc7": ("shine", "xubc7", True, None),
}


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    basisu = argv[1]
    out = argv[2] if len(argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "ktx2")
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        for name, (kind, codec, linear, ref) in FIXTURES.items():
            dst = os.path.join(out, name + ".ktx2")
            tb.encode_ktx2(basisu, tc.mip_chain(image(kind), "lanczos3", ref), codec, OPTS, linear, work, dst)
            print("%s %d bytes" % (dst, os.path.getsize(dst)))
        # refused by the runtime: no mip chain, a texture array
        for kind in ("rgb", "shine"):
            write_png(os.path.join(work, kind + ".png"), kind)
        jobs = [("rgb_nomips", [os.path.join(work, "rgb.png")], ["-uastc"]),
                ("rgb_array", [os.path.join(work, "rgb.png"), os.path.join(work, "shine.png")], ["-uastc", "-mipmap", "-tex_type", "2darray"])]
        for name, srcs, opts in jobs:
            dst = os.path.join(out, name + ".ktx2")
            cmd = [basisu, "-ktx2", "-linear", "-mip_linear", "-no_multithreading", "-quiet", "-no_stats"] + opts + srcs + ["-output_file", dst]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode or not os.path.exists(dst):
                sys.exit("%s failed: %s" % (name, (r.stdout + r.stderr)[-400:]))
            print("%s %d bytes" % (dst, os.path.getsize(dst)))


if __name__ == "__main__":
    main(sys.argv)
