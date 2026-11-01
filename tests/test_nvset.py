"""Runs the game-independent non-volatile settings test (runtime/build/nvset_test).

The kernel answers ExQueryNonVolatileSetting from src/nvset.c; the test checks the defaults
(English, NTSC-M, North America, 4:3) that --widescreen sets the flag XGetVideoFlags reads, and that --language maps
en/es/fr/de/it to XC_LANGUAGE (and rejects anything else).
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "nvset_test")


class NvSettingsTest(unittest.TestCase):
    def test_settings(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/nvset_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual([line for line in res.stdout.splitlines() if line.startswith("FAIL")], [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
