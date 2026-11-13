"""Runs the game-independent --lamp-shadows test (runtime/build/lamp_shadow_test).

The test binary is tests/lamp_shadow_test.c linked with src/lamp_shadow.c: the option parsing
(--lamp-shadows, --no-lamp-shadows, --lamp-shadows-strength 0.1-1, rejected values leave the setting
unchanged) and the renderer input built from the frame's light projection and lights (inverse
projection, r = A q - B, the nearest lights in the depth stage's coordinates). It needs no game data;
the renderer side is covered by renderer_test (test_lamp_shadows).
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "lamp_shadow_test")


class LampShadowTest(unittest.TestCase):
    def test_lamp_shadow(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/lamp_shadow_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)

    def test_invalid_strength_exits_2(self):
        xbrun = os.path.join(ROOT, "runtime", "build", "xbrun")
        if not os.path.exists(xbrun):
            self.skipTest("runtime/build/xbrun not built")
        for bad in (["--lamp-shadows-strength", "0"], ["--lamp-shadows-strength", "1.5"], ["--lamp-shadows-strength", "x"],
                    ["--lamp-shadows-strength"]):
            res = subprocess.run([xbrun, "default.xbe", "disc", *bad], capture_output=True, text=True, timeout=30)
            self.assertEqual(res.returncode, 2, bad)


if __name__ == "__main__":
    unittest.main()
