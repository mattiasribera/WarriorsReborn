"""Runs the game-independent --skip-intro test (runtime/build/hle_skipintro_test).

The test binary is tools/hle_skipintro_test.c linked with src/hle_skipintro.c: a host buffer
stands in for the title's main. The options are parsed, the three boot movie calls become NOPs
with every other byte kept, a changed byte at any site leaves all three untouched, and a
disabled skip writes nothing. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_skipintro_test")


class HleSkipIntroTest(unittest.TestCase):
    def test_hle_skipintro(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_skipintro_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
