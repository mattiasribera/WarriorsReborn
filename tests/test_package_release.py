"""Tests for tools/package_release.py (release zip, SHA256SUMS.txt and release notes).

Everything runs on a synthetic tree in a temporary folder: fake PE files stand in for the built
programs and short texts for the license files. No build and no game data are needed.
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import package_release as pr  # noqa: E402

CHANGELOG = """# Changelog

## Unreleased

- Something not released yet.

## v1.2.3 (beta) - 2026-11-02

New **picture** options and `--flag` support.

### Display

- See [docs/ARGUMENTS.md](docs/ARGUMENTS.md) and [README.md](README.md#known-issues).
- Upstream: [SDL](https://www.libsdl.org/).

## v1.2.2 (beta) - 2026-11-01

Older text.
"""

FAKE_PE = b"MZ" + b"\0" * 62


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def make_tree(base, version="1.2.3"):
    """A minimal repository, SDL2 devel folder, zig lib folder and Vulkan-Headers folder."""
    repo = os.path.join(base, "repo")
    for mk in ("runtime/Makefile", "launcher/Makefile"):
        write(os.path.join(repo, mk), "X = 1\nRELEASE_VERSION = %s\nY = 2\n" % version)
    write(os.path.join(repo, "CHANGELOG.md"), CHANGELOG)
    write(os.path.join(repo, "LICENSE"), "GPL text\n")
    write(os.path.join(repo, "docs/ARGUMENTS.md"), "# Arguments\n")
    write(os.path.join(repo, "launcher/third_party/imgui/LICENSE.txt"), "imgui\n")
    write(os.path.join(repo, "launcher/third_party/inter/LICENSE.txt"), "inter\n")
    write(os.path.join(repo, "runtime/third_party/stb/LICENSE"), "stb\n")
    write(os.path.join(repo, "runtime/build/TheWarriors.exe"), FAKE_PE + b"exe")
    write(os.path.join(repo, "runtime/build/xbrun-core.dll"), FAKE_PE + b"dll")
    write(os.path.join(repo, "launcher/build/warriors-launcher.exe"), FAKE_PE + b"launcher")
    sdl = os.path.join(base, "SDL2-2.32.10")
    write(os.path.join(sdl, "LICENSE.txt"), "zlib\n")
    write(os.path.join(sdl, "i686-w64-mingw32/bin/SDL2.dll"), FAKE_PE + b"sdl")
    zlib = os.path.join(base, "zig/lib")
    for rel in ("libcxx/LICENSE.TXT", "libcxxabi/LICENSE.TXT", "libunwind/LICENSE.TXT",
                "libc/mingw/COPYING"):
        write(os.path.join(zlib, rel), rel + "\n")
    vk = os.path.join(base, "Vulkan-Headers")
    write(os.path.join(vk, "LICENSE.md"), "vulkan\n")
    return repo, sdl, zlib, vk


class NotesTest(unittest.TestCase):
    def test_extracts_only_the_version_section(self):
        notes = pr.release_notes(CHANGELOG, "1.2.3", "owner/repo")
        self.assertTrue(notes.startswith("New **picture** options"))
        self.assertIn("### Display", notes)
        self.assertNotIn("Older text", notes)
        self.assertNotIn("not released yet", notes)
        self.assertNotIn("## v1.2.3", notes)

    def test_relative_links_point_at_the_tag(self):
        notes = pr.release_notes(CHANGELOG, "1.2.3", "owner/repo")
        self.assertIn("(https://github.com/owner/repo/blob/v1.2.3/docs/ARGUMENTS.md)", notes)
        self.assertIn("(https://github.com/owner/repo/blob/v1.2.3/README.md#known-issues)", notes)
        self.assertIn("(https://www.libsdl.org/)", notes)

    def test_missing_section_fails(self):
        with self.assertRaises(pr.ReleaseError):
            pr.release_notes(CHANGELOG, "9.9.9", "owner/repo")

    def test_summary_is_plain_text(self):
        self.assertEqual(pr.changelog_summary(CHANGELOG, "1.2.3"),
                         "New picture options and --flag support.")


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = self.tmp.name
        self.repo, self.sdl, self.zlib, self.vk = make_tree(self.base)

    def tearDown(self):
        self.tmp.cleanup()

    def package(self, out, **kw):
        return pr.package(repo=self.repo, version="1.2.3", sdl2_devel=self.sdl, zig_lib=self.zlib,
                          vulkan_headers=self.vk, out_dir=out, date_epoch=1793577600, **kw)

    def test_layout_matches_the_release(self):
        zpath, _ = self.package(os.path.join(self.base, "dist"))
        self.assertEqual(os.path.basename(zpath), "WarriorsReborn-v1.2.3-windows-x64.zip")
        with zipfile.ZipFile(zpath) as z:
            names = z.namelist()
            top = "WarriorsReborn-v1.2.3/"
            expected = [top] + [top + n for n in pr.TOP_FILES] + [top + "licenses/"] + \
                [top + "licenses/" + n for n in sorted(pr.license_sources(self.repo, self.sdl,
                                                                           self.zlib, self.vk))]
            self.assertEqual(sorted(names), sorted(expected))
            self.assertEqual(z.read(top + "SDL2.dll"), FAKE_PE + b"sdl")
            self.assertEqual(z.read(top + "licenses/Vulkan-Headers-LICENSE.txt"), b"vulkan\n")
            self.assertEqual(z.read(top + "licenses/mingw-w64-COPYING.txt"), b"libc/mingw/COPYING\n")
            readme = z.read(top + "README.txt").decode("ascii")
            self.assertIn("WarriorsReborn v1.2.3 (beta)", readme)
            self.assertIn("New in v1.2.3", readme)
            self.assertIn("New picture options and --flag support.", readme)
            self.assertIn("\r\n", readme)
            self.assertNotIn("\n", readme.replace("\r\n", ""))
            info = z.getinfo(top + "TheWarriors.exe")
            self.assertEqual(info.external_attr >> 16 & 0o777, 0o755)

    def test_sha256sums(self):
        zpath, sums = self.package(os.path.join(self.base, "dist"))
        with open(zpath, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        with open(sums, "rb") as f:
            self.assertEqual(f.read(), ("%s  %s\n" % (digest, os.path.basename(zpath))).encode())

    def test_output_is_deterministic(self):
        a, _ = self.package(os.path.join(self.base, "a"))
        b, _ = self.package(os.path.join(self.base, "b"))
        with open(a, "rb") as fa, open(b, "rb") as fb:
            self.assertEqual(fa.read(), fb.read())

    def test_readme_override_is_copied_as_is(self):
        readme = os.path.join(self.base, "README.txt")
        write(readme, b"hand written\r\n")
        zpath, _ = self.package(os.path.join(self.base, "dist"), readme=readme)
        with zipfile.ZipFile(zpath) as z:
            self.assertEqual(z.read("WarriorsReborn-v1.2.3/README.txt"), b"hand written\r\n")

    def test_version_must_match_the_makefiles(self):
        with self.assertRaises(pr.ReleaseError):
            pr.package(repo=self.repo, version="1.2.4", sdl2_devel=self.sdl, zig_lib=self.zlib,
                       vulkan_headers=self.vk, out_dir=os.path.join(self.base, "dist"),
                       date_epoch=0)

    def test_missing_binary_fails(self):
        os.remove(os.path.join(self.repo, "runtime/build/xbrun-core.dll"))
        with self.assertRaises(pr.ReleaseError):
            self.package(os.path.join(self.base, "dist"))

    def test_non_pe_binary_fails(self):
        write(os.path.join(self.repo, "launcher/build/warriors-launcher.exe"), b"#!/bin/sh\n")
        with self.assertRaises(pr.ReleaseError):
            self.package(os.path.join(self.base, "dist"))

    def test_command_line(self):
        out = os.path.join(self.base, "cli")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/package_release.py"),
                            "package", "--repo", self.repo, "--version", "1.2.3",
                            "--sdl2-devel", self.sdl, "--zig-lib", self.zlib,
                            "--vulkan-headers", self.vk, "--out", out],
                           capture_output=True, text=True, env=dict(os.environ,
                                                                    SOURCE_DATE_EPOCH="1793577600"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(os.path.isfile(os.path.join(out, "SHA256SUMS.txt")))
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/package_release.py"),
                            "version", "--repo", self.repo], capture_output=True, text=True)
        self.assertEqual((r.returncode, r.stdout), (0, "1.2.3\n"))


if __name__ == "__main__":
    unittest.main()
