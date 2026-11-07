"""Runs the game-independent --neon-glow test (runtime/build/neon_glow_test).

The test binary is runtime/tests/neon_glow_test.c linked with src/neon_glow.c (and texpack for PNG
reading): the options, the <hash>.emit.png / .emit.dds mask names, the mask index over temporary
packs (first pack wins, the .dds wins within a pack, mask-only packs, texpack ignoring the masks),
grey PNG and BC4 .emit.dds loading into an L8 mip chain, the BC4 decoder and the per-sign flicker.
It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "neon_glow_test")


class NeonGlowTest(unittest.TestCase):
    def test_neon_glow(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/neon_glow_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
