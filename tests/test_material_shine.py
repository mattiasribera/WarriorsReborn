"""Game-independent tests of tools/material_shine.py (material shine in the B / A channels of the relief pack's
specular maps). The image-text model is replaced by a fake classifier that calls blue crops glass and red crops
brick, so no model, GPU or network is needed and every image is synthetic. Covered: R and G are kept byte for byte;
glass shines brighter and tighter than brick; an atlas gets a material per region; busy texels shine less than
smooth ones of the same material; transparent texels get no shine; small textures are classified whole; the pack
run (class skips, maps without a specular map, in place or into --out, resume through material_shine.csv, --force,
a map with a .dds needs an encoder); and the model files are pinned by SHA-256. The pack tests are skipped when
runtime/build/pngraw is missing.
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    import numpy as np
    import material_shine as ms
    import texture_upscale as tu
except ImportError:   # numpy missing
    np = None
PNGRAW = os.path.join(ROOT, "runtime", "build", "pngraw")


def fake_classify(images):
    """Blue crops are glass, red crops brick, by how much bluer than red they are."""
    out = []
    for im in images:
        m = im[..., :3].reshape(-1, 3).mean(axis=0)
        t = float(np.clip(0.5 + 2.0 * (m[2] - m[0]), 0.0, 1.0))
        p = np.zeros(len(ms.NAMES), dtype=np.float32)
        p[ms.NAMES.index("glass")], p[ms.NAMES.index("brick")] = t, 1.0 - t
        out.append(p)
    return np.stack(out)


def solid(h, w, rgb, alpha=255):
    img = np.empty((h, w, 4), dtype=np.uint8)
    img[..., :3] = rgb
    img[..., 3] = alpha
    return img


def legacy_spec(h, w, seed=3):
    rng = np.random.default_rng(seed)
    s = np.zeros((h, w, 4), dtype=np.uint8)
    s[..., 0] = rng.integers(0, 90, (h, w))
    s[..., 1] = rng.integers(30, 150, (h, w))
    s[..., 3] = 255
    return s


@unittest.skipIf(np is None, "needs numpy")
class ShineTest(unittest.TestCase):
    def test_keeps_r_and_g(self):
        spec = legacy_spec(64, 64)
        out, _ = ms.shine_map(spec, solid(128, 128, (40, 60, 200)), fake_classify)
        self.assertTrue((out[..., :2] == spec[..., :2]).all())
        self.assertEqual(out.shape, spec.shape)

    def test_glass_shines_more_than_brick(self):
        glass, _ = ms.shine_map(legacy_spec(32, 32), solid(64, 64, (30, 50, 220)), fake_classify)
        brick, _ = ms.shine_map(legacy_spec(32, 32), solid(64, 64, (200, 60, 40)), fake_classify)
        self.assertGreater(glass[..., 2].mean(), 200)    # glass 0.9 x 255 on smooth texels
        self.assertGreater(glass[..., 3].mean(), 220)
        self.assertLess(brick[..., 2].mean(), 20)        # brick 0.05
        self.assertLess(brick[..., 3].mean(), 80)

    def test_atlas_regions(self):
        img = solid(128, 128, (200, 60, 40))   # brick ...
        img[:, :64, :3] = (30, 50, 220)         # ... with a glass left half
        out, regions = ms.shine_map(legacy_spec(64, 64), img, fake_classify)
        self.assertEqual(regions.shape, (ms.GRID, ms.GRID, len(ms.NAMES)))
        self.assertGreater(int(out[32, 4, 2]), int(out[32, 60, 2]) + 80)

    def test_busy_texels_shine_less(self):
        rng = np.random.default_rng(5)
        img = solid(128, 128, (30, 50, 220))
        img[:, 64:, :3] = np.clip(img[:, 64:, :3].astype(int) + rng.integers(-60, 60, (128, 64, 1)), 0, 255)
        out, _ = ms.shine_map(legacy_spec(128, 128), img, fake_classify)
        self.assertGreater(out[:, 8:48, 2].mean(), out[:, 80:120, 2].mean() * 1.5)

    def test_transparent_texels(self):
        img = solid(64, 64, (30, 50, 220))
        img[:, :32, 3] = 0
        out, _ = ms.shine_map(legacy_spec(64, 64), img, fake_classify)
        self.assertEqual(int(out[:, :30, 2].max()), 0)
        self.assertGreater(int(out[:, 34:, 2].min()), 100)

    def test_small_texture_is_classified_whole(self):
        calls = []

        def counting(images):
            calls.append(len(images))
            return fake_classify(images)
        out, regions = ms.shine_map(legacy_spec(8, 8), solid(16, 16, (30, 50, 220)), counting)
        self.assertEqual(calls, [1])
        self.assertEqual(regions.shape, (1, 1, len(ms.NAMES)))
        self.assertGreater(out[..., 2].mean(), 200)

    def test_materials_in_range(self):
        for name, (shine, gloss, prompts) in ms.MATERIALS.items():
            self.assertTrue(0 <= shine <= 1 and 0 <= gloss <= 1 and prompts, name)
        self.assertEqual(ms.MATERIALS["none"][0], 0)

    def test_models_are_pinned(self):
        for name, (path, sha) in ms.MODEL_FILES.items():
            self.assertEqual(len(sha), 64, name)
        self.assertIn(ms.MODEL_COMMIT, ms.MODEL_BASE)


@unittest.skipIf(np is None or not os.path.isfile(PNGRAW), "needs numpy and runtime/build/pngraw")
class PackTest(unittest.TestCase):
    H = {"glass": "00000000000000a1", "brick": "00000000000000b2", "char": "00000000000000c3", "nomap": "00000000000000d4"}

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.pack = os.path.join(self.tmp.name, "pack")
        self.relief = os.path.join(self.tmp.name, "relief")
        os.makedirs(self.pack)
        os.makedirs(self.relief)
        tu.write_png(os.path.join(self.pack, self.H["glass"] + ".png"), solid(128, 128, (30, 50, 220)))
        tu.write_png(os.path.join(self.pack, self.H["brick"] + ".png"), solid(64, 64, (200, 60, 40)))
        tu.write_png(os.path.join(self.pack, self.H["char"] + ".png"), solid(64, 64, (30, 50, 220)))
        tu.write_png(os.path.join(self.pack, self.H["nomap"] + ".png"), solid(64, 64, (30, 50, 220)))
        for k, size in (("glass", 64), ("brick", 32), ("char", 32)):
            tu.write_png(os.path.join(self.relief, self.H[k] + "_s.png"), legacy_spec(size, size))
        with open(os.path.join(self.pack, "selection.csv"), "w") as f:
            f.write("hash,format,w,h,status,class\n")
            f.write("%s,DXT1,32,32,upscaled,character\n" % self.H["char"])

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *extra):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = ms.main(["--pack", self.pack, "--relief", self.relief, "--pngraw", PNGRAW] + list(extra), classify=fake_classify)
        return rc, buf.getvalue()

    def spec(self, k, d=None):
        return tu.read_png(os.path.join(d or self.relief, self.H[k] + "_s.png"))

    def test_in_place_run_and_resume(self):
        before = {k: self.spec(k) for k in ("glass", "brick", "char")}
        rc, log = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("2 maps with material, 1 skipped by class, 1 without a specular map, 0 failed", log)
        self.assertTrue((self.spec("char") == before["char"]).all())    # a character keeps its map
        for k in ("glass", "brick"):
            self.assertTrue((self.spec(k)[..., :2] == before[k][..., :2]).all())
        self.assertGreater(self.spec("glass")[..., 2].mean(), self.spec("brick")[..., 2].mean() + 100)
        with open(os.path.join(self.relief, ms.CSV_NAME)) as f:
            text = f.read()
        self.assertIn(self.H["glass"] + ",glass,", text)
        self.assertIn(self.H["brick"] + ",brick,", text)
        rc, log = self.run_main()
        self.assertIn("2 textures (2 already done)", log)   # the skipped ones are looked at again (cheap)
        self.assertIn("0 maps with material", log)
        rc, log = self.run_main("--force", "--hash", self.H["glass"])
        self.assertIn("1 maps with material", log)

    def test_out_folder(self):
        out = os.path.join(self.tmp.name, "shine")
        before = self.spec("glass")
        rc, log = self.run_main("--out", out)
        self.assertEqual(rc, 0)
        self.assertTrue((self.spec("glass") == before).all())   # the relief pack is not touched
        self.assertEqual(sorted(n for n in os.listdir(out) if n.endswith(".png")),
                         sorted(self.H[k] + "_s.png" for k in ("glass", "brick")))
        self.assertGreater(self.spec("glass", out)[..., 2].mean(), 200)

    def test_dds_needs_encoder(self):
        with open(os.path.join(self.relief, self.H["glass"] + "_s.dds"), "wb") as f:
            f.write(b"DDS ")
        rc, log = self.run_main("--encoder", os.path.join(self.tmp.name, "missing"))
        self.assertEqual(rc, 1)
        self.assertIn("1 failed", log)
        with open(os.path.join(self.relief, self.H["glass"] + "_s.dds"), "rb") as f:
            self.assertEqual(f.read(), b"DDS ")   # left as it was

    def test_out_must_not_be_the_pack(self):
        with self.assertRaises(SystemExit):
            ms.main(["--pack", self.pack, "--relief", self.relief, "--out", self.pack, "--pngraw", PNGRAW], classify=fake_classify)


if __name__ == "__main__":
    unittest.main()
