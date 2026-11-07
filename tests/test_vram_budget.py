"""Runs the game-independent adaptive texture-pack cache cap test (runtime/build/vram_budget_test).

The test binary is runtime/tests/vram_budget_test.c linked with src/vram_budget.c: the cap's target
from the driver's memory budget and usage (VK_EXT_memory_budget), the 1 GiB / --texpack-budget
maximum and the floor, shrinking under pressure, hysteresis against jitter, the cooldown after a
dropping shrink and growth after several samples in a row. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "vram_budget_test")


class VramBudgetTest(unittest.TestCase):
    def test_vram_budget(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/vram_budget_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
