"""Runs the game-independent host input layer test (runtime/build/input_map_test).

The test binary is src/input_map.c built with INPUT_MAP_TEST over src/hle_input.c's pad state:
it feeds synthetic key event sequences (X11 auto-repeat pairs, SDL repeat flags) and checks the
held state, the key bindings, the mouse-look mapping and the key map config parser. It needs
no game data and no display. It also checks that the shipped example key map file parses
without errors and restates the built-in defaults.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "input_map_test")
EXAMPLE_CFG = os.path.join(ROOT, "runtime", "warriors-input.cfg.example")


class InputMapTest(unittest.TestCase):
    def test_input_map(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/input_map_test"], check=True)
        res = subprocess.run([TEST_BIN, EXAMPLE_CFG], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
