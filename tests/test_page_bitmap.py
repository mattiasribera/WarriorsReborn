"""Runs the game-independent plat_is_mapped test (runtime/build/page_bitmap_test).

The test binary is runtime/tests/page_bitmap_test.c linked with src/platform/posix.c: guest pages
mapped by plat_map_fixed answer from the page bitmap (also after a partial plat_unmap and a remap),
other mappings through the mincore fallback, and a million guest page checks cost well under a
million system calls. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "page_bitmap_test")


class PageBitmapTest(unittest.TestCase):
    def test_page_bitmap(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/page_bitmap_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=300)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
