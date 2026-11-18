"""Runs the game-independent memory-registry test (runtime/build/hle_memreg_test).

The test binary is tools/hle_memreg_test.c linked with src/hle_memreg.c: a host buffer stands
in for the title's memory manager set-up. The registry's node count (512 as shipped) is raised
with every other byte kept, a second pass or a changed byte writes nothing, and the used /
capacity read-back follows the pool pointer. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_memreg_test")


class HleMemregTest(unittest.TestCase):
    def test_hle_memreg(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_memreg_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
