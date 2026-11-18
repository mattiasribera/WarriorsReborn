"""Game-independent tests of tools/parallax_height.py (height for --parallax in the relief pack's normal maps). Every
image is synthetic: a brick pattern (raised blocks, sunken mortar) is turned into an OpenGL normal map the way the
relief tool encodes it, and the tool must give the mortar back as deep and the bricks as the surface. Covered: the
recovered height (mortar deep, bricks at the top, the highest texel 255); weak relief stays shallow; a flat map stays 255 (no depth); RGB kept
byte for byte; metal/glass shine (specular B) keeps texels at the surface; see-through colour texels stay at the
surface; the class and material rules; and the pack run (in place and into --out, skips by class, see-through and
material, .dds needs an encoder, resume). The pack tests are skipped when runtime/build/pngraw is missing.
"""
import contextlib
import csv
import io
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    import numpy as np
    import parallax_height as ph
    import relief_from_albedo_ai as rai
    import texture_upscale as tu
except ImportError:   # numpy missing
    np = None
PNGRAW = os.path.join(ROOT, "runtime", "build", "pngraw")


def bricks(size=128, rows=8, cols=4, mortar=3):
    """size x size height: 1 on the bricks, 0 in the mortar (running bond), softened by a small blur; and the mask
    of mortar texels."""
    h = np.ones((size, size), dtype=np.float32)
    bh, bw = size // rows, size // cols
    for r in range(rows):
        h[r * bh:r * bh + mortar, :] = 0.0
        off = (bw // 2) * (r % 2)
        for c in range(cols + 1):
            x = (c * bw + off) % size
            h[r * bh:(r + 1) * bh, x:x + mortar] = 0.0
    mask = h < 0.5
    return rai.nfa.blur(h, 1), mask


def normal_map(height, k=6.0):
    """RGBA normal map of a wrapping height (OpenGL: +y up the image; rows go down), alpha 255."""
    dx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) / 2.0
    drow = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) / 2.0
    return rai.encode_normals(-dx * k, drow * k)


@unittest.skipIf(np is None, "needs numpy")
class HeightTest(unittest.TestCase):
    def test_bricks_mortar_deep(self):
        h, mortar = bricks()
        img = ph.with_height(normal_map(h), ph.height_map(normal_map(h)))
        a = img[..., 3].astype(np.float32)
        self.assertEqual(int(a.max()), 255)
        self.assertLess(a[mortar].mean(), a[~mortar].mean() - 100, "mortar sits well below the bricks")
        self.assertGreater(np.median(a[~mortar]), 180, "the bricks are near the surface")
        self.assertTrue(ph.has_height(img))

    def test_weak_relief_stays_shallow(self):
        h, mortar = bricks()
        strong = ph.height_map(normal_map(h, 6.0))
        weak = ph.height_map(normal_map(h, 0.6))
        self.assertLess(strong[mortar].mean(), 0.3)
        self.assertGreater(weak[mortar].mean(), 0.8, "a tenth of the relief: a tenth of the depth")

    def test_flat_stays_surface(self):
        flat = rai.encode_normals(np.zeros((64, 64), np.float32), np.zeros((64, 64), np.float32))
        img = ph.with_height(flat, ph.height_map(flat))
        self.assertTrue((img[..., 3] == 255).all())
        self.assertFalse(ph.has_height(img))

    def test_rgb_kept(self):
        h, _ = bricks(64, 4, 2)
        n = normal_map(h)
        img = ph.with_height(n, ph.height_map(n))
        self.assertTrue((img[..., :3] == n[..., :3]).all())

    def test_shiny_texels_stay_at_surface(self):
        h, mortar = bricks(64, 4, 2)
        height = ph.height_map(normal_map(h))
        spec = np.zeros((32, 32, 4), np.uint8)   # half size, like the relief pack's specular maps
        spec[:, 16:, 2] = 230                     # right half: glass shine
        spec[:, :16, 2] = 13                      # left half: brick
        out = ph.flatten_shiny(height, spec)
        self.assertTrue(np.allclose(out[:, 34:], 1.0), "glass texels at the surface")
        self.assertTrue(np.allclose(out[:, :30], height[:, :30]), "brick texels keep their depth")
        self.assertIs(ph.flatten_shiny(height, np.zeros((32, 32, 4), np.uint8)), height, "no material: unchanged")

    def test_see_through_texels_at_surface(self):
        h, _ = bricks(64, 4, 2)
        alpha = np.full((64, 64), 255, np.uint8)
        alpha[:8] = 0
        img = ph.with_height(normal_map(h), np.zeros((64, 64), np.float32), alpha)
        self.assertTrue((img[:8, :, 3] == 255).all() and (img[8:, :, 3] == 0).all())

    def test_classes_and_materials(self):
        self.assertIsNone(ph.wanted("environment_tiling", None))
        self.assertIsNone(ph.wanted("environment_unique", None))
        for cls in ("character", "prop", "signage_text", "decal_alpha", "fx", "ui_screen", None):
            self.assertEqual(ph.wanted(cls, None), "class", cls)
        self.assertIsNone(ph.wanted("prop", "wood"))
        self.assertIsNone(ph.wanted("environment_tiling", "brick"))
        self.assertEqual(ph.wanted("environment_tiling", "metal"), "material")
        self.assertEqual(ph.wanted("prop", "glass"), "material")
        self.assertEqual(ph.wanted("character", "fabric"), "material")
        self.assertEqual(ph.wanted("character", "brick"), "class")


