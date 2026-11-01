"""Runs the game-independent rate-fix test (runtime/build/hle_ratefix_test).

The test binary is tools/hle_ratefix_test.c linked with src/hle_ratefix.c: a host buffer stands
in for guest memory. A fix whose signature matches is applied, a mismatching one is left
untouched and reported as skipped, --no-rate-fixes disables every fix, and each entry of the
title's fix table applies to a planted copy of its instruction bytes. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_ratefix_test")


class HleRatefixTest(unittest.TestCase):
    def test_hle_ratefix(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_ratefix_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
