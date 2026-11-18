"""Runs the game-independent --auto-exposure test (runtime/build/auto_exposure_test).

The test binary is runtime/tests/auto_exposure_test.c linked with src/auto_exposure.c: the option
parsing (--auto-exposure, --no-auto-exposure, --auto-exposure-strength 0-2; rejected values leave
the setting unchanged), the parameters per strength, the adaptation step the GPU runs (no gain at
the key, about 0.6 s up and 2.5 s down, a lightning flash moves the gain under 0.1 stop, a big
meter change glides and only the caller's camera-cut snap jumps, the hideout turn without wind-up,
nothing metered holds, -0.4 to +0.5 stop) and the highlight roll-off. It needs
no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "auto_exposure_test")


class AutoExposureTest(unittest.TestCase):
    def test_auto_exposure(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/auto_exposure_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