@unittest.skipIf(np is None or not os.path.isfile(PNGRAW), "needs numpy and runtime/build/pngraw")
class PackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.pack = os.path.join(self.tmp.name, "pack")
        self.relief = os.path.join(self.tmp.name, "relief")
        os.makedirs(self.pack)
        os.makedirs(self.relief)
        h, _ = bricks(64, 4, 2)
        self.n = normal_map(h)
        self.hashes = {"a" * 16: "environment_tiling", "b" * 16: "character", "c" * 16: "environment_unique",
                       "d" * 16: "prop"}
        colour = np.full((64, 64, 4), 128, np.uint8)
        colour[..., 3] = 255
        cut = colour.copy()
        cut[:16, :, 3] = 0   # a fence: a quarter see-through
        for k in self.hashes:
            tu.write_png(os.path.join(self.pack, k + ".png"), cut if k[0] == "c" else colour)
            tu.write_png(os.path.join(self.relief, k + "_n.png"), self.n)
        with open(os.path.join(self.pack, "selection.csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["hash", "class"])
            for k, c in self.hashes.items():
                w.writerow([k, c])

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *extra):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = ph.main(["--relief", self.relief, "--pack", self.pack, "--pngraw", PNGRAW, "--no-dds"] + list(extra))
        return rc, out.getvalue()

    def alpha_min(self, folder, k):
        return int(tu.read_png(os.path.join(folder, k + "_n.png"))[..., 3].min())

    def test_in_place(self):
        rc, log = self.run_tool()
        self.assertEqual(rc, 0, log)
        self.assertIn("1 with height", log)
        self.assertLess(self.alpha_min(self.relief, "a" * 16), 100, "tiling environment: height")
        for k in "bcd":   # character, see-through, prop without a material
            self.assertEqual(self.alpha_min(self.relief, k * 16), 255, k)
        img = tu.read_png(os.path.join(self.relief, "a" * 16 + "_n.png"))
        self.assertTrue((img[..., :3] == self.n[..., :3]).all(), "RGB kept")
        rc, log = self.run_tool()
        self.assertIn("1 already there", log)

    def test_out_and_materials(self):
        mats = os.path.join(self.tmp.name, "material_shine.csv")
        with open(mats, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["hash", "material", "p", "shine", "gloss"])
            w.writerow(["a" * 16, "metal", "0.8", "0.5", "0.6"])
            w.writerow(["d" * 16, "wood", "0.7", "0.08", "0.3"])
        out = os.path.join(self.tmp.name, "out")
        rc, log = self.run_tool("--out", out, "--materials", mats)
        self.assertEqual(rc, 0, log)
        self.assertEqual(sorted(os.listdir(out)), ["d" * 16 + "_n.png"], "only the wooden prop, into --out")
        self.assertLess(self.alpha_min(out, "d" * 16), 100)
        self.assertEqual(self.alpha_min(self.relief, "d" * 16), 255, "the relief pack is untouched")

    def test_dds_needs_encoder(self):
        open(os.path.join(self.relief, "a" * 16 + "_n.dds"), "wb").close()
        old = os.environ.pop("TEXPACK_BC_ENCODER", None)
        try:
            with self.assertRaises(SystemExit), contextlib.redirect_stdout(io.StringIO()):
                ph.main(["--relief", self.relief, "--pack", self.pack, "--pngraw", PNGRAW, "--encoder",
                         os.path.join(self.tmp.name, "missing")])
        finally:
            if old is not None:
                os.environ["TEXPACK_BC_ENCODER"] = old


if __name__ == "__main__":
    unittest.main()
