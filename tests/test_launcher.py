"""Runs the game-independent launcher tests.

launcher/build/launcher_test is launcher/tools/launcher_test.cpp linked with the launcher core:
the profile file round trip (paths with spaces and drive letters, unknown keys and sections
kept), the argv builder for each option with defaults left out, shell quoting, copy validation
against synthetic XBE headers, and a child process with its output in a log file.

The GUI checks run launcher/build/warriors-launcher on a synthetic copy (a fake default.xbe
header and a fake xbrun shell script that records its argv): --smoke draws a few frames with the
SDL dummy video driver, and --play starts the fake xbrun exactly as the Play button does, which
must receive the argv --print-argv reports. --extract-iso extracts a synthetic XDVDFS image and refuses
one whose default.xbe is another title. They are skipped without the SDL2 headers or the
i386 libSDL2. No game data is used.
"""
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(ROOT, "launcher")
BUILD = os.path.join(LAUNCHER, "build")
TEST_BIN = os.path.join(BUILD, "launcher_test")
GUI_BIN = os.path.join(BUILD, "warriors-launcher")
SDL2_HEADER = os.path.expanduser("~/.cache/launcher/sdl/SDL2-2.32.10/i686-w64-mingw32/include/SDL2/SDL.h")
SDL2_LIB = "/usr/lib/i386-linux-gnu/libSDL2-2.0.so.0"

FAKE_XBRUN = """#!/bin/sh
printf '%s\\0' "$0" "$@" > "$(dirname "$0")/recorded-argv"
echo fake xbrun ran
echo fake xbrun stderr >&2
exit 7
"""


