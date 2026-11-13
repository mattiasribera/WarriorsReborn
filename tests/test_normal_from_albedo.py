"""Game-independent tests of tools/normal_from_albedo.py (stopgap relief/specular maps from a PNG
pack): a flat colour gives a flat normal map; two large colour areas stay flat away from their
seam (the high-pass); a raised bump tilts its normals away from its centre in the OpenGL
convention (+Y / green up the image); --strength scales the tilt; transparent texels are flat; the
specular map stays at or below --spec-max with the gloss asked for, at half the normal map's size; --max-size caps the maps; UI-like, font-category and
tiny textures get no maps; only the selected hashes are processed; and the output names. Uses
synthetic textures (no game data). Skipped when numpy or runtime/build/pngraw is missing.
"""
import csv
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    import numpy as np
    import normal_from_albedo as nfa
    import texture_upscale as tu
except ImportError:   # numpy missing
    np = None
PNGRAW = os.path.join(ROOT, "runtime", "build", "pngraw")


def decode(img):
    return img[..., :3].astype(np.float32) / 127.5 - 1.0


def bump(size=64, r=8):
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    d = np.hypot(x - size / 2, y - size / 2)
    v = np.clip(1 - d / r, 0, 1)
    img = np.empty((size, size, 4), dtype=np.uint8)
    img[..., :3] = (60 + 150 * v)[..., None].astype(np.uint8)
    img[..., 3] = 255
    return img


@unittest.skipIf(np is None or not os.path.isfile(PNGRAW), "needs numpy and runtime/build/pngraw")
class NormalFromAlbedoTest(unittest.TestCase):
    def test_flat_colour_is_flat(self):
        img = np.full((32, 32, 4), (90, 120, 40, 255), dtype=np.uint8)
        n = nfa.normal_map(img)
        self.assertTrue((n[..., :3] == (128, 128, 255)).all())
        self.assertTrue((n[..., 3] == 255).all())

    def test_large_areas_stay_flat(self):
        img = np.full((128, 128, 4), 255, dtype=np.uint8)
        img[:, :64, :3] = 40
        img[:, 64:, :3] = 200
        t = decode(nfa.normal_map(img))
        # far from both seams (x = 64 and the wrap at x = 0) the high-pass leaves no relief
        self.assertLess(np.abs(t[:, 24:40, :2]).max(), 0.02)
        self.assertLess(np.abs(t[:, 88:104, :2]).max(), 0.02)
        self.assertGreater(np.abs(t[:, 62:66, 0]).max(), 0.2)   # the seam itself is relief

    def test_bump_tilts_outwards_opengl_convention(self):
        t = decode(nfa.normal_map(bump(), strength=8.0))
        c = 32
        self.assertGreater(t[c, c + 3, 0], 0.03)    # right of the centre: +X
        self.assertLess(t[c, c - 3, 0], -0.03)      # left: -X
        self.assertGreater(t[c - 3, c, 1], 0.03)    # above (smaller row): +Y = green up the image
        self.assertLess(t[c + 3, c, 1], -0.03)      # below: -Y
        self.assertTrue((t[..., 2] > 0).all())

    def test_strength_scales_tilt(self):
        a = decode(nfa.normal_map(bump(), strength=1.0))
        b = decode(nfa.normal_map(bump(), strength=4.0))
        self.assertGreater(np.abs(b[..., 0]).max(), np.abs(a[..., 0]).max() * 1.5)
        z = decode(nfa.normal_map(bump(), strength=0.0))
        self.assertLess(np.abs(z[..., :2]).max(), 0.01)

    def test_transparent_texels_flat(self):
        img = bump()
        img[:, :32, 3] = 0
        n = nfa.normal_map(img)
        self.assertTrue((n[:, :32, :3] == (128, 128, 255)).all())

    def test_specular_conservative(self):
        img = np.full((16, 16, 4), 255, dtype=np.uint8)
        img[:8, :, :3] = (255, 0, 0)   # saturated: no sheen
        s = nfa.specular_map(img, spec_max=0.2, gloss=0.3)
        self.assertLessEqual(int(s[..., 0].max()), 51)
        self.assertEqual(int(s[12, 4, 0]), 51)      # white: the most
        self.assertEqual(int(s[2, 4, 0]), 0)        # saturated red: none
        self.assertTrue((s[..., 1] == 77).all() and (s[..., 2] == 0).all() and (s[..., 3] == 255).all())

    def test_pack_run_skips_ui_and_writes_names(self):
        with tempfile.TemporaryDirectory() as d:
            pack, out = os.path.join(d, "pack"), os.path.join(d, "out")
            os.makedirs(pack)
            wall = "00000000000000a1"
            ui = np.zeros((32, 32, 4), dtype=np.uint8)
            ui[8:24, 8:24] = (250, 250, 250, 255)   # mostly transparent grey glyph: ui-like
            font = "00000000000000a3"
            tu.write_png(os.path.join(pack, wall + ".png"), bump())
            tu.write_png(os.path.join(pack, "00000000000000a2.png"), ui)
            tu.write_png(os.path.join(pack, font + ".png"), bump())
            tu.write_png(os.path.join(pack, "00000000000000a4.png"), bump(4, 2))   # too small
            tu.write_png(os.path.join(pack, "00000000000000a5.png"), bump())       # not selected below
            with open(os.path.join(pack, "manifest.csv"), "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["hash", "category"])
                w.writerow([font, "font"])
            sel = ["--hash", wall, "--hash", "00000000000000a2", "--hash", font, "--hash", "00000000000000a4"]
            rc = nfa.main(["--pack", pack, "--out", out, "--pngraw", PNGRAW] + sel)
            self.assertEqual(rc, 0)
            self.assertEqual(sorted(os.listdir(out)), [wall + "_n.png", wall + "_s.png"])
            tu.PNGRAW = PNGRAW
            n = tu.read_png(os.path.join(out, wall + "_n.png"))
            self.assertEqual(n.shape, (64, 64, 4))
            self.assertEqual(tu.read_png(os.path.join(out, wall + "_s.png")).shape, (32, 32, 4))   # specular: half size
            rc = nfa.main(["--pack", pack, "--out", os.path.join(d, "o3"), "--pngraw", PNGRAW, "--max-size", "16", "--hash", wall])
            self.assertEqual(tu.read_png(os.path.join(d, "o3", wall + "_n.png")).shape, (16, 16, 4))   # --max-size
            rc = nfa.main(["--pack", pack, "--out", os.path.join(d, "o2"), "--pngraw", PNGRAW, "--no-specular", "--hash", wall])
            self.assertEqual(sorted(os.listdir(os.path.join(d, "o2"))), [wall + "_n.png"])

    def test_out_must_differ(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit):
                nfa.main(["--pack", d, "--out", d, "--pngraw", PNGRAW])


if __name__ == "__main__":
    unittest.main()
