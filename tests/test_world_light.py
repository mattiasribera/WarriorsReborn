"""Runs the game-independent --world-lighting test (runtime/build/world_light_test).

The test binary is runtime/tests/world_light_test.c linked with src/world_light.c: the option
parsing (--world-lighting, --world-lighting-strength, --no-world-lighting), the vertex-program
classifier (world, object and the programs that are neither, encoded with the NV2A field table),
the view normals and box of a recorded draw, and the per-draw light mask. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "world_light_test")


class WorldLightTest(unittest.TestCase):
    def test_world_light(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/world_light_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
