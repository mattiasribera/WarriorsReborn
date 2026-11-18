"""Game-independent tests of tools/relief_from_albedo_ai.py (relief/specular maps from a local AI estimator). The
network is replaced by a fake, local estimator (Sobel normals of the grey tile, bright = high), so no model, GPU or
network is needed and every image is synthetic. Covered: tiled inference equals whole-image inference and has no
seam under wrap padding; small textures; the OpenGL convention (+Y / green up the image) of a raised bump;
--strength and the character class scale the tilt; a constant tilt is flattened; transparent texels are flat and do
not make false edges; the height integrated from the slopes peaks on the bump; the specular map's ranges (smooth
glossier than rough, character duller, transparent 0); the pack run (class skips, names, sizes, resume, --force,
--dds encoder required); and the model download (missing with --no-download, checksum). The pack tests are skipped
when runtime/build/pngraw is missing.
"""
import contextlib
import hashlib
import io
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    import numpy as np
    import relief_from_albedo_ai as rai
    import texture_upscale as tu
except ImportError:   # numpy missing
    np = None
PNGRAW = os.path.join(ROOT, "runtime", "build", "pngraw")


def sobel_normals(grey, k=4.0, mode="wrap"):
    """3 x H x W encoded normals of grey taken as height (OpenGL: +y up the image)."""
    p = np.pad(grey, 1, mode=mode)
    dx = (p[1:-1, 2:] - p[1:-1, :-2]) / 2.0
    drow = (p[2:, 1:-1] - p[:-2, 1:-1]) / 2.0
    n = np.stack([-dx * k, drow * k, np.ones_like(grey)])
    n /= np.linalg.norm(n, axis=0, keepdims=True)
    return n * 0.5 + 0.5


def fake_estimator(tile):
    return sobel_normals(tile, mode="edge")


def decode(img):
    return img[..., :3].astype(np.float32) / 127.5 - 1.0


def smooth_noise(h, w, seed=1, r=3):
    rng = np.random.default_rng(seed)
    return rai.nfa.blur(rng.random((h, w)).astype(np.float32), r)


def bump(size=128, r=24):
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    v = np.clip(1 - np.hypot(x - size / 2, y - size / 2) / r, 0, 1)
    img = np.empty((size, size, 4), dtype=np.uint8)
    img[..., :3] = (60 + 150 * v)[..., None].astype(np.uint8)
    img[..., 3] = 255
    return img


@unittest.skipIf(np is None, "needs numpy")
class InferenceTest(unittest.TestCase):
    def test_tiled_equals_whole_image(self):
        g = smooth_noise(400, 600)
        tiled = rai.infer_tiled(g, fake_estimator, overlap=64, mode="wrap")
        whole = sobel_normals(g, mode="wrap")
        self.assertEqual(tiled.shape, (3, 400, 600))
        self.assertLess(np.abs(tiled - whole).max(), 0.01)

    def test_wrap_has_no_seam(self):
        # a tiling texture shifted by any amount gets the same (shifted) prediction: no seam at its borders
        g = smooth_noise(300, 300, seed=2)
        a = rai.infer_tiled(g, fake_estimator, mode="wrap")
        b = np.roll(rai.infer_tiled(np.roll(g, (150, 150), axis=(0, 1)), fake_estimator, mode="wrap"),
                    (-150, -150), axis=(1, 2))
        self.assertLess(np.abs(a - b).max(), 0.01)

    def test_small_texture(self):
        g = smooth_noise(32, 48)
        for mode in ("wrap", "reflect"):
            p = rai.infer_tiled(g, fake_estimator, mode=mode)
            self.assertEqual(p.shape, (3, 32, 48))
            self.assertTrue(np.isfinite(p).all())

    def test_tile_weight_positive(self):
        w = rai.tile_weight(256, 64)
        self.assertGreater(w.min(), 0)
        self.assertEqual(w[128, 128], 1.0)


