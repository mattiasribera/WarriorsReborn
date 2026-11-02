"""Runs the game-independent --shadow-range test (runtime/build/shadow_range_test).

The test binary is tools/shadow_range_test.c linked with src/shadow_range.c: the option parsing
(--shadow-range, --no-shadow-range, --shadow-range-dist 0.5-50, rejected values leave the setting
unchanged) and the fit of the light projection (r = A q - B) from a draw's depth-stage
coordinates, which rejects orthographic, flat and inconsistent draws. It needs no game data; the
renderer side is covered by renderer_test (test_shadow_range).
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "shadow_range_test")


class ShadowRangeTest(unittest.TestCase):
    def test_shadow_range(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/shadow_range_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)

    def test_invalid_distance_exits_2(self):
        xbrun = os.path.join(ROOT, "runtime", "build", "xbrun")
        if not os.path.exists(xbrun):
            self.skipTest("runtime/build/xbrun not built")
        for bad in (["--shadow-range-dist", "0.1"], ["--shadow-range-dist", "x"], ["--shadow-range-dist"]):
            res = subprocess.run([xbrun, "default.xbe", "disc", *bad], capture_output=True, text=True, timeout=30)
            self.assertEqual(res.returncode, 2, bad)


if __name__ == "__main__":
    unittest.main()
