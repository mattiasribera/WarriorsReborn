"""Runs the game-independent XACT 3D positioning test (runtime/build/audio3d_test).

DirectSound 3D distance attenuation and stereo pan helpers, and the HLE listener/source
setters driving a synthetic 3D cue through the mixer. Needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "audio3d_test")


class Audio3dTest(unittest.TestCase):
    def test_audio3d(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/audio3d_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
