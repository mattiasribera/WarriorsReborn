"""Runs the game-independent texture dump / replacement pack test (runtime/build/texpack_test).

Checks the texture content hash (level 0 texels; stable, independent of addresses, linear row
padding and the mip count), PNG write/read, mip chain generation, the replacement pack index and
loader, the key -> hash memo and the dump directory with its manifest. Uses a temporary
directory only; no game data.
"""
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "texpack_test")


class TexturePackTest(unittest.TestCase):
    def test_texture_pack_helpers(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/texpack_test"], check=True)
        with tempfile.TemporaryDirectory() as tmp:
            res = subprocess.run([TEST_BIN, tmp], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
