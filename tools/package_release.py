#!/usr/bin/env python3
"""Assembles the Windows release zip of WarriorsReborn, its SHA256SUMS.txt and release notes.

Used by .github/workflows/release.yml and runnable locally with the same result. It only packs
files that already exist: build the Windows programs first (make -C runtime windows,
make -C launcher windows), and have the SDL2 mingw development package, zig's lib folder and
Vulkan-Headers at hand (the same versions the workflow pins).

Zip layout (WarriorsReborn-vX.Y.Z-windows-x64.zip):

    WarriorsReborn-vX.Y.Z/
        ARGUMENTS.md  README.txt  SDL2.dll  TheWarriors.exe
        warriors-launcher.exe  xbrun-core.dll
        licenses/     license texts of the port and the third-party software

Entries are written in a fixed order with a fixed date (SOURCE_DATE_EPOCH, else the date of the
last commit), so the same inputs always give the same zip.

Usage:
    python3 tools/package_release.py package --sdl2-devel DIR --vulkan-headers DIR
                                     [--zig-lib DIR] [--version X.Y.Z] [--readme FILE]
                                     [--out dist] [--repo .]
    python3 tools/package_release.py notes [--version X.Y.Z] [--repo .] [--github OWNER/NAME]
    python3 tools/package_release.py version [--repo .]

package writes <out>/WarriorsReborn-vX.Y.Z-windows-x64.zip and <out>/SHA256SUMS.txt. The version
defaults to RELEASE_VERSION of the Makefiles; both Makefiles and CHANGELOG.md must agree with
it. --readme replaces the generated README.txt. notes prints the CHANGELOG.md section of the
version as Markdown, with relative links pointing at the tag on GitHub.

Standard library only.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import textwrap
import time
import zipfile

GITHUB_REPO = "mattiasribera/WarriorsReborn"

# Third-party versions shipped in the zip; keep them in step with the pins in release.yml.
SDL2_VERSION = "2.32.10"
ZIG_VERSION = "0.17.0"
VULKAN_HEADERS_VERSION = "1.4.363"
IMGUI_VERSION = "1.91.5"
INTER_VERSION = "4.1"

# Top-level files of the release folder, in zip order.
TOP_FILES = ["ARGUMENTS.md", "README.txt", "SDL2.dll", "TheWarriors.exe",
             "warriors-launcher.exe", "xbrun-core.dll"]
PROGRAMS = {
    "TheWarriors.exe": "runtime/build/TheWarriors.exe",
    "xbrun-core.dll": "runtime/build/xbrun-core.dll",
    "warriors-launcher.exe": "launcher/build/warriors-launcher.exe",
}


class ReleaseError(Exception):
    pass


# ---- version and changelog ----

def makefile_version(repo):
    versions = set()
    for mk in ("runtime/Makefile", "launcher/Makefile"):
        with open(os.path.join(repo, mk), encoding="utf-8") as f:
            m = re.search(r"^RELEASE_VERSION\s*=\s*(\S+)\s*$", f.read(), re.MULTILINE)
        if not m:
            raise ReleaseError("%s: no RELEASE_VERSION" % mk)
        versions.add(m.group(1))
    if len(versions) != 1:
        raise ReleaseError("runtime/Makefile and launcher/Makefile disagree on RELEASE_VERSION: %s"
                           % ", ".join(sorted(versions)))
    return versions.pop()


def changelog_section(changelog, version):
    """The body of '## vX.Y.Z ...' up to the next '## ' heading, without the heading."""
    lines = changelog.splitlines()
    head = re.compile(r"^## v%s(\s|$)" % re.escape(version))
    for i, line in enumerate(lines):
        if head.match(line):
            body = []
            for nxt in lines[i + 1:]:
                if nxt.startswith("## "):
                    break
                body.append(nxt)
            return "\n".join(body).strip("\n")
    raise ReleaseError("CHANGELOG.md has no '## v%s' section" % version)


def release_notes(changelog, version, github=GITHUB_REPO):
    body = changelog_section(changelog, version)
    base = "https://github.com/%s/blob/v%s/" % (github, version)

    def absolute(m):
        target = m.group(2)
        if re.match(r"^[a-z][a-z0-9+.-]*:|^#|^/", target, re.IGNORECASE):
            return m.group(0)
        return "%s%s%s)" % (m.group(1), base, target)

    return re.sub(r"(\]\()([^)\s]+)\)", absolute, body) + "\n"


def changelog_summary(changelog, version):
    """The first paragraph of the version's section as plain text."""
    para = []
    for line in changelog_section(changelog, version).splitlines():
        if not line.strip():
            if para:
                break
            continue
        if line.startswith("#"):
            break
        para.append(line.strip())
    text = " ".join(para)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    return text.replace("**", "").replace("`", "")


