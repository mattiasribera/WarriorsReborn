"""Runs the game-independent renderer tests (runtime/build/renderer_test, geom_test).

renderer_test initializes the Vulkan renderer offscreen (no display needed) and exits 77 when no
Vulkan loader or device is available, which skips the test instead of failing it.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "runtime", "build")
SKIP = 77


def run(name):
    subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/" + name], check=True)
    return subprocess.run([os.path.join(BUILD, name)], capture_output=True, text=True, timeout=120)


class RendererTest(unittest.TestCase):
    def check(self, res):
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)

    def test_geometry_helpers(self):
        self.check(run("geom_test"))

    def test_vulkan_clear_readback(self):
        res = run("renderer_test")
        if res.returncode == SKIP:
            self.skipTest(res.stdout.strip())
        self.check(res)


if __name__ == "__main__":
    unittest.main()
