"""Game-independent tests of tools/texture_compress.py: mip and block math, the DDS header the
runtime loader requires, the block-aligned atlas and its slicing back into levels, the mip
filters and alpha-coverage preservation, texture classification, and the whole conversion with a
stand-in encoder (no bc7enc, no game data). Skipped when numpy is not installed.
"""
import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    import numpy as np
    import texture_compress as tc
    import texture_upscale as tu
except ImportError:   # numpy missing
    np = None

# Stand-in for bc7enc: logs its arguments, and writes a DX10 DDS whose block i holds the RGBA of
# the top-left texel of atlas block i (then zeros), so the tests can see which texels went where.
FAKE = '''#!%s
import os, struct, sys
sys.path.insert(0, %r)
import texture_upscale as tu
tu.PNGRAW = %r
args = sys.argv[1:]
with open(%r, "a") as f:
    f.write(" ".join(a for a in args if a.startswith("-")) + "\\n")
src, dst = [a for a in args if not a.startswith("-")]
img = tu.read_png(src)
h, w = img.shape[:2]
bc1 = "-1" in args
bpb = 8 if bc1 else 16
out = bytearray()
for by in range(h // 4):
    for bx in range(w // 4):
        out += bytes(img[by * 4, bx * 4]) + bytes(bpb - 4)
hdr = b"DDS " + struct.pack("<7I", 124, 0xA1007, h, w, len(out), 0, 0) + bytes(44)
hdr += struct.pack("<2I4s5I", 32, 4, b"DX10", 0, 0, 0, 0, 0) + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
hdr += struct.pack("<5I", 71 if bc1 else 98, 3, 0, 1, 0)
open(dst, "wb").write(hdr + bytes(out))
'''


def solid(w, h, rgba):
    img = np.empty((h, w, 4), dtype=np.uint8)
    img[...] = rgba
    return img


@unittest.skipIf(np is None, "numpy not installed")
class MathTest(unittest.TestCase):
    def test_level_dims_and_bytes(self):
        self.assertEqual(tc.level_dims(5, 3), [(5, 3), (2, 1), (1, 1)])
        self.assertEqual(tc.level_dims(1, 1), [(1, 1)])
        self.assertEqual(len(tc.level_dims(1024, 256)), 11)
        self.assertEqual(tc.level_bytes(tc.BC1, 1, 1), 8)       # smaller than a block: one block
        self.assertEqual(tc.level_bytes(tc.BC7, 5, 3), 32)      # 2 x 1 blocks
        self.assertEqual(sum(tc.level_bytes(tc.BC7, w, h) for w, h in tc.level_dims(8, 4)), 80)

    def test_dds_header(self):
        hdr = tc.dds_header(tc.BC7, 8, 4, 4)
        self.assertEqual(len(hdr), 148)
        self.assertEqual(hdr[:4], b"DDS ")
        size, flags, h, w, linear, depth, mips = struct.unpack_from("<7I", hdr, 4)
        self.assertEqual((size, h, w, linear, depth, mips), (124, 4, 8, 32, 0, 4))
        self.assertTrue(flags & 0x20000, "DDSD_MIPMAPCOUNT")
        self.assertEqual(struct.unpack_from("<2I4s", hdr, 76), (32, 4, b"DX10"))
        self.assertEqual(struct.unpack_from("<5I", hdr, 128), (98, 3, 0, 1, 0))
        self.assertEqual(struct.unpack_from("<I", tc.dds_header(tc.BC1, 4, 4, 3), 128)[0], 71)
        caps = struct.unpack_from("<I", hdr, 108)[0]
        self.assertEqual(caps, 0x401008)

    def test_atlas_layout(self):
        aw, ah, pos = tc.atlas_layout(tc.level_dims(8, 4))
        self.assertEqual((aw, ah), (12, 8))   # the three small levels side by side are wider than level 0
        self.assertEqual(pos, [(0, 0), (0, 4), (4, 4), (8, 4)])
        aw, ah, pos = tc.atlas_layout(tc.level_dims(64, 64))
        self.assertEqual((aw, ah), (68, 96))   # 32 + 16 + 8 + 4 + 4 + 4: levels below 4 take a block
        self.assertTrue(all(x % 4 == 0 and y % 4 == 0 for x, y in pos))
        self.assertEqual(tc.atlas_layout([(1, 1)])[:2], (4, 4))

    def test_atlas_round_trip(self):
        dims = tc.level_dims(10, 6)   # not multiples of 4
        colors = [(10 * i, 20 + i, 255 - i, 255) for i in range(len(dims))]
        levels = [solid(w, h, c) for (w, h), c in zip(dims, colors)]
        atlas, pos = tc.build_atlas(levels)
        bh, bw = atlas.shape[0] // 4, atlas.shape[1] // 4
        blocks = b"".join(bytes(atlas[y * 4, x * 4]) + bytes(12) for y in range(bh) for x in range(bw))
        out = tc.slice_levels(blocks, tc.BC7, atlas.shape[1], dims, pos)
        self.assertEqual(len(out), sum(tc.level_bytes(tc.BC7, w, h) for w, h in dims))
        off = 0
        for (w, h), c in zip(dims, colors):
            n = tc.level_bytes(tc.BC7, w, h) // 16
            for b in range(n):
                self.assertEqual(tuple(out[off + b * 16: off + b * 16 + 4]), c, "level %dx%d block %d" % (w, h, b))
            off += n * 16

    def test_edge_padding(self):
        lvl = solid(2, 1, (0, 0, 0, 255))
        lvl[0, 1] = (9, 9, 9, 255)
        atlas, _ = tc.build_atlas([lvl])
        self.assertEqual(atlas.shape[:2], (4, 4))
        self.assertTrue((atlas[:, 1:] == (9, 9, 9, 255)).all(), "a level is padded with its own edge texels")


