"""Runs the game-independent --filmic test (runtime/build/filmic_test).

The test binary is runtime/tests/filmic_test.c linked with src/filmic.c: the option parsing
(--filmic, --no-filmic, --filmic-strength 0-2, --filmic-grain 0-1, --filmic-vignette 0-1,
--filmic-lut FILE; rejected values leave the setting unchanged), the tonemap (strictly
increasing, black and white points, linear mid-grey kept, identity at strength 0), the
procedural 70s grade and its 3D LUT (monotonic along the grey axis and each primary, black,
white and mid-grey points, warm highlights, teal shadows) and the .cube parser. It needs no game
data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "filmic_test")


class FilmicTest(unittest.TestCase):
    def test_filmic(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/filmic_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
