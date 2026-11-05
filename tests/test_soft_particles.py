"""Runs the game-independent --soft-particles test (runtime/build/soft_particles_test).

The test binary is tools/soft_particles_test.c linked with src/soft_particles.c: the option
parsing (--soft-particles, --no-soft-particles, --soft-particles-distance 0.05-4, rejected values
leave the setting unchanged), the signature of the title's world particle draws (FVF 0x142,
indexed triangle list, depth test on, depth write off, alpha or additive blending) and the rule
that softens only view-plane billboards, never ground decals or wall tags, and the world-fixed
sprites (a tag seen head-on stays hard once its sprite was seen off the view plane). It needs no game
data; the renderer side is covered by renderer_test (test_soft_particles).
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "soft_particles_test")


class SoftParticlesTest(unittest.TestCase):
    def test_soft_particles(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/soft_particles_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