@unittest.skipIf(np is None, "numpy not installed")
class MipTest(unittest.TestCase):
    def test_box_matches_runtime(self):
        img = np.zeros((2, 4, 4), dtype=np.uint8)   # texpack_test's 4x2 chain values
        img[0, 0] = img[1, 1] = (128, 128, 128, 255)
        img[0, 1] = img[1, 0] = (128, 128, 128, 127)
        img[:, 2:] = (255, 0, 0, 255)
        chain = tc.mip_chain(img, "box")
        self.assertEqual([l.shape[:2] for l in chain], [(2, 4), (1, 2), (1, 1)])
        self.assertEqual(tuple(chain[1][0, 0]), (128, 128, 128, 191))   # (255 + 127 + 127 + 255 + 2) // 4
        self.assertEqual(tuple(chain[1][0, 1]), (255, 0, 0, 255))
        self.assertEqual(tuple(chain[2][0, 0]), (192, 64, 64, 223))

    def test_lanczos_keeps_flat_color_and_alpha_zero_color(self):
        chain = tc.mip_chain(solid(16, 8, (40, 80, 120, 255)))
        self.assertEqual(len(chain), 5)
        self.assertTrue(all((l == (40, 80, 120, 255)).all() for l in chain))
        img = solid(8, 8, (200, 10, 10, 0))
        img[:, :4] = (10, 200, 10, 255)
        lvl = tc.mip_chain(img)[1]
        self.assertEqual(lvl.shape, (4, 4, 4))
        self.assertTrue((lvl[:, 0, :3] == (10, 200, 10)).all(), "premultiplied: the transparent red does not bleed in")
        self.assertLessEqual(int(lvl[..., 3].max()), 255)

    def test_alpha_coverage(self):
        rng = np.random.default_rng(1)
        img = solid(64, 64, (255, 255, 255, 0))
        img[..., 3] = np.where(rng.random((64, 64)) < 0.3, 255, 0)   # thin speckle cutout, 30% coverage
        plain = tc.mip_chain(img)
        kept = tc.mip_chain(img, coverage_ref=128)
        target = tc.coverage(img[..., 3], 128)
        self.assertLess(tc.coverage(plain[3][..., 3], 128), target / 2, "without it the cutout thins out")
        for l in kept[1:5]:
            self.assertAlmostEqual(tc.coverage(l[..., 3], 128), target, delta=0.08)


