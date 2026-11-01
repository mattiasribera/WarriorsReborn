"""Runs the game-independent frame pacing test (runtime/build/pacing_test).

The test binary is tools/pacing_test.c linked with src/pacing.c: the cadence statistics behind
the `[pacing]` line of `--stats` are fed synthetic step counters (even pacing, a slipping phase,
odd steps, resets and unreadable frames), and the frame-locked clock behind
QueryPerformanceCounter runs against a fake host clock (on schedule, lag beyond a period,
recovery, slow and bursting presents, monotonic reads). It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "pacing_test")


class PacingTest(unittest.TestCase):
    def test_pacing(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/pacing_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
