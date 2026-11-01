"""Runs the game-independent XAPI timing test (runtime/build/hle_xapi_test).

The test binary maps the XBE's XAPI address range, plants the entry bytes of the statically
linked QueryPerformanceCounter, installs the HLE patch and calls it with the guest calling
convention. It checks that the counter ticks at the Xbox CPU clock (733333333 Hz, the value
XAPI's QueryPerformanceFrequency returns) instead of the host time-stamp counter rate, so it
needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_xapi_test")


class XapiTimingTest(unittest.TestCase):
    def test_performance_counter_runs_at_xbox_cpu_clock(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_xapi_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
