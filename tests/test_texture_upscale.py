"""Game-independent tests of tools/texture_upscale.py: selection rules, wrap padding and crop,
the separate alpha path and the whole pipeline with a stand-in upscaler (nearest-neighbor 4x,
no GPU, no game data). Skipped when numpy is not installed.
"""
import csv
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    import numpy as np
    import texture_upscale as tu
except ImportError:   # numpy missing
    np = None

FAKE = '''#!/usr/bin/env python3
# Stand-in for realesrgan-ncnn-vulkan: nearest-neighbor 4x of every PNG in -i into -o.
import os, sys
sys.path.insert(0, %r)
import numpy as np
import texture_upscale as tu
a = sys.argv
i, o = a[a.index("-i") + 1], a[a.index("-o") + 1]
for n in os.listdir(i):
    img = tu.read_png(os.path.join(i, n))[..., :3]
    tu.write_png(os.path.join(o, n), np.repeat(np.repeat(img, 4, axis=0), 4, axis=1))
'''

FAKE_INFER = '''#!/usr/bin/env python3
# Stand-in for tools/spandrel_infer.py: records argv, nearest-neighbor 4x of every PNG in -i into -o.
import json, os, sys
sys.path.insert(0, %r)
import numpy as np
import texture_upscale as tu
a = sys.argv
with open(os.path.join(os.path.dirname(os.path.abspath(a[0])), "argv.json"), "w") as f:
    json.dump(a[1:], f)
i, o = a[a.index("-i") + 1], a[a.index("-o") + 1]
for n in os.listdir(i):
    img = tu.read_png(os.path.join(i, n))[..., :3]
    tu.write_png(os.path.join(o, n), np.repeat(np.repeat(img, 4, axis=0), 4, axis=1))
'''


