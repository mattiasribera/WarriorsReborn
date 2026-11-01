"""Runs the game-independent spawn scaling test (runtime/build/hle_spawn_test).

The test binary is src/hle_spawn.c built with HLE_SPAWN_TEST: it checks the scaling and clamping
arithmetic and the --spawn-scale value parser, then plants copies of the four patched sites
(GangAddSpawner's and GangSetMaxConcurrent's max-alive stores, SetSpawnMax, SetCopSpawnMax) at
their guest addresses, installs the patches and runs them with the guest's registers and
calling convention. Scale 1 must leave every byte unchanged. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_spawn_test")


class HleSpawnTest(unittest.TestCase):
    def test_hle_spawn(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_spawn_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
