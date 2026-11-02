"""Runs the game-independent --ssao test (runtime/build/ssao_test).

The test binary is tools/ssao_test.c linked with src/ssao.c: the option parsing (--ssao,
--no-ssao, --ssao-strength 0.1-2, --ssao-radius 0.1-8, rejected values leave the setting
unchanged), the rule that ends the opaque 3D world for the renderer's ambient occlusion split
(the last depth-tested, depth-writing back-buffer draw after the last depth clear) and the
projection scale read from a world-view-projection matrix. It needs no game data; the renderer
side is covered by renderer_test (test_ssao).
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "ssao_test")


class SsaoTest(unittest.TestCase):
    def test_ssao(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/ssao_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
