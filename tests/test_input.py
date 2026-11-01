"""Runs the game-independent HLE input test (runtime/build/hle_input_test).

The test binary is src/hle_input.c built with HLE_INPUT_TEST: it maps the XBE's XPP (XAPI
input) address range itself, plants the expected entry bytes, installs the patches and calls
them with the guest calling convention, so it needs no game data. It also checks the host input
state (keyboard mapping, controller merge, packet numbers) and the --input-script parser.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_input_test")


class HleInputTest(unittest.TestCase):
    def test_hle_input_boundary(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_input_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
