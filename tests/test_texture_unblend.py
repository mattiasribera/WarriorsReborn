"""Game-independent tests of tools/texture_unblend.py: alpha recovered from a texture painted on a
solid dark background (a glowing sign on a black panel), so the panel turns transparent while the
texture composited back over that background is unchanged. Skipped when numpy is not installed.
"""
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    import numpy as np
except ImportError:   # numpy missing
    np = None
if np is not None:
    import texture_unblend as tb
    import texture_upscale as tu

NAVY = (8, 1, 23)


def sign(w=32, h=16):
    """A navy panel with a white block (the letters) and a purple glow fading into the panel."""
    img = np.zeros((h, w, 4), np.uint8)
    img[..., :3] = NAVY
    img[..., 3] = 255
    glow = np.array((200, 60, 230), np.float32)
    for x in range(4, 28):
        t = 1.0 - abs(x - 16) / 12.0   # 0 at the panel, 1 in the middle
        img[6:10, x, :3] = (np.array(NAVY, np.float32) * (1 - t) + glow * t + 0.5).astype(np.uint8)
    img[7:9, 12:20, :3] = 255
    return img


def over(img, bg):
    """img (straight alpha) composited over a solid background, float 0..255."""
    a = img[..., 3:4].astype(np.float32) / 255.0
    return img[..., :3].astype(np.float32) * a + np.array(bg, np.float32) * (1 - a)


@unittest.skipIf(np is None, "numpy not installed")
class UnblendTest(unittest.TestCase):
    def test_panel_transparent_letters_opaque(self):
        out = tb.unblend(sign(), NAVY, floor=0.0)
        self.assertTrue((out[0, :, 3] == 0).all())          # the panel row
        self.assertTrue((out[:, 0, 3] == 0).all())
        self.assertTrue((out[7:9, 12:20, 3] == 255).all())   # the white letters
        self.assertTrue((out[7:9, 12:20, :3] == 255).all())

    def test_over_background_unchanged(self):
        src = sign()
        out = tb.unblend(src, NAVY, floor=0.0)
        diff = np.abs(over(out, NAVY) - src[..., :3].astype(np.float32))
        self.assertLessEqual(float(diff.max()), 2.0)

    def test_glow_is_partly_transparent(self):
        out = tb.unblend(sign(), NAVY, floor=0.0)
        a = out[8, 4:13, 3].astype(int)   # from the panel towards the letters
        self.assertGreater(a[-1], a[1])
        self.assertTrue(0 < a[3] < 255)

    def test_floor_drops_faint_noise(self):
        src = sign()
        src[0, 0, :3] = (NAVY[0] + 3, NAVY[1] + 3, NAVY[2] + 3)   # compression noise in the panel
        self.assertGreater(tb.unblend(src, NAVY, floor=0.0)[0, 0, 3], 0)
        self.assertEqual(tb.unblend(src, NAVY, floor=0.04)[0, 0, 3], 0)

    def test_darker_than_panel_is_panel(self):
        src = sign()
        src[0, 0, :3] = (0, 0, 0)
        self.assertEqual(tb.unblend(src, NAVY, floor=0.0)[0, 0, 3], 0)

    def test_existing_alpha_kept(self):
        src = sign()
        src[7, 12, 3] = 100
        self.assertEqual(tb.unblend(src, NAVY, floor=0.0)[7, 12, 3], 100)

    def test_auto_background_is_the_border(self):
        self.assertEqual(tb.border_color(sign()), NAVY)

    def test_transparent_texels_take_neighbour_colour(self):
        out = tb.unblend(sign(), NAVY, floor=0.04)
        # texels next to the glow are bled with its colour, not left navy (no dark filtering halo)
        self.assertGreater(int(out[5, 16, 0]), 100)


@unittest.skipIf(np is None, "numpy not installed")
class CliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/pngraw"], check=True)
        tu.PNGRAW = os.path.join(ROOT, "runtime", "build", "pngraw")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.pack, self.out = os.path.join(self.tmp.name, "pack"), os.path.join(self.tmp.name, "out")
        os.makedirs(self.pack)
        self.h1, self.h2 = "00000000000000b1", "00000000000000b2"
        for h in (self.h1, self.h2):
            tu.write_png(os.path.join(self.pack, h + ".png"), sign())

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *args):
        return tb.main(["--pack", self.pack, "--out", self.out, "--pngraw", tu.PNGRAW] + list(args))

    def test_writes_only_listed_hashes(self):
        self.assertEqual(self.run_tool(self.h1), 0)
        self.assertEqual(os.listdir(self.out), [self.h1 + ".png"])
        img = tu.read_png(os.path.join(self.out, self.h1 + ".png"))
        self.assertEqual(img[0, 0, 3], 0)
        self.assertEqual(img[8, 16, 3], 255)
        self.assertEqual(len(os.listdir(self.pack)), 2)   # the source pack is untouched

    def test_background_option(self):
        self.assertEqual(self.run_tool(self.h1, "--background", "000000"), 0)
        img = tu.read_png(os.path.join(self.out, self.h1 + ".png"))
        self.assertGreater(img[0, 0, 3], 0)   # navy is not the given black: kept partly visible

    def test_missing_hash_fails(self):
        self.assertEqual(self.run_tool("00000000000000ff"), 1)
        self.assertFalse(os.path.exists(os.path.join(self.out, "00000000000000ff.png")))

    def test_bad_hash_rejected(self):
        with self.assertRaises(SystemExit):
            self.run_tool("nothex")


if __name__ == "__main__":
    unittest.main()
