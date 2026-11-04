"""Runs the game-independent draw-distance test (runtime/build/hle_drawdist_test).

The test binary is tests/hle_drawdist_test.c linked with src/hle_drawdist.c: a host buffer with
the shipped bytes planted at each patch site stands in for guest memory. It checks the option
parsing, the default (steady fog only), --draw-distance / --prop-detail writes, and that a patch
whose bytes differ at any site writes nothing. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_drawdist_test")


class HleDrawdistTest(unittest.TestCase):
    def test_hle_drawdist(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_drawdist_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