@unittest.skipIf(np is None, "needs numpy")
class MapsTest(unittest.TestCase):
    def maps(self, img, cls=None, **kw):
        return rai.relief_maps(img, fake_estimator, cls, dict(kw, flatten_radius=kw.get("flatten_radius", 0.0)))

    def test_flat_colour_is_flat(self):
        n, s = self.maps(np.full((64, 64, 4), (90, 120, 40, 255), dtype=np.uint8))
        self.assertTrue((n[..., :3] == (128, 128, 255)).all())
        self.assertTrue((n[..., 3] == 255).all())
        self.assertEqual(s.shape, (32, 32, 4))

    def test_bump_opengl_convention(self):
        t = decode(self.maps(bump())[0])
        c, d = 64, 12
        self.assertGreater(t[c, c + d, 0], 0.1)    # right of the bump: normal leans right (+X, red up)
        self.assertLess(t[c, c - d, 0], -0.1)
        self.assertGreater(t[c - d, c, 1], 0.1)    # above it (smaller row): leans up the image (+Y, green up)
        self.assertLess(t[c + d, c, 1], -0.1)

    def test_strength_scales_tilt(self):
        t0 = decode(self.maps(bump(), strength=0.0)[0])
        t1 = decode(self.maps(bump(), strength=1.0)[0])
        t2 = decode(self.maps(bump(), strength=2.0)[0])
        self.assertLess(np.abs(t0[..., :2]).max(), 0.01)
        self.assertGreater(np.abs(t2[..., 0]).max(), np.abs(t1[..., 0]).max() + 0.05)

    def test_character_is_gentler(self):
        env = np.abs(decode(self.maps(bump(), "environment_tiling")[0])[..., 0]).max()
        char = np.abs(decode(self.maps(bump(), "character")[0])[..., 0]).max()
        self.assertLess(char, env * 0.6)

    def test_constant_tilt_is_flattened(self):
        def tilted(tile):
            n = np.zeros((3,) + tile.shape, dtype=np.float32)
            n[0], n[2] = 0.3, 0.95
            return n * 0.5 + 0.5
        img = np.full((64, 64, 4), 128, dtype=np.uint8)
        kept = decode(rai.relief_maps(img, tilted, None, {"flatten_radius": 0.0})[0])
        flat = decode(rai.relief_maps(img, tilted, None, {"flatten_radius": 0.25})[0])
        self.assertGreater(kept[..., 0].mean(), 0.2)
        self.assertLess(np.abs(flat[..., :2]).max(), 0.02)

    def test_transparent_texels(self):
        img = bump(128, 16)          # bump between columns 48 and 80
        img[:, :40, :3] = 0          # black, transparent left part: no edge at its border
        img[:, :40, 3] = 0
        n, s = self.maps(img)
        t = decode(n)
        self.assertLess(np.abs(t[:, :40, :2]).max(), 0.01)
        self.assertLess(np.abs(t[:, 41:44, :2]).max(), 0.05)
        self.assertEqual(int(s[:, :19, 0].max()), 0)

    def test_height_peaks_on_bump(self):
        g = bump()[..., 0].astype(np.float32) / 255
        n = sobel_normals(g, k=8.0)
        sx, sy = (n[0] * 2 - 1) / (n[2] * 2 - 1), (n[1] * 2 - 1) / (n[2] * 2 - 1)
        hgt = rai.height_from_slopes(sx, sy)
        self.assertEqual(np.unravel_index(hgt.argmax(), hgt.shape), (64, 64))
        self.assertGreater(hgt[64, 64] - hgt[5, 5], 0.5)

    def test_specular_ranges(self):
        flat_h = np.full((64, 64), 0.5, dtype=np.float32)
        smooth = np.full((64, 64, 4), 160, dtype=np.uint8)
        rough = smooth.copy()
        rough[..., :3] = np.clip(160 + np.random.default_rng(3).normal(0, 50, (64, 64, 1)), 0, 255).astype(np.uint8)
        a = rai.specular_map(smooth, flat_h, spec_max=0.4, gloss_min=0.1, gloss_max=0.7)
        b = rai.specular_map(rough, flat_h, spec_max=0.4, gloss_min=0.1, gloss_max=0.7)
        c = rai.specular_map(smooth, flat_h, "character", spec_max=0.4, gloss_min=0.1, gloss_max=0.7)
        for m in (a, b, c):
            self.assertLessEqual(int(m[..., 0].max()), round(0.4 * 255))
            self.assertGreaterEqual(int(m[..., 1].min()), round(0.1 * 255) - 1)
            self.assertLessEqual(int(m[..., 1].max()), round(0.7 * 255) + 1)
            self.assertTrue((m[..., 2] == 0).all() and (m[..., 3] == 255).all())
        self.assertGreater(a[..., 0].mean(), b[..., 0].mean())
        self.assertGreater(a[..., 1].mean(), b[..., 1].mean() + 50)
        self.assertLess(c[..., 0].mean(), a[..., 0].mean() * 0.7)


