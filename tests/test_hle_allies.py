"""Runs the game-independent --smart-allies test (runtime/build/hle_allies_test).

It checks the crew score and circle rules and the option parser, then plants both call sites
(the target score call and the ring-action call) at their guest addresses with stand-in callees,
installs the patches and runs them: crew choosers get the new scores and ring waits, enemies keep
the title's values, and with the option off no byte changes. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_allies_test")


class HleAlliesTest(unittest.TestCase):
    def test_hle_allies(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_allies_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