# ---- README.txt ----

README_TEMPLATE = """\
{title}
{rule}

NO GAME FILES ARE INCLUDED. You need your own legally obtained copy of The
Warriors for the original Xbox, as a disc image (.iso) made from your own disc.
Each player must supply their own copy.

Install (Windows)
-----------------
1. Unzip this folder anywhere and keep all of its files together.
2. Run warriors-launcher.exe.
3. The setup wizard opens:
   - Step 1 finds TheWarriors.exe by itself.
   - Step 2: choose your own game disc image (.iso). The launcher checks that
     it is The Warriors, then copies the game files out of it into a "game"
     folder next to the launcher (about 5.2 GB, you can pick another folder).
     This is done once. Your .iso is only read and is left untouched. If you
     already have an extracted copy (the folder with default.xbe), choose
     "I already have an extracted folder" instead.
   - Step 3: your saved games go to the "hdd" folder next to the launcher
     (created for you).
   - Step 4: pick "Performance" or "Quality".
4. Press PLAY.

{news_title}
{news_rule}
{news}

Requirements: Windows 10 or 11 (64-bit) and a GPU whose driver supports
Vulkan 1.0 or newer (current NVIDIA, AMD and Intel drivers do).

Optional packs
--------------
HD texture packs and button-icon packs are not included. You can make them
from your own copy of the game (see the README of the WarriorsReborn source
repository), then choose them in the launcher.

Known issues
------------
- 60 FPS mode is new. If a cutscene looks or plays wrong, set the frame rate
  to 30 on the Display page.
- An AI-upscaled texture pack can make the characters of the final level look
  white on some PCs (not reproduced yet). Play that level without the pack.
- On Windows all XInput controllers (Xbox-style pads) share one mapping:
  remapping the buttons of one changes all of them.
- The programs are not code-signed, so Windows SmartScreen or an antivirus may
  warn on first run. Check the zip against SHA256SUMS.txt from the release page.

Files
-----
warriors-launcher.exe  the launcher (settings, setup, PLAY)
TheWarriors.exe        the game program
xbrun-core.dll         the port's runtime
SDL2.dll               SDL {sdl} (window, sound, controllers)
licenses\\              license texts of the port and the third-party software

License
-------
WarriorsReborn is Copyright (C) 2026 Mattias Ribera Rojas, free software under
the GNU General Public License version 3 or later, WITHOUT ANY WARRANTY - see
licenses\\WarriorsReborn-GPL-3.0-LICENSE.txt. The source code is published in
the WarriorsReborn repository next to this release.

Third-party software
--------------------
- SDL {sdl} (SDL2.dll) - zlib license - licenses\\SDL2-LICENSE.txt
- Dear ImGui v{imgui}, compiled into warriors-launcher.exe - MIT -
  licenses\\imgui-LICENSE.txt
- Inter {inter} font (Regular, SemiBold), embedded in warriors-launcher.exe -
  SIL Open Font License 1.1 - licenses\\Inter-LICENSE.txt
- stb_image v2.30 and stb_image_write v1.16, compiled into xbrun-core.dll -
  MIT or public domain - licenses\\stb-LICENSE.txt
- Vulkan-Headers {vulkan} (Khronos), compiled into xbrun-core.dll -
  Apache-2.0 or MIT - licenses\\Vulkan-Headers-LICENSE.txt
- LLVM libc++, libc++abi and libunwind (from zig {zig}), linked into
  xbrun-core.dll and warriors-launcher.exe - Apache-2.0 with LLVM exception -
  licenses\\libcxx-LICENSE.txt, libcxxabi-LICENSE.txt, libunwind-LICENSE.txt
- mingw-w64 runtime support code (from zig {zig}), in all three programs -
  ZPL 2.1 (some parts public domain or BSD) - licenses\\mingw-w64-COPYING.txt

Not included, provided by your system: the Microsoft Universal C Runtime
(part of Windows 10/11) and the Vulkan loader (vulkan-1.dll) with the Vulkan
driver, which come with Windows and your graphics driver.

The disc image reader, the launcher and the runtime are the port's own code.
The Warriors and all related names and marks are trademarks of their
respective owners; "Xbox" is a trademark of Microsoft. This project is not
affiliated with, endorsed by or sponsored by Rockstar Games, Take-Two
Interactive, Paramount or Microsoft, and contains none of their files.
"""