@unittest.skipIf(np is None or not os.path.isfile(PNGRAW), "needs numpy and runtime/build/pngraw")
class PackTest(unittest.TestCase):
    H = {"tile": "00000000000000a1", "ui": "00000000000000b2", "fx": "00000000000000c3", "tiny": "00000000000000d4",
         "char": "00000000000000e5"}

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.pack = os.path.join(self.tmp.name, "pack")
        self.out = os.path.join(self.tmp.name, "maps")
        os.makedirs(self.pack)
        tu.write_png(os.path.join(self.pack, self.H["tile"] + ".png"), bump(128))
        tu.write_png(os.path.join(self.pack, self.H["ui"] + ".png"), bump(64))
        tu.write_png(os.path.join(self.pack, self.H["fx"] + ".png"), bump(64))
        tu.write_png(os.path.join(self.pack, self.H["tiny"] + ".png"), bump(4))
        tu.write_png(os.path.join(self.pack, self.H["char"] + ".png"), bump(1024, 200))
        with open(os.path.join(self.pack, "selection.csv"), "w") as f:
            f.write("hash,format,w,h,status,class\n")
            for k, c in (("tile", "environment_tiling"), ("ui", "ui_screen"), ("fx", "fx"), ("char", "character")):
                f.write("%s,DXT1,32,32,upscaled,%s\n" % (self.H[k], c))

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *extra):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = rai.main(["--pack", self.pack, "--out", self.out, "--pngraw", PNGRAW, "--max-size", "256"] + list(extra),
                          estimator=fake_estimator)
        return rc, buf.getvalue()

    def test_pack_run_and_resume(self):
        rc, log = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("2 with maps, 0 already there, 2 skipped by class, 0 UI/font skipped, 1 too small", log)
        self.assertEqual(sorted(os.listdir(self.out)), sorted(
            "%s_%s.png" % (self.H[k], s) for k in ("tile", "char") for s in "ns"))
        self.assertEqual(tu.read_png(os.path.join(self.out, self.H["char"] + "_n.png")).shape, (256, 256, 4))
        self.assertEqual(tu.read_png(os.path.join(self.out, self.H["char"] + "_s.png")).shape, (128, 128, 4))
        rc, log = self.run_main()
        self.assertIn("0 with maps, 2 already there", log)
        rc, log = self.run_main("--force", "--hash", self.H["tile"])
        self.assertIn("1 with maps, 0 already there", log)

    def test_no_specular_and_skip_class(self):
        rc, log = self.run_main("--no-specular", "--skip-class", "character")
        self.assertEqual(sorted(os.listdir(self.out)), sorted(
            "%s_n.png" % self.H[k] for k in ("tile", "ui", "fx")))

    def test_dds_needs_encoder(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_main("--dds", "--encoder", os.path.join(self.tmp.name, "missing"))
        self.assertIn("BC encoder not found", str(cm.exception.code))

    def test_out_must_differ(self):
        with self.assertRaises(SystemExit):
            rai.main(["--pack", self.pack, "--out", self.pack, "--pngraw", PNGRAW], estimator=fake_estimator)


@unittest.skipIf(np is None, "needs numpy")
class ModelTest(unittest.TestCase):
    def test_missing_without_download(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(FileNotFoundError):
                rai.ensure_model(os.path.join(d, "m.onnx"), download=False)

    def test_download_checks_sha(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "src.bin")
            with open(src, "wb") as f:
                f.write(b"model bytes")
            url = "file://" + src
            dst = os.path.join(d, "cache", "m.onnx")
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(RuntimeError):
                    rai.ensure_model(dst, url, "0" * 64)
                self.assertFalse(os.path.exists(dst) or os.path.exists(dst + ".part"))
                good = hashlib.sha256(b"model bytes").hexdigest()
                self.assertEqual(rai.ensure_model(dst, url, good), dst)
            with open(dst, "rb") as f:
                self.assertEqual(f.read(), b"model bytes")
            self.assertEqual(rai.ensure_model(dst, "file:///nonexistent", good), dst)   # present: no download


if __name__ == "__main__":
    unittest.main()
