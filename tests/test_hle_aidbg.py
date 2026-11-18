"""Runs the game-independent --ai-debug core test (runtime/build/hle_aidbg_test).

The test binary is src/hle_aidbg.c built with HLE_AIDBG_TEST: a fake guest (brain table, character
pool, handle table, positions, bodies, vtables followed by class names, goal and action objects)
is read through the runtime's read callback; it checks class names, handle resolution, the crew /
enemy classification, goal targets and the idle/engaged/damage statistics. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_aidbg_test")


class HleAidbgTest(unittest.TestCase):
    def test_hle_aidbg(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_aidbg_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