@unittest.skipIf(np is None, "numpy not installed")
class ClassifyTest(unittest.TestCase):
    def test_classes(self):
        opaque = solid(8, 8, (100, 50, 25, 255))
        alpha = opaque.copy()
        alpha[0, 0, 3] = 254
        self.assertEqual(tc.classify(opaque, None, tc.BC1), tc.BC1)
        self.assertEqual(tc.classify(opaque, None, tc.BC7), tc.BC7)
        self.assertEqual(tc.classify(alpha, "cutout", tc.BC1), tc.BC7)
        self.assertEqual(tc.classify(opaque, "ui", tc.BC1), "png")
        self.assertEqual(tc.classify(opaque, "font", tc.BC1), "png")
        self.assertEqual(tc.classify(opaque, None, tc.BC1, skipped=True), "png")
        glyphs = solid(8, 8, (230, 230, 230, 0))
        glyphs[::2, ::2, 3] = 255   # gray ink on transparent: ui-like
        self.assertEqual(tc.classify(glyphs, None, tc.BC1), "png")
        self.assertEqual(tc.classify(glyphs, None, tc.BC1, ui_heuristic=False), tc.BC7)

    def test_categories(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "selection.csv"), "w") as f:
                f.write("hash,format,w,h,status,category\naaaaaaaaaaaaaaaa,0x0C,8,8,upscaled,UI\nbbbbbbbbbbbbbbbb,0x0C,8,8,upscaled,\n")
            cats = tc.read_categories(d)
            self.assertEqual(cats, {"aaaaaaaaaaaaaaaa": "ui"})


