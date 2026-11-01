"""Runs the game-independent YUV quad test (runtime/build/yuv_test).

Checks the Xbox swizzle/unswizzle helpers, the YUV -> RGB coefficients read from a synthetic
Bink-style pixel shader definition, the BT.601 fallback and the scaled, clipped quad.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "yuv_test")


class YuvTest(unittest.TestCase):
    def test_yuv_helpers(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/yuv_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
