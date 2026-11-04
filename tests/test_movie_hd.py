"""Runs the game-independent --movie-hd test (runtime/build/movie_hd_test).

Checks the _hd file-name rule, the Bink header check, the 16:9 fit rectangle, the GPU texel
coordinates and the bilinear YUV quad.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "movie_hd_test")


class MovieHdTest(unittest.TestCase):
    def test_movie_hd_helpers(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/movie_hd_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
