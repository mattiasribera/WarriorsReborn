"""Runs the text-fix test (runtime/build/hle_textfix_test).

The test binary is tools/hle_textfix_test.c linked with src/hle_textfix.c: a host buffer stands
in for a file. A match inside one read, a match straddling reads at every split point, a read
inside the match, no match, differing bytes at an expected offset, a wrong language, the kill
switch, a scan-mode entry and the title's table are checked. It needs no game data; when
extracted/disc/Main.wad0 exists it also checks the real file (only the two Spanish gym labels
change, and only with Spanish).
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_textfix_test")
MAIN_WAD = os.path.join(ROOT, "extracted", "disc", "Main.wad0")


class HleTextfixTest(unittest.TestCase):
    def run_test(self, *args):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_textfix_test"], check=True)
        res = subprocess.run([TEST_BIN, *args], capture_output=True, text=True, timeout=300)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)
        return res.stdout

    def test_hle_textfix(self):
        self.run_test()

    @unittest.skipUnless(os.path.exists(MAIN_WAD), "needs extracted/disc/Main.wad0 (game data)")
    def test_real_main_wad(self):
        out = self.run_test(MAIN_WAD)
        self.assertIn("es, 4093-byte reads", out)


if __name__ == "__main__":
    unittest.main()
