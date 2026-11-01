"""Runs the game-independent --warp test (runtime/build/hle_warp_test).

The test binary is tools/hle_warp_test.c linked with src/hle_warp.c: option parsing and
validation, the mission -> level table, the all-or-nothing signature check on a planted buffer,
and both stubs executed in host executable memory against fake title functions (one-shot level
redirect with the checkpoint call, blocked profile write). It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_warp_test")


class HleWarpTest(unittest.TestCase):
    def test_hle_warp(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_warp_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
