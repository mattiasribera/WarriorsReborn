"""Runs the game-independent HDR10 / saturation test (runtime/build/hdr_test).

The test binary is runtime/tests/hdr_test.c linked with src/hdr.c: the option parsing (--hdr
off|auto|on, --hdr-peak 400-10000, --hdr-paper-white 80-500, --saturation 0.5-2), the PQ (SMPTE
ST 2084) encoding of known values, BT.709 -> BT.2020, saturation, the highlight expansion, the HDR
pixel and the swapchain surface format choice. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hdr_test")


class HdrTest(unittest.TestCase):
    def test_hdr(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hdr_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
