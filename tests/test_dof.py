"""Runs the game-independent --cutscene-dof test (runtime/build/dof_test).

The test binary is runtime/tests/dof_test.c linked with src/dof.c: the option parsing
(--cutscene-dof, --no-cutscene-dof, --cutscene-dof-strength 0.25-2; rejected values leave the
setting unchanged), the parameters per strength, the bars gate (0.3 s fades in and out, a new
cutscene flagged once), the camera-cut test on the title's view matrix, the focus pick (the
character in the centre wins over the wall behind, nothing drawn gives no meter), its smoothing
and snap, and the circle of confusion. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "dof_test")


class DofTest(unittest.TestCase):
    def test_dof(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/dof_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
