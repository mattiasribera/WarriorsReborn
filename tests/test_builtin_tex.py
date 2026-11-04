"""Built-in joint texture (runtime/src/builtin_tex.c, runtime/src/builtin_joint.h).

The generated header must match tools/joint_texture/make_joint.py: regenerated and compared when
numpy and Pillow are available (directly or through uv), otherwise its SHA-256 is compared with
the recorded value. The C test checks the lookup and its priority over texture packs. Original
procedural art only; no game data.
"""
import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "runtime", "src", "builtin_joint.h")
GENERATOR = os.path.join(ROOT, "tools", "joint_texture", "make_joint.py")
TEST_BIN = os.path.join(ROOT, "runtime", "build", "builtin_tex_test")
HEADER_SHA256 = "02db0f46ae719a84814a3f5390bbc8fe0fc5d76e816ffcacee970a802b3195e8"


def generator_python():
    if importlib.util.find_spec("numpy") and importlib.util.find_spec("PIL"):
        return [sys.executable]
    uv = shutil.which("uv")
    if uv:
        probe = [uv, "run", "-q", "--no-project", "--with", "numpy", "--with", "pillow", "python"]
        if subprocess.run(probe + ["-c", "import numpy, PIL"], capture_output=True, timeout=300).returncode == 0:
            return probe
    return None


class BuiltinJointHeaderTest(unittest.TestCase):
    def test_header_hash(self):
        with open(HEADER, "rb") as f:
            self.assertEqual(hashlib.sha256(f.read()).hexdigest(), HEADER_SHA256)

    def test_header_matches_generator(self):
        py = generator_python()
        if not py:
            self.skipTest("numpy/Pillow unavailable; test_header_hash covers the header")
        res = subprocess.run(py + [GENERATOR, "--check", HEADER], capture_output=True, text=True, timeout=300)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)


class BuiltinTextureLookupTest(unittest.TestCase):
    def test_lookup_and_pack_priority(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/builtin_tex_test"], check=True)
        with tempfile.TemporaryDirectory() as tmp:
            res = subprocess.run([TEST_BIN, tmp], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual([line for line in res.stdout.splitlines() if line.startswith("FAIL")], [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
