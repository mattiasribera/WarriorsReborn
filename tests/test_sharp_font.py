"""Runs the game-independent --sharp-fonts test (runtime/build/sharp_font_test).

The test binary is runtime/tools/sharp_font_test.c linked with src/sharp_font.c: on synthetic
atlases, white anti-aliased letters (curves, straight and slanted strokes, a thin accent) come out
closer to the shape rendered at 4x than a bilinear upscale, with a narrow edge, the same ink area
and white ink, a faint stroke keeps its level, while coloured icons, grey art, a soft white glow,
an opaque picture and empty space come out exactly as the bilinear upscale the GPU samples today.
Also the atlas hashes, the option and the argument checks. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "sharp_font_test")


class SharpFontTest(unittest.TestCase):
    def test_sharp_font(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/sharp_font_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