@unittest.skipIf(np is None, "numpy not installed")
class ConvertTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/pngraw"], check=True)
        tu.PNGRAW = os.path.join(ROOT, "runtime", "build", "pngraw")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        self.pack, self.out = os.path.join(d, "pack"), os.path.join(d, "out")
        os.makedirs(self.pack)
        self.enc = os.path.join(d, "fake_bc7enc")
        self.log = os.path.join(d, "enc.log")   # in the script: worker processes may keep an older environment
        with open(self.enc, "w") as f:
            f.write(FAKE % (sys.executable, os.path.join(ROOT, "tools"), tu.PNGRAW, self.log))
        os.chmod(self.enc, 0o755)
        self.opaque, self.alpha, self.ui = "00000000000000a1", "00000000000000a2", "00000000000000a3"
        tu.write_png(os.path.join(self.pack, self.opaque + ".png"), solid(8, 4, (10, 20, 30, 255)))
        img = solid(5, 3, (200, 100, 50, 255))
        img[0, 0, 3] = 0
        tu.write_png(os.path.join(self.pack, self.alpha + ".png"), img)
        tu.write_png(os.path.join(self.pack, self.ui + ".png"), solid(4, 4, (1, 2, 3, 255)))
        tu.write_png(os.path.join(self.pack, "notahash.png"), solid(4, 4, (1, 2, 3, 255)))
        with open(os.path.join(self.pack, "selection.csv"), "w") as f:
            f.write("hash,format,w,h,status,category\n%s,0x0C,4,4,upscaled,ui\n" % self.ui)

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *extra):
        return tc.main(["--pack", self.pack, "--out", self.out, "--encoder", self.enc, "-j", "2", "--pngraw", tu.PNGRAW] + list(extra))

    def read_dds(self, h):
        with open(os.path.join(self.out, h + ".dds"), "rb") as f:
            return f.read()

    def test_convert(self):
        self.assertEqual(self.run_tool(), 0)
        names = sorted(os.listdir(self.out))
        self.assertEqual(names, [self.opaque + ".dds", self.alpha + ".dds", self.ui + ".png"])
        d = self.read_dds(self.opaque)
        self.assertEqual(struct.unpack_from("<I", d, 128)[0], 71)
        self.assertEqual(len(d), 148 + sum(tc.level_bytes(tc.BC1, w, h) for w, h in tc.level_dims(8, 4)))
        self.assertEqual(struct.unpack_from("<I", d, 28)[0], 4, "mip count")
        self.assertEqual(tuple(d[148:152]), (10, 20, 30, 255), "level 0, block 0")
        d = self.read_dds(self.alpha)
        self.assertEqual(struct.unpack_from("<I", d, 128)[0], 98)
        self.assertEqual(len(d), 148 + 32 + 16 + 16)
        self.assertEqual(d[148 + 3], 0, "level 0 keeps the transparent texel")
        with open(self.log) as f:
            calls = sorted(f.read().split("\n")[:-1])
        self.assertEqual(calls, ["-q -g -1 -b", "-q -g -u6"])
        # up to date: nothing re-encoded
        self.assertEqual(self.run_tool(), 0)
        with open(self.log) as f:
            self.assertEqual(len(f.read().split("\n")[:-1]), 2)
        # a newer input is redone
        future = time.time() + 5
        os.utime(os.path.join(self.pack, self.opaque + ".png"), (future, future))
        self.assertEqual(self.run_tool("--opaque", "bc7"), 0)
        self.assertEqual(struct.unpack_from("<I", self.read_dds(self.opaque), 128)[0], 98)

    def test_alpha_coverage_default_and_none(self):
        rng = np.random.default_rng(2)
        img = solid(64, 64, (255, 255, 255, 0))
        img[..., 3] = np.where(rng.random((64, 64)) < 0.3, 255, 0)   # speckle cutout, ui-like: hence --no-ui-heuristic
        tu.write_png(os.path.join(self.pack, self.alpha + ".png"), img)
        self.assertEqual(self.run_tool("--no-ui-heuristic"), 0)
        default = self.read_dds(self.alpha)
        self.assertEqual(self.run_tool("--no-ui-heuristic", "--alpha-coverage", "all", "--force"), 0)
        self.assertEqual(self.read_dds(self.alpha), default, "all is the default")
        self.assertEqual(self.run_tool("--no-ui-heuristic", "--alpha-coverage", "none", "--force"), 0)
        none = self.read_dds(self.alpha)
        self.assertNotEqual(none, default, "none turns coverage off")
        self.assertEqual(self.run_tool("--no-ui-heuristic", "--alpha-coverage", "off", "--force"), 0)
        self.assertEqual(self.read_dds(self.alpha), none, "off is an alias of none")

    def test_ui_reclassified_drops_stale_dds_and_with_png(self):
        os.makedirs(self.out)
        stale = os.path.join(self.out, self.ui + ".dds")
        open(stale, "wb").close()
        self.assertEqual(self.run_tool("--with-png"), 0)
        self.assertFalse(os.path.exists(stale), "a stale .dds would win over the UI PNG")
        self.assertTrue(all(os.path.exists(os.path.join(self.out, h + ".png")) for h in (self.opaque, self.alpha, self.ui)))

    def test_missing_encoder(self):
        old = os.environ.pop(tc.ENCODER_ENV, None)
        try:
            with self.assertRaises(SystemExit) as cm:
                tc.main(["--pack", self.pack, "--out", self.out, "--encoder", os.path.join(self.tmp.name, "nope"),
                         "--pngraw", tu.PNGRAW])
            self.assertIn("bc7enc_rdo", str(cm.exception))
        finally:
            if old is not None:
                os.environ[tc.ENCODER_ENV] = old

    def test_encoder_from_environment(self):
        os.environ[tc.ENCODER_ENV] = self.enc
        try:
            self.assertEqual(tc.find_encoder(None), self.enc)
        finally:
            del os.environ[tc.ENCODER_ENV]


if __name__ == "__main__":
    unittest.main()
