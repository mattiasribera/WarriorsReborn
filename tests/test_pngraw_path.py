"""Game-independent tests of how the texture tools find the pngraw PNG decoder: pngraw.exe on
Windows (next to the tool, then runtime/build), pngraw elsewhere, and the --pngraw option of
tools/texture_upscale.py, tools/spandrel_infer.py and tools/texture_compress.py. Skipped when
numpy is not installed.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    import numpy as np
except ImportError:
    np = None
if np is not None:
    import spandrel_infer as si
    import texture_compress as tc
    import texture_upscale as tu


def touch(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").close()


@unittest.skipIf(np is None, "numpy not installed")
class FindPngrawTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        self.root = self.d.name
        self.tools = os.path.join(self.root, "tools")
        self.build = os.path.join(self.root, "runtime", "build")

    def tearDown(self):
        self.d.cleanup()

    def find(self, platform):
        return tu.find_pngraw(platform=platform, tool_dir=self.tools, root=self.root)

    def test_windows_prefers_exe_next_to_the_tool(self):
        touch(os.path.join(self.tools, "pngraw.exe"))
        touch(os.path.join(self.build, "pngraw.exe"))
        self.assertEqual(self.find("win32"), os.path.join(self.tools, "pngraw.exe"))

    def test_windows_falls_back_to_runtime_build(self):
        touch(os.path.join(self.build, "pngraw.exe"))
        self.assertEqual(self.find("win32"), os.path.join(self.build, "pngraw.exe"))

    def test_windows_ignores_the_linux_binary(self):
        touch(os.path.join(self.build, "pngraw"))
        self.assertEqual(self.find("win32"), os.path.join(self.build, "pngraw.exe"))

    def test_linux_uses_runtime_build_without_exe(self):
        touch(os.path.join(self.build, "pngraw"))
        touch(os.path.join(self.build, "pngraw.exe"))
        self.assertEqual(self.find("linux"), os.path.join(self.build, "pngraw"))

    def test_linux_also_accepts_a_binary_next_to_the_tool(self):
        touch(os.path.join(self.tools, "pngraw"))
        self.assertEqual(self.find("linux"), os.path.join(self.tools, "pngraw"))

    def test_missing_returns_the_runtime_build_path(self):
        self.assertEqual(self.find("win32"), os.path.join(self.build, "pngraw.exe"))
        self.assertEqual(self.find("linux"), os.path.join(self.build, "pngraw"))

    def test_missing_message_names_the_path_and_the_option(self):
        msg = tu.missing_pngraw("C:/x/pngraw.exe")
        self.assertIn("C:/x/pngraw.exe", msg)
        self.assertIn("--pngraw", msg)

    def test_venv_python_matches_the_platform(self):
        self.assertEqual(tu.default_python("win32", "H"), os.path.join("H", "tools", "upscale-venv", "Scripts", "python.exe"))
        self.assertEqual(tu.default_python("linux", "H"), os.path.join("H", "tools", "upscale-venv", "bin", "python"))


@unittest.skipIf(np is None, "numpy not installed")
class PngrawOptionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/pngraw"], check=True)

    def setUp(self):
        self.saved = tu.PNGRAW
        self.d = tempfile.TemporaryDirectory()
        self.bogus = os.path.join(self.d.name, "no", "pngraw.exe")

    def tearDown(self):
        tu.PNGRAW = self.saved
        self.d.cleanup()

    def test_upscale_rejects_a_missing_pngraw_option(self):
        with self.assertRaises(SystemExit) as cm:
            tu.main(["--pngraw", self.bogus, "--dump", self.d.name, "--out", self.d.name])
        self.assertIn(self.bogus, str(cm.exception.code))

    def test_upscale_option_defaults_to_the_resolved_decoder(self):
        self.assertEqual(tu.build_parser().parse_args([]).pngraw, tu.PNGRAW)

    def test_spandrel_rejects_a_missing_pngraw_option(self):
        with self.assertRaises(SystemExit) as cm:
            si.main(["-i", self.d.name, "-o", self.d.name, "--model", "a", "--pngraw", self.bogus],
                    load=lambda path, device, half: (None, 1))
        self.assertIn(self.bogus, str(cm.exception.code))

    def test_spandrel_decodes_with_the_given_pngraw(self):
        moved = os.path.join(self.d.name, "bin", "pngraw")
        os.makedirs(os.path.dirname(moved))
        shutil.copy2(self.saved, moved)
        indir, outdir = os.path.join(self.d.name, "in"), os.path.join(self.d.name, "out")
        os.makedirs(indir)
        os.makedirs(outdir)
        img = np.full((4, 4, 3), 7, dtype=np.uint8)
        tu.write_png(os.path.join(indir, "t.png"), img)
        si.main(["-i", indir, "-o", outdir, "--model", "a", "--pngraw", moved],
                load=lambda path, device, half: (lambda x: x, 1))
        self.assertEqual(tu.PNGRAW, moved)
        self.assertTrue(os.path.exists(os.path.join(outdir, "t.png")))

    def test_compress_option_defaults_to_the_resolved_decoder(self):
        self.assertEqual(tc.build_parser().parse_args(["--pack", "p", "--out", "o"]).pngraw, tu.PNGRAW)


@unittest.skipIf(shutil.which("zig") is None, "zig not installed")
class PngrawExeBuildTest(unittest.TestCase):
    def test_windows_build_is_a_32_bit_console_exe(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/pngraw.exe"], check=True)
        with open(os.path.join(ROOT, "runtime", "build", "pngraw.exe"), "rb") as f:
            data = f.read(4096)
        self.assertEqual(data[:2], b"MZ")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        self.assertEqual(data[pe:pe + 4], b"PE\0\0")
        self.assertEqual(struct.unpack_from("<H", data, pe + 4)[0], 0x14C)          # IMAGE_FILE_MACHINE_I386
        self.assertEqual(struct.unpack_from("<H", data, pe + 24 + 68)[0], 3)        # IMAGE_SUBSYSTEM_WINDOWS_CUI


if __name__ == "__main__":
    unittest.main()