def readme_text(version, changelog):
    """README.txt for the zip, with CRLF line ends (opened in Notepad by most players)."""
    title = "WarriorsReborn v%s (beta) - The Warriors native PC port" % version
    news_title = "New in v%s" % version
    summary = changelog_summary(changelog, version)
    news = textwrap.fill(summary + " The full list is in CHANGELOG.md of the source repository.",
                         width=78)
    text = README_TEMPLATE.format(title=title, rule="=" * len(title), news_title=news_title,
                                  news_rule="-" * len(news_title), news=news, sdl=SDL2_VERSION,
                                  imgui=IMGUI_VERSION, inter=INTER_VERSION,
                                  vulkan=VULKAN_HEADERS_VERSION, zig=ZIG_VERSION)
    return text.replace("\n", "\r\n").encode("ascii")


# ---- inputs ----

def license_sources(repo, sdl2_devel, zig_lib, vulkan_headers):
    """Zip name under licenses/ -> source file."""
    return {
        "WarriorsReborn-GPL-3.0-LICENSE.txt": os.path.join(repo, "LICENSE"),
        "imgui-LICENSE.txt": os.path.join(repo, "launcher/third_party/imgui/LICENSE.txt"),
        "Inter-LICENSE.txt": os.path.join(repo, "launcher/third_party/inter/LICENSE.txt"),
        "stb-LICENSE.txt": os.path.join(repo, "runtime/third_party/stb/LICENSE"),
        "SDL2-LICENSE.txt": os.path.join(sdl2_devel, "LICENSE.txt"),
        "Vulkan-Headers-LICENSE.txt": os.path.join(vulkan_headers, "LICENSE.md"),
        "libcxx-LICENSE.txt": os.path.join(zig_lib, "libcxx/LICENSE.TXT"),
        "libcxxabi-LICENSE.txt": os.path.join(zig_lib, "libcxxabi/LICENSE.TXT"),
        "libunwind-LICENSE.txt": os.path.join(zig_lib, "libunwind/LICENSE.TXT"),
        "mingw-w64-COPYING.txt": os.path.join(zig_lib, "libc/mingw/COPYING"),
    }


def read_file(path, what):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise ReleaseError("%s: %s (%s)" % (what, path, e.strerror))


def read_pe(path, what):
    data = read_file(path, what)
    if data[:2] != b"MZ":
        raise ReleaseError("%s: %s is not a Windows program (no MZ header)" % (what, path))
    return data


