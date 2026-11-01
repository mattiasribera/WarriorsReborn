"""Runs the game-independent guest memory placement test (runtime/build/memmap_test).

plat_map_fixed must never put guest pages inside a range another part of the process reserved
(on Windows the AMD Vulkan driver reserves 32 MB ranges and later commits or frees pieces of
them), must map free ranges, report unmapped pages as unmapped and map an unmapped range again.
It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "memmap_test")


class MemmapTest(unittest.TestCase):
    def test_guest_pages_stay_out_of_foreign_reservations(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/memmap_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
