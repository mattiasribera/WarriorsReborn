"""Game-independent tests of tools/ui_redraw.py (sharp non-generative upscale of flat UI art with
re-typeset labels): the Lanczos resampler, the colour classes and two-colour unmixing, the sharp
redraw of a flat shape (crisper edge than a plain upscale, flat colours kept, 1-texel lines kept),
the label finder (Otsu ink, a coloured line and stray marks ignored), label replacement (old ink
gone, new text in the ink colour inside the ink box), alpha and the area outside the regions
(plain premultiplied upscale) and the command line. Skipped without numpy and Pillow. No game data.
"""
import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    import numpy as np
    from PIL import Image
except ImportError:   # numpy or Pillow missing
    np = None
if np is not None:
    import ui_redraw as ur

FONT = os.path.join(ROOT, "launcher", "third_party", "inter", "Inter-SemiBold.ttf")
PAPER = (200, 190, 150)
WATER = (60, 90, 125)
INK = (45, 45, 45)
GREEN = (40, 120, 50)


def disc_image(n=32, r=10.0):
    """Water with a paper disc: a flat two-colour shape with a staircase edge."""
    yy, xx = np.mgrid[0:n, 0:n]
    img = np.empty((n, n, 3))
    img[:] = WATER
    img[np.hypot(yy - n / 2 + 0.5, xx - n / 2 + 0.5) < r] = PAPER
    return img


def edge_width(row, lo, hi):
    """Number of samples strictly between 10% and 90% of the way from lo to hi."""
    t = (row - lo) / (hi - lo)
    return int(np.sum((t > 0.1) & (t < 0.9)))


@unittest.skipIf(np is None, "numpy or Pillow not installed")
class ResampleTest(unittest.TestCase):
    def test_lanczos_rows_sum_to_one(self):
        m = ur.lanczos_matrix(7, 4)
        self.assertEqual(m.shape, (28, 7))
        np.testing.assert_allclose(m.sum(1), 1.0)

    def test_constant_stays_constant(self):
        img = np.full((5, 6, 3), 77.0)
        up = ur.upscale(img, 3)
        self.assertEqual(up.shape, (15, 18, 3))
        np.testing.assert_allclose(up, 77.0)

    def test_gauss_blur_keeps_mean_of_constant(self):
        np.testing.assert_allclose(ur.gauss_blur(np.full((9, 9), 5.0), 1.5), 5.0)


@unittest.skipIf(np is None, "numpy or Pillow not installed")
class ClassesTest(unittest.TestCase):
    def test_kmeans_finds_flat_colours(self):
        img = disc_image()
        c = ur.kmeans(img.reshape(-1, 3), 2)
        np.testing.assert_allclose(c, [WATER, PAPER], atol=1e-6)   # sorted dark to light

    def test_kmeans_is_deterministic(self):
        px = np.random.default_rng(3).uniform(0, 255, (500, 3))
        np.testing.assert_array_equal(ur.kmeans(px, 5), ur.kmeans(px, 5))

    def test_coverage_unmixes_an_edge_texel(self):
        centers = np.array([WATER, PAPER], float)
        img = np.array([[WATER, PAPER, 0.25 * np.array(WATER) + 0.75 * np.array(PAPER)]], float)
        cov = ur.coverage(img, centers)
        np.testing.assert_allclose(cov[0, 0], [1, 0])
        np.testing.assert_allclose(cov[0, 1], [0, 1])
        np.testing.assert_allclose(cov[0, 2], [0.25, 0.75], atol=1e-6)

    def test_prune_drops_an_in_between_centre(self):
        mid = 0.5 * (np.array(WATER) + np.array(PAPER))
        kept = ur.prune_mixtures(np.array([WATER, mid, PAPER], float), tol=5)
        np.testing.assert_allclose(kept, [WATER, PAPER])


@unittest.skipIf(np is None, "numpy or Pillow not installed")
class RedrawFlatTest(unittest.TestCase):
    def test_edge_is_sharper_than_lanczos_and_colours_flat(self):
        img = disc_image()
        flat = ur.redraw_flat(img, 4, classes=2, ink_detail=0.0)
        lan = ur.upscale(img, 4)
        self.assertEqual(flat.shape, (128, 128, 3))
        row = 64
        w_flat = edge_width(flat[row, :, 0], WATER[0], PAPER[0])
        w_lan = edge_width(lan[row, :, 0], WATER[0], PAPER[0])
        self.assertLess(w_flat, w_lan)
        self.assertLessEqual(w_flat, 6)   # two edges, at most 3 samples each
        np.testing.assert_allclose(flat[64, 64], PAPER, atol=1.0)
        np.testing.assert_allclose(flat[2, 2], WATER, atol=1.0)

    def test_one_texel_line_survives(self):
        img = np.empty((24, 24, 3))
        img[:] = PAPER
        img[:, 11] = GREEN
        flat = ur.redraw_flat(img, 4, classes=2)
        centre = flat[48, 44:48].mean(0)
        np.testing.assert_allclose(centre, GREEN, atol=12.0)
        np.testing.assert_allclose(flat[48, 20], PAPER, atol=2.0)

    def test_excluded_texels_are_filled_from_surroundings(self):
        img = disc_image()
        img[14:18, 14:18] = INK
        ex = np.zeros(img.shape[:2], bool)
        ex[14:18, 14:18] = True
        flat = ur.redraw_flat(img, 4, classes=3, exclude=ex)
        np.testing.assert_allclose(flat[64, 64], PAPER, atol=3.0)