def zig_lib_dir():
    try:
        out = subprocess.run(["zig", "env"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise ReleaseError("--zig-lib not given and 'zig env' failed: %s" % e)
    m = re.search(r'"?\.?lib_dir"?\s*[=:]\s*"([^"]+)"', out)
    if not m:
        raise ReleaseError("--zig-lib not given and 'zig env' shows no lib_dir")
    return m.group(1)


def default_epoch(repo):
    env = os.environ.get("SOURCE_DATE_EPOCH")
    if env:
        return int(env)
    try:
        out = subprocess.run(["git", "-C", repo, "log", "-1", "--format=%ct"], capture_output=True,
                             text=True, check=True, stdin=subprocess.DEVNULL).stdout
        return int(out.strip())
    except (OSError, subprocess.CalledProcessError, ValueError):
        return 315532800  # 1980-01-01, the earliest zip date


# ---- packaging ----

def zip_name(version):
    return "WarriorsReborn-v%s-windows-x64.zip" % version


def write_zip(path, entries, epoch):
    """entries: list of (name, bytes or None for a folder, executable)."""
    date = time.gmtime(max(epoch, 315532800))[:6]
    with zipfile.ZipFile(path, "w") as z:
        for name, data, executable in entries:
            info = zipfile.ZipInfo(name, date_time=date)
            info.create_system = 3  # Unix, so the permission bits below are kept
            if data is None:
                info.external_attr = (0o40755 << 16) | 0x10
                z.writestr(info, b"")
            else:
                info.external_attr = (0o100755 if executable else 0o100644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, data, compresslevel=9)


def package(repo, version, sdl2_devel, zig_lib, vulkan_headers, out_dir, date_epoch, readme=None):
    """Writes the zip and SHA256SUMS.txt into out_dir; returns both paths."""
    mk_version = makefile_version(repo)
    if version != mk_version:
        raise ReleaseError("version %s does not match RELEASE_VERSION %s of the Makefiles"
                           % (version, mk_version))
    changelog = read_file(os.path.join(repo, "CHANGELOG.md"), "changelog").decode("utf-8")
    changelog_section(changelog, version)

    files = {
        "ARGUMENTS.md": read_file(os.path.join(repo, "docs/ARGUMENTS.md"), "ARGUMENTS.md"),
        "README.txt": (read_file(readme, "--readme") if readme
                       else readme_text(version, changelog)),
        "SDL2.dll": read_pe(os.path.join(sdl2_devel, "i686-w64-mingw32/bin/SDL2.dll"), "SDL2.dll"),
    }
    for name, rel in PROGRAMS.items():
        files[name] = read_pe(os.path.join(repo, rel), name + " (build it first)")
    licenses = {name: read_file(src, "license " + name)
                for name, src in license_sources(repo, sdl2_devel, zig_lib, vulkan_headers).items()}

    top = "WarriorsReborn-v%s/" % version
    entries = [(top, None, False)]
    for name in TOP_FILES:
        entries.append((top + name, files[name], name.endswith((".exe", ".dll"))))
    entries.append((top + "licenses/", None, False))
    for name in sorted(licenses):
        entries.append((top + "licenses/" + name, licenses[name], False))

    os.makedirs(out_dir, exist_ok=True)
    zpath = os.path.join(out_dir, zip_name(version))
    write_zip(zpath, entries, date_epoch)
    with open(zpath, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    sums = os.path.join(out_dir, "SHA256SUMS.txt")
    with open(sums, "w", encoding="ascii", newline="\n") as f:
        f.write("%s  %s\n" % (digest, zip_name(version)))
    return zpath, sums


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

    p = sub.add_parser("package", help="write the release zip and SHA256SUMS.txt")
    p.add_argument("--repo", default=root)
    p.add_argument("--version", help="X.Y.Z (default: RELEASE_VERSION of the Makefiles)")
    p.add_argument("--sdl2-devel", required=True,
                   help="extracted SDL2-%s folder of the mingw development package" % SDL2_VERSION)
    p.add_argument("--zig-lib", help="zig's lib folder (default: from 'zig env')")
    p.add_argument("--vulkan-headers", required=True,
                   help="Vulkan-Headers %s source folder" % VULKAN_HEADERS_VERSION)
    p.add_argument("--readme", help="use this README.txt instead of the generated one")
    p.add_argument("--out", default="dist")

    n = sub.add_parser("notes", help="print the release notes for a version")
    n.add_argument("--repo", default=root)
    n.add_argument("--version")
    n.add_argument("--github", default=GITHUB_REPO)

    v = sub.add_parser("version", help="print RELEASE_VERSION of the Makefiles")
    v.add_argument("--repo", default=root)

    args = ap.parse_args(argv)
    try:
        if args.cmd == "version":
            print(makefile_version(args.repo))
        elif args.cmd == "notes":
            version = args.version or makefile_version(args.repo)
            changelog = read_file(os.path.join(args.repo, "CHANGELOG.md"), "changelog")
            sys.stdout.write(release_notes(changelog.decode("utf-8"), version, args.github))
        else:
            version = args.version or makefile_version(args.repo)
            zpath, sums = package(args.repo, version, args.sdl2_devel,
                                  args.zig_lib or zig_lib_dir(), args.vulkan_headers, args.out,
                                  default_epoch(args.repo), args.readme)
            with open(sums, encoding="ascii") as f:
                sys.stdout.write(f.read())
            print("wrote %s and %s" % (zpath, sums))
    except ReleaseError as e:
        print("package_release: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
