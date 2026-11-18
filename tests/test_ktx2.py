"""Runs the KTX2 (Basis Universal) transcoding test (runtime/build/ktx2_test).

The synthetic fixtures in runtime/tests/ktx2 (made by runtime/tests/ktx2_fixtures.py; no game data)
must come back as BC1/BC3/BC7 with the full mip chain, keeping alpha-test shapes, the height in a
normal map's alpha and all four channels of a 4-channel map; damaged files are refused.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "ktx2_test")


class Ktx2Test(unittest.TestCase):
    def test_ktx2_transcoding(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/ktx2_test"], check=True)
        res = subprocess.run([TEST_BIN, os.path.join(ROOT, "runtime", "tests", "ktx2")], capture_output=True, text=True, timeout=120)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual([l for l in res.stdout.splitlines() if l.startswith("FAIL")], [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
