"""Game-independent tests of tools/spandrel_infer.py without torch: tiled inference with stand-in
models (pure numpy callables) must match whole-image inference, chains multiply the scale, and
the CLI reads and writes PNG directories. Skipped when numpy is not installed.
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
except ImportError:
    np = None
if np is not None:
    import spandrel_infer as si
    import texture_upscale as tu


def nearest(scale):
    return lambda img: np.repeat(np.repeat(img, scale, axis=0), scale, axis=1)


def box3(img):
    """1x stand-in with a 3x3 receptive field (edge-clamped), so tile borders matter."""
    p = np.pad(img, ((1, 1), (1, 1), (0, 0)), mode="edge")
    return sum(p[dy:dy + img.shape[0], dx:dx + img.shape[1]] for dy in range(3) for dx in range(3)) / 9.0


@unittest.skipIf(np is None, "numpy not installed")
class SpandrelInferTest(unittest.TestCase):
    def test_tiled_matches_whole_image(self):
        rng = np.random.default_rng(2)
        img = rng.random((37, 53, 3), dtype=np.float32)
        for fn, scale in ((nearest(4), 4), (box3, 1)):
            whole = fn(img)
            tiled = si.tile_apply(img, fn, scale, tile=16, overlap=4)
            self.assertEqual(tiled.shape, (37 * scale, 53 * scale, 3))
            self.assertTrue(np.allclose(tiled, whole, atol=1e-6))

    def test_small_image_is_one_call(self):
        calls = []

        def fn(img):
            calls.append(img.shape)
            return img
        si.tile_apply(np.zeros((8, 8, 3), np.float32), fn, 1, tile=16, overlap=4)
        self.assertEqual(calls, [(8, 8, 3)])

    def test_chain_multiplies_scale_and_rounds(self):
        img = np.array([[[0, 128, 255]]], dtype=np.uint8)
        out = si.run_chain(img, [(lambda x: x * 0.5, 1), (nearest(4), 4)], tile=16, overlap=4)
        self.assertEqual(out.shape, (4, 4, 3))
        self.assertEqual(out.dtype, np.uint8)
        self.assertEqual(out[0, 0].tolist(), [0, 64, 128])

    def test_cli_with_stand_in_models(self):
        with tempfile.TemporaryDirectory() as d:
            indir, outdir = os.path.join(d, "in"), os.path.join(d, "out")
            os.makedirs(indir)
            os.makedirs(outdir)
            img = np.random.default_rng(3).integers(0, 256, (6, 5, 3), dtype=np.uint8)
            tu.write_png(os.path.join(indir, "t.png"), img)
            si.main(["-i", indir, "-o", outdir, "--model", "a", "--model", "b"],
                    load=lambda path, device, half: (nearest(2), 2))
            out = tu.read_png(os.path.join(outdir, "t.png"))
            self.assertEqual(out.shape, (24, 20, 4))
            self.assertTrue(np.array_equal(out[..., :3], np.repeat(np.repeat(img, 4, axis=0), 4, axis=1)))

    def test_skip_existing_outputs(self):
        with tempfile.TemporaryDirectory() as d:
            indir, outdir = os.path.join(d, "in"), os.path.join(d, "out")
            os.makedirs(indir)
            os.makedirs(outdir)
            img = np.zeros((4, 4, 3), np.uint8)
            tu.write_png(os.path.join(indir, "done.png"), img)
            tu.write_png(os.path.join(indir, "todo.png"), img)
            tu.write_png(os.path.join(outdir, "done.png"), np.full((2, 2, 3), 7, np.uint8))
            seen = []

            def fn(x):
                seen.append(x.shape)
                return nearest(2)(x)
            si.main(["-i", indir, "-o", outdir, "--model", "a", "--skip-existing"], load=lambda p, dev, half: (fn, 2))
            self.assertEqual(len(seen), 1)
            self.assertEqual(tu.read_png(os.path.join(outdir, "done.png")).shape, (2, 2, 4))   # untouched
            self.assertEqual(tu.read_png(os.path.join(outdir, "todo.png")).shape, (8, 8, 4))

    def test_out_of_memory_halves_tile_and_failures_continue(self):
        with tempfile.TemporaryDirectory() as d:
            indir, outdir = os.path.join(d, "in"), os.path.join(d, "out")
            os.makedirs(indir)
            os.makedirs(outdir)
            rng = np.random.default_rng(4)
            img = rng.integers(0, 256, (20, 20, 3), dtype=np.uint8)
            tu.write_png(os.path.join(indir, "big.png"), img)
            tu.write_png(os.path.join(indir, "bad.png"), np.zeros((3, 3, 3), np.uint8))
            tiles = []

            def fn(x):
                if x.shape[0] == 3:
                    raise ValueError("broken input")
                tiles.append(max(x.shape[:2]))
                if max(x.shape[:2]) > 12:
                    raise RuntimeError("CUDA out of memory. Tried to allocate 2.00 GiB")
                return nearest(2)(x)
            rc = si.main(["-i", indir, "-o", outdir, "--model", "a", "--tile", "32", "--overlap", "2"],
                         load=lambda p, dev, half: (fn, 2))
            self.assertEqual(rc, 0)
            self.assertFalse(os.path.exists(os.path.join(outdir, "bad.png")))          # failed, others go on
            out = tu.read_png(os.path.join(outdir, "big.png"))
            self.assertTrue(np.array_equal(out[..., :3], np.repeat(np.repeat(img, 2, 0), 2, 1)))
            self.assertLessEqual(tiles[-1], 12)                                         # tile was halved

    def test_importable_without_torch(self):
        res = subprocess.run([sys.executable, "-c", "import sys; sys.path.insert(0, %r); import spandrel_infer; "
                              "print('torch' in sys.modules)" % os.path.join(ROOT, "tools")],
                             capture_output=True, text=True, check=True)
        self.assertEqual(res.stdout.strip(), "False")


if __name__ == "__main__":
    unittest.main()
