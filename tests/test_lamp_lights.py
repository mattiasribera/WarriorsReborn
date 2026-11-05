"""Runs the game-independent --lamp-lights test (runtime/build/lamp_lights_test).

The test binary is runtime/tests/lamp_lights_test.c linked with src/lamp_lights.c: the option
parsing (--lamp-lights, --no-lamp-lights, --lamp-lights-strength 0.1-3, rejected values leave the
setting unchanged), which halo quads are lights (lamp halos and cones, not sparks, smoke or haze),
the merging of one lamp's quads, the tracker (stable ids under jitter and reordering, fade in and
out, the off-screen hold, the cap with hysteresis) and the falloff. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "lamp_lights_test")


class LampLightsTest(unittest.TestCase):
    def test_lamp_lights(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/lamp_lights_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
