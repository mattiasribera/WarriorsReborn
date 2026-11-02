"""Runs the game-independent --bloom test (runtime/build/bloom_test).

The test binary is tools/bloom_test.c linked with src/bloom.c: the option parsing (--bloom,
--no-bloom, --bloom-strength 0.1-3, --bloom-threshold 0-1, rejected values leave the setting
unchanged) and the rule that ends the 3D scene for the renderer's bloom split (the last
depth-tested back-buffer draw). It needs no game data; the renderer side is covered by
renderer_test (test_bloom).
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "bloom_test")


class BloomTest(unittest.TestCase):
    def test_bloom(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/bloom_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