@unittest.skipIf(np is None, "numpy or Pillow not installed")
class LabelTest(unittest.TestCase):
    def label_image(self):
        img = np.empty((40, 60, 3))
        img[:] = PAPER
        img[15:25, 10:50] = INK          # the "lettering"
        img[5:35, 30] = GREEN            # a coloured line crossing it
        img[33, 52] = INK                # a stray mark inside the search box
        return img

    def test_otsu_splits_two_levels(self):
        thr = ur.otsu(np.array([40] * 50 + [200] * 50))
        self.assertTrue(40 < thr < 200)

    def test_find_ink_box_and_colour(self):
        ink, tight, color = ur.find_ink(self.label_image(), (5, 10, 56, 36))
        self.assertEqual(tight, (10, 15, 50, 25))
        np.testing.assert_allclose(color, INK)
        self.assertFalse(ink[10, 30])    # the green line is not ink
        self.assertTrue(ink[20, 20])

    def test_no_ink_raises(self):
        with self.assertRaises(ValueError):
            ur.find_ink(np.full((10, 10, 3), 128.0), (0, 0, 10, 10))

    @unittest.skipUnless(os.path.exists(FONT), "launcher font missing")
    def test_label_replaced_in_ink_colour(self):
        rgb = self.label_image()
        rgba = np.concatenate([rgb, np.full(rgb.shape[:2] + (1,), 255.0)], -1).astype(np.uint8)
        spec = {"scale": 4, "classes": 3, "font": FONT,
                "labels": [{"text": "ABC", "box": [5, 10, 56, 36]}]}
        out = ur.redraw(rgba, spec).astype(float)
        self.assertEqual(out.shape, (160, 240, 4))
        box = out[60:100, 40:200, :3]
        lum = ur.luminance(box)
        dark = lum < 90
        self.assertTrue(0.15 < dark.mean() < 0.8)   # glyphs, not a solid block
        np.testing.assert_allclose(np.median(box[dark], 0), INK, atol=12.0)
        # the old block's solid ink is gone between the glyph rows above and below the text
        self.assertGreater(ur.luminance(out[56, 40:200, :3]).mean(), 120)
        self.assertGreater(ur.luminance(out[103, 40:200, :3]).mean(), 120)
        np.testing.assert_array_equal(out[..., 3], 255)


@unittest.skipIf(np is None, "numpy or Pillow not installed")
class WholeImageTest(unittest.TestCase):
    def test_alpha_and_outside_region_are_plain_upscale(self):
        rgba = np.zeros((32, 48, 4), np.uint8)
        rgba[:, :32, :3] = disc_image().astype(np.uint8)
        rgba[:, :32, 3] = 255
        rgba[10:20, 36:44] = (255, 255, 255, 255)   # an opaque marker on transparency
        spec = {"scale": 4, "classes": 2, "regions": [[0, 0, 32, 32]]}
        out = ur.redraw(rgba, spec)
        ref = np.clip(ur.premul_upscale(rgba.astype(float), 4) + 0.5, 0, 255).astype(np.uint8)
        np.testing.assert_array_equal(out[..., 3], ref[..., 3])
        np.testing.assert_array_equal(out[:, 140:], ref[:, 140:])
        self.assertEqual(out[0, 190, 3], 0)

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "a.png")
            Image.fromarray(np.concatenate([disc_image(16, 5).astype(np.uint8),
                                            np.full((16, 16, 1), 255, np.uint8)], -1), "RGBA").save(src)
            spec = os.path.join(d, "spec.json")
            with open(spec, "w") as f:
                json.dump({"scale": 2, "classes": 2}, f)
            dst = os.path.join(d, "b.png")
            self.assertEqual(ur.main(["--spec", spec, src, dst]), 0)
            with Image.open(dst) as im:
                self.assertEqual(im.size, (32, 32))


if __name__ == "__main__":
    unittest.main()
