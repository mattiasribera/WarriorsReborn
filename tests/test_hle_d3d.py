"""Runs the game-independent HLE D3D test (runtime/build/hle_d3d_test).

The test binary maps the XBE's D3D8 library address range itself, plants the expected entry
bytes and uses the null video backend, so it needs no game data and no display.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_d3d_test")
BMP = os.path.join(ROOT, "runtime", "build", "hle_d3d_test.bmp")


class HleD3dTest(unittest.TestCase):
    def test_hle_d3d_boundary(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_d3d_test"], check=True)
        res = subprocess.run([TEST_BIN, BMP], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)
        with open(BMP, "rb") as f:
            self.assertEqual(f.read(2), b"BM")


if __name__ == "__main__":
    unittest.main()
