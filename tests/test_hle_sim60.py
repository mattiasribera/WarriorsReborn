"""Runs the game-independent sim60 patch test (runtime/build/hle_sim60_test).

The test binary is tools/hle_sim60_test.c linked with src/hle_sim60.c: a host buffer stands in
for guest memory. The patch set is written only with --sim60 and only as a whole (one
mismatching or malformed entry writes nothing and is reported), and every entry of the title's
table applies to a planted copy of its original bytes. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_sim60_test")


class HleSim60Test(unittest.TestCase):
    def test_hle_sim60(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_sim60_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
