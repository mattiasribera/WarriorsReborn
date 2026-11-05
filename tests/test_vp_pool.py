"""Runs the game-independent vertex-program pool test (runtime/build/vp_pool_test).

The test binary is runtime/tests/vp_pool_test.c linked with the HLE D3D sources: one sequence of
program draws (1 to 6000 vertices, inputs and constants rewritten after every draw, a frame that
overflows the pool's job queue) runs with --vp-threads 0, 1, 2, 3, 7 and auto in child processes,
and every draw's output must be byte-identical to the synchronous run. It also checks that
single-batch draws run on the recording thread and the default pool size (physical cores - 2).
It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "vp_pool_test")


class VpPoolTest(unittest.TestCase):
    def test_vp_pool(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/vp_pool_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=300)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
