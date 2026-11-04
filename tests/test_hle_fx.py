"""Runs the game-independent effect-pool test (runtime/build/hle_fx_test).

The test binary is src/hle_fx.c built with HLE_FX_TEST: it reads planted pool objects the way the
--stats [fx] line does, plants the patched sites at their guest addresses, installs the patches
and runs the stubs with the guest's registers. Sites with unexpected bytes must stay untouched.
It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_fx_test")


class HleFxTest(unittest.TestCase):
    def test_hle_fx(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_fx_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
