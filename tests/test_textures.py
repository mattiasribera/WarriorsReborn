"""Runs the game-independent Xbox texture helpers test (runtime/build/xtex_test).

Checks D3DPixelContainer Format/Size parsing, level sizes and offsets, texel conversion of the
8/16/32-bit formats, Morton unswizzling, linear pitches, CPU DXT1/3/5 decoding and the texture
cache key.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "xtex_test")


class TextureTest(unittest.TestCase):
    def test_texture_helpers(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/xtex_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