def fake_iso(xbe, partition=0):
    """A synthetic XDVDFS image: volume descriptor at sector 32, root folder at 33 with default.xbe
    and a Media folder (sector 34) holding one file."""
    sec = 2048

    def dirent(name, attr, sector, size, right):
        e = struct.pack("<HHIIBB", 0, right, sector, size, attr, len(name)) + name
        return e + b"\0" * (-len(e) % 4)

    media = dirent(b"intro.bik", 0x20, 44, 7, 0)
    first = dirent(b"default.xbe", 0x20, 40, len(xbe), 0)
    root = dirent(b"default.xbe", 0x20, 40, len(xbe), len(first) // 4) + dirent(b"Media", 0x10, 34, len(media), 0)
    img = bytearray(48 * sec)
    vd = b"MICROSOFT*XBOX*MEDIA" + struct.pack("<II", 33, len(root))
    img[32 * sec:32 * sec + len(vd)] = vd
    img[32 * sec + 0x7EC:32 * sec + 0x7EC + 20] = b"MICROSOFT*XBOX*MEDIA"
    img[33 * sec:33 * sec + len(root)] = root
    img[34 * sec:34 * sec + len(media)] = media
    img[40 * sec:40 * sec + len(xbe)] = xbe
    img[44 * sec:44 * sec + 7] = b"BIKdata"
    return bytes(img)


def fake_xbe(title):
    f = bytearray(0x1000)
    f[0:4] = b"XBEH"
    f[0x104:0x108] = struct.pack("<I", 0x10000)
    f[0x118:0x11C] = struct.pack("<I", 0x10178)
    t = title.encode("utf-16-le")
    f[0x178 + 0x0C:0x178 + 0x0C + len(t)] = t
    return bytes(f)


class LauncherCoreTest(unittest.TestCase):
    def test_core(self):
        subprocess.run(["make", "-s", "-C", LAUNCHER, "build/launcher_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)

    def test_keymap_file_matches_the_runtime(self):
        # launcher_test writes the default key map as the Controls page does; the runtime's own
        # input_map_test checks a key map file restates its built-in layout and settings.
        subprocess.run(["make", "-s", "-C", LAUNCHER, "build/launcher_test"], check=True)
        subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/input_map_test"], check=True)
        cfg = os.path.join(BUILD, "test-scratch", "keys-defaults.cfg")
        res = subprocess.run([os.path.join(ROOT, "runtime", "build", "input_map_test"), cfg], capture_output=True,
                             text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("(0 invalid lines)", res.stdout)
        self.assertIn("ALL OK", res.stdout)


@unittest.skipUnless(os.path.exists(SDL2_HEADER) and os.path.exists(SDL2_LIB), "SDL2 headers or i386 libSDL2 missing")
class LauncherGuiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", LAUNCHER, "build/warriors-launcher"], check=True)
        os.makedirs(BUILD, exist_ok=True)
        cls.tmp = tempfile.mkdtemp(prefix="launcher-test ", dir=BUILD)
        copy = os.path.join(cls.tmp, "game copy")
        os.makedirs(os.path.join(copy, "disc"))
        os.makedirs(os.path.join(copy, "hdd"))
        with open(os.path.join(copy, "disc", "default.xbe"), "wb") as f:
            f.write(fake_xbe("The Warriors"))
        cls.xbrun = os.path.join(copy, "xbrun")
        with open(cls.xbrun, "w") as f:
            f.write(FAKE_XBRUN)
        os.chmod(cls.xbrun, 0o755)
        cls.ini = os.path.join(cls.tmp, "profiles.ini")
        with open(cls.ini, "w") as f:
            f.write(f"[launcher]\nselected = stable\n"
                    f"[profile stable]\nxbrun = {cls.xbrun}\ndisc = {copy}/disc\nhdd = {copy}/hdd\n"
                    f"resolution = 1920x1080\nwidescreen = true\naniso = 16\nlanguage = es\n"
                    f"[profile test]\nxbrun = {cls.xbrun}\ndisc = {copy}/disc/\nframe_lock = false\n"
                    f"extra_args = --log-limit 50 \"--x y\"\n"
                    f"[profile legacy]\nxbrun = {cls.xbrun}\ndisc = {copy}/disc\nresolution = 1920x1080\nstats = true\n"
                    f"extra_args = --sim60 --hud-hd-glass --supersample 2 --shadow-scale 2 --shadow-soft --log-limit 5\n")
        with open(cls.ini) as f:
            cls.ini_text = f.read()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_smoke(self):
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        res = subprocess.run([GUI_BIN, "--smoke", self.ini], capture_output=True, text=True, timeout=60, env=env)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("smoke: 3 profiles", res.stdout)
        with open(self.ini) as f:
            self.assertEqual(f.read(), self.ini_text, "smoke must not rewrite an unchanged profiles file")

    def test_play_starts_builder_argv(self):
        for profile in ("stable", "test"):
            with self.subTest(profile=profile):
                printed = subprocess.run([GUI_BIN, "--print-argv", self.ini, profile], capture_output=True,
                                         text=True, timeout=30, check=True).stdout.splitlines()
                res = subprocess.run([GUI_BIN, "--play", self.ini, profile], capture_output=True, text=True, timeout=30)
                self.assertEqual(res.returncode, 7, res.stdout + res.stderr)
                with open(os.path.join(os.path.dirname(self.xbrun), "recorded-argv"), "rb") as f:
                    recorded = f.read().decode().split("\0")[:-1]
                self.assertEqual(recorded, printed)
                log = [line[5:] for line in res.stdout.splitlines() if line.startswith("log: ")][0]
                with open(log) as f:
                    text = f.read()
                os.remove(log)
                self.assertIn("fake xbrun ran\n", text)
                self.assertIn("fake xbrun stderr\n", text)
        self.assertEqual(printed[1:], [os.path.dirname(self.xbrun) + "/disc/default.xbe",
                                       os.path.dirname(self.xbrun) + "/disc",
                                       "--no-frame-lock", "--skip-intro", "--log-limit", "50", "--x y"])

    def test_capture_every_page(self):
        # --capture draws a page off-screen into a PNG and never writes the profiles file.
        pages = ["play", "graphics", "hud", "gameplay", "setup", "advanced", "wizard1", "wizard4", "graphics:1200",
                 "wizard2-iso", "wizard2-extract", "setup-iso", "controls", "controls-es", "padmap", "padmap-save-es", "log",
                 "options@1.5:1320x660", "home-es@1.25:1366x700"]
        for page in pages:
            with self.subTest(page=page):
                png = os.path.join(self.tmp, page.replace(":", "-").replace("@", "-") + ".png")
                res = subprocess.run([GUI_BIN, "--capture", page, png, self.ini], capture_output=True, text=True,
                                     timeout=60)
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
                with open(png, "rb") as f:
                    data = f.read()
                self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
                w, h = struct.unpack(">II", data[16:24])
                size = page.split(":")[1] if ":" in page else "760"
                want = tuple(map(int, size.split("x"))) if "x" in size else (1120, int(size))
                self.assertEqual((w, h), want)
                raw = zlib.decompress(data[41:data.index(b"IEND") - 8])
                self.assertEqual(len(raw), h * (w * 4 + 1))
        with open(self.ini) as f:
            self.assertEqual(f.read(), self.ini_text, "--capture must not write the profiles file")
        res = subprocess.run([GUI_BIN, "--capture", "nope", os.path.join(self.tmp, "x.png"), self.ini],
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(res.returncode, 2)

    def test_extract_iso(self):
        # --extract-iso: the title check first, then every file; a rerun skips what is there.
        good = os.path.join(self.tmp, "good.iso")
        with open(good, "wb") as f:
            f.write(fake_iso(fake_xbe("The Warriors")))
        dest = os.path.join(self.tmp, "extracted", "game")
        for run in range(2):
            res = subprocess.run([GUI_BIN, "--extract-iso", good, dest], capture_output=True, text=True, timeout=60)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("2 files (%d already there)" % (2 * run), res.stdout)
        with open(os.path.join(dest, "default.xbe"), "rb") as f:
            self.assertEqual(f.read(), fake_xbe("The Warriors"))
        with open(os.path.join(dest, "Media", "intro.bik"), "rb") as f:
            self.assertEqual(f.read(), b"BIKdata")
        other = os.path.join(self.tmp, "other.iso")
        with open(other, "wb") as f:
            f.write(fake_iso(fake_xbe("Other Game")))
        res = subprocess.run([GUI_BIN, "--extract-iso", other, os.path.join(self.tmp, "other")], capture_output=True,
                             text=True, timeout=60)
        self.assertEqual(res.returncode, 3, res.stdout + res.stderr)
        self.assertIn("not The Warriors", res.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "other")))
        res = subprocess.run([GUI_BIN, "--extract-iso", self.ini, os.path.join(self.tmp, "x")], capture_output=True,
                             text=True, timeout=60)
        self.assertEqual(res.returncode, 3)
        self.assertIn("not an Xbox disc image", res.stderr)

    def test_legacy_extra_args_keep_the_command(self):
        # A profile from launcher v1 with the options now covered by controls typed in extra_args.
        # It has no skip_intro key, so it loads with Skip intro videos on (the only addition).
        printed = subprocess.run([GUI_BIN, "--print-argv", self.ini, "legacy"], capture_output=True,
                                 text=True, timeout=30, check=True).stdout.splitlines()
        self.assertEqual(printed[3:], ["--resolution", "1920x1080", "--stats", "--sim60", "--hud-hd-glass",
                                       "--supersample", "2", "--shadow-scale", "2", "--shadow-soft",
                                       "--skip-intro", "--log-limit", "5"])


if __name__ == "__main__":
    unittest.main()
