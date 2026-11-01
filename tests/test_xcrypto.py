"""Runs the game-independent kernel crypto test (runtime/build/kxc_test).

The test binary is src/kxc.c built with KXC_TEST: it checks XcSHAInit/XcSHAUpdate/XcSHAFinal
against the RFC 3174 SHA-1 vectors and XcHMAC against the RFC 2202 HMAC-SHA1 vectors (cases
1-5, whole and split across the two data buffers), plus the Xbox truncation of keys longer
than 64 bytes. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "kxc_test")


class XcryptoTest(unittest.TestCase):
    def test_sha1_and_hmac_vectors(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/kxc_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
