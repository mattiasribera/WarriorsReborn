"""Runs the game-independent kernel file I/O test (runtime/build/kfile_test).

The test binary is src/kfile.c built with KFILE_TEST: it maps a temporary host directory as
the Xbox hard disk, opens directories through NtOpenFile and enumerates them with
NtQueryDirectoryFile (masks, restart, empty directory, small buffers), and checks that path
mapping refuses '..' components. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "kfile_test")


class KfileTest(unittest.TestCase):
    def test_kfile_directory_query(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/kfile_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
