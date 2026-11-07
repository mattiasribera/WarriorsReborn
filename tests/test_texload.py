"""Runs the game-independent texture pack load queue test (runtime/build/texload_test).

The test binary is runtime/tests/texload_test.c linked with src/texload.c: one slot per hash and
kind, demand loads before prefetches, promotion of a drawn prefetch, the staged-bytes cap, dropping
images nobody takes, the per-frame upload budget (bytes and ms, the first upload always goes),
failures, and slots being loaded never dropped. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "texload_test")


class TexLoadTest(unittest.TestCase):
    def test_texload(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/texload_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
