"""Runs the game-independent HUD glass test (runtime/build/hud_glass_test).

The test binary is tools/hud_glass_test.c linked with src/hud_glass.c: the HUD bar recognizer on
quads that reproduce the recorded rage meter draw (player panel) (one bar, its strip quads marked, the
backing quad and other atlas parts not), two bars sharing one draw, draws that are not quads, a
scaled viewport, the alpha fade of the bar quads and the target-pixel box math (margin, scale,
outward rounding, clipping). It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hud_glass_test")


class HudGlassTest(unittest.TestCase):
    def test_hud_glass(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hud_glass_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
