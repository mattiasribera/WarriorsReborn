"""Runs the game-independent --char-lighting test (runtime/build/char_light_test).

The test binary is runtime/tests/char_light_test.c linked with src/char_light.c: the option parsing
(--char-lighting, --no-char-lighting) and the smooth character normals (outward whatever the
winding, welded across seams, the fallback for unusable triangles, smooth on a curved surface). It
needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "char_light_test")


class CharLightTest(unittest.TestCase):
    def test_char_light(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/char_light_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