def make_dump(d, rows):
    dump = os.path.join(d, "dump")
    os.makedirs(dump)
    with open(os.path.join(dump, "manifest.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["hash", "format", "w", "h", "levels", "linear", "has_alpha", "draws", "first_frame"])
        for h, tw, th, lin, alpha in rows:
            w.writerow([h, "0x0F", tw, th, 1, lin, alpha, 1, 0])
    rng = np.random.default_rng(1)
    src = {}
    for h, tw, th, lin, alpha in rows:
        img = rng.integers(0, 256, (th, tw, 4), dtype=np.uint8)
        if not alpha:
            img[..., 3] = 255
        src[h] = img
        tu.write_png(os.path.join(dump, h + ".png"), img)
    return dump, src


@unittest.skipIf(np is None, "numpy not installed")
class TextureUpscaleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/pngraw"], check=True)

    def test_png_round_trip(self):
        img = np.arange(5 * 3 * 4, dtype=np.uint8).reshape(5, 3, 4)
        with tempfile.TemporaryDirectory() as d:
            tu.write_png(os.path.join(d, "x.png"), img)
            self.assertTrue(np.array_equal(tu.read_png(os.path.join(d, "x.png")), img))

    def test_wrap_pad_and_crop(self):
        img = np.arange(4 * 4 * 3, dtype=np.uint8).reshape(4, 4, 3)
        p = tu.wrap_pad(img, 2)
        self.assertEqual(p.shape, (8, 8, 3))
        self.assertTrue(np.array_equal(p[0, 2:6], img[2]))      # top pad = bottom rows
        self.assertTrue(np.array_equal(p[2:6, 0], img[:, 2]))   # left pad = right columns
        big = np.repeat(np.repeat(p, 4, axis=0), 4, axis=1)
        self.assertTrue(np.array_equal(tu.crop(big, 2, 4), np.repeat(np.repeat(img, 4, axis=0), 4, axis=1)))

    def test_fit_and_ui_heuristic(self):
        img = np.full((64, 32, 4), 200, dtype=np.uint8)
        self.assertEqual(tu.fit(img, 16).shape, (16, 8, 4))
        self.assertEqual(tu.fit(img, 64).shape, (64, 32, 4))
        glyphs = np.zeros((16, 16, 4), dtype=np.uint8)
        glyphs[4:8, 4:8] = 255                                  # white glyph on transparent
        self.assertTrue(tu.ui_like(glyphs))
        opaque = np.full((16, 16, 4), 255, dtype=np.uint8)
        self.assertFalse(tu.ui_like(opaque))
        colored = glyphs.copy()
        colored[4:8, 4:8, 1] = 0                                # magenta texels
        self.assertFalse(tu.ui_like(colored))

    def test_pipeline_with_stand_in_upscaler(self):
        with tempfile.TemporaryDirectory() as d:
            dump, out = os.path.join(d, "dump"), os.path.join(d, "pack")
            os.makedirs(dump)
            rows = [("00000000000000a1", 32, 16, 0, 1), ("00000000000000a2", 8, 8, 0, 0),
                    ("00000000000000a3", 32, 32, 1, 0), ("00000000000000a4", 16, 16, 0, 0),
                    ("00000000000000a5", 16, 16, 0, 0)]
            with open(os.path.join(dump, "manifest.csv"), "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["hash", "format", "w", "h", "levels", "linear", "has_alpha", "draws", "first_frame"])
                for h, tw, th, lin, alpha in rows:
                    w.writerow([h, "0x0F", tw, th, 1, lin, alpha, 1, 0])
            rng = np.random.default_rng(1)
            src = {}
            for h, tw, th, lin, alpha in rows:
                img = rng.integers(0, 256, (th, tw, 4), dtype=np.uint8)
                if not alpha:
                    img[..., 3] = 255
                src[h] = img
                tu.write_png(os.path.join(dump, h + ".png"), img)
            fake = os.path.join(d, "fake_esrgan.py")
            with open(fake, "w") as f:
                f.write("#!" + sys.executable + "\n" + FAKE % os.path.join(ROOT, "tools"))
            os.chmod(fake, 0o755)
            with open(os.path.join(d, "reject.json"), "w") as f:
                json.dump({"rejected": ["00000000000000a5"]}, f)
            tu.main(["--dump", dump, "--out", out, "--model", "fake", "--esrgan", fake, "--pad", "4",
                     "--reject", os.path.join(d, "reject.json")])
            pack = os.path.join(out, "fake")
            names = sorted(n for n in os.listdir(pack) if n.endswith(".png"))
            self.assertEqual(names, ["00000000000000a1.png", "00000000000000a4.png"])   # small, linear, rejected out
            up = tu.read_png(os.path.join(pack, "00000000000000a1.png"))
            want = np.repeat(np.repeat(src["00000000000000a1"], 4, axis=0), 4, axis=1)
            self.assertEqual(up.shape, (64, 128, 4))
            self.assertTrue(np.array_equal(up, want))   # color and the separately upscaled alpha
            with open(os.path.join(pack, "selection.csv")) as f:
                status = {r["hash"]: r["status"] for r in csv.DictReader(f)}
            self.assertEqual(status["00000000000000a2"], "skipped:small")
            self.assertEqual(status["00000000000000a3"], "skipped:linear")
            self.assertEqual(status["00000000000000a5"], "skipped:rejected")

    def test_resolve_model(self):
        with tempfile.TemporaryDirectory() as d:
            for n in ("1x_DEDXT.pth", "4x-GameAI-2-0.pth", "4xTextures_GTAV_rgt-s.pth",
                      "4xTextures_GTAV_rgt-s.safetensors", "notes.txt"):
                open(os.path.join(d, n), "w").close()
            self.assertEqual(os.path.basename(tu.resolve_model(d, "1x-DEDXT")), "1x_DEDXT.pth")
            self.assertEqual(os.path.basename(tu.resolve_model(d, "4x-gameai-2-0")), "4x-GameAI-2-0.pth")
            self.assertEqual(os.path.basename(tu.resolve_model(d, "4x-Textures-GTAV-rgt-s")),
                             "4xTextures_GTAV_rgt-s.safetensors")   # safetensors preferred, '-'/'_' ignored
            with self.assertRaises(SystemExit):
                tu.resolve_model(d, "4x-Missing")

    def test_spandrel_backend_chain(self):
        with tempfile.TemporaryDirectory() as d:
            dump, src = make_dump(d, [("00000000000000b1", 32, 16, 0, 1), ("00000000000000b2", 16, 16, 0, 0)])
            models = os.path.join(d, "models")
            os.makedirs(models)
            for n in ("1x_DEDXT.pth", "4x-GameAI-2-0.safetensors"):
                open(os.path.join(models, n), "w").close()
            fake = os.path.join(d, "fake_infer.py")
            with open(fake, "w") as f:
                f.write(FAKE_INFER % os.path.join(ROOT, "tools"))
            out = os.path.join(d, "pack")
            tu.main(["--dump", dump, "--out", out, "--backend", "spandrel", "--chain", "1x-DEDXT,4x-GameAI-2-0",
                     "--model-dir", models, "--python", sys.executable, "--infer", fake, "--pad", "4"])
            pack = os.path.join(out, "1x-DEDXT+4x-GameAI-2-0")
            up = tu.read_png(os.path.join(pack, "00000000000000b1.png"))
            self.assertTrue(np.array_equal(up, np.repeat(np.repeat(src["00000000000000b1"], 4, axis=0), 4, axis=1)))
            self.assertTrue(os.path.exists(os.path.join(pack, "00000000000000b2.png")))
            with open(os.path.join(d, "argv.json")) as f:
                argv = json.load(f)
            paths = [argv[i + 1] for i, a in enumerate(argv) if a == "--model"]
            self.assertEqual([os.path.basename(p) for p in paths], ["1x_DEDXT.pth", "4x-GameAI-2-0.safetensors"])
            self.assertEqual([n for n in os.listdir(out) if n.startswith(".")], [])   # work dir cleaned up


if __name__ == "__main__":
    unittest.main()
