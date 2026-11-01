"""Runs the game-independent shading tests: the NV2A vertex program interpreter
(runtime/build/vsh_test) and the D3DPIXELSHADERDEF conversion (runtime/build/psh_test); the
combiner evaluation itself is checked on the GPU by runtime/build/renderer_test (test_renderer.py).

The microcode is encoded by the tests themselves from the xemu field table, so no game data is
needed.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "runtime", "build")


def run(name):
    subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/" + name], check=True)
    return subprocess.run([os.path.join(BUILD, name)], capture_output=True, text=True, timeout=60)


class ShadingTest(unittest.TestCase):
    def check(self, res):
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)

    def test_vertex_program_interpreter(self):
        self.check(run("vsh_test"))

    def test_pixel_shader_definition(self):
        self.check(run("psh_test"))


if __name__ == "__main__":
    unittest.main()
