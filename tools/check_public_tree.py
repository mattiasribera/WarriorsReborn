#!/usr/bin/env python3
"""Public-tree guard: fails when the repository contains material that must never be published.

Checks every tracked file (``git ls-files``; falls back to walking the tree outside a git
checkout) for:

* forbidden paths: game-data or private working directories (extracted/, iso/, hdd/, odd/, ...);
* forbidden extensions: disc images, executables, game archives, media, saves, dumps, images;
* files larger than 5 MB;
* private markers in text: personal home paths, private IP addresses, e-mail addresses.

Usage: python3 tools/check_public_tree.py [repo-root]     (exit 0 clean, 1 findings)
"""
import os
import re
import subprocess
import sys

MAX_BYTES = 5 * 1024 * 1024

FORBIDDEN_DIRS = {
    "extracted", "iso", "hdd", "disc", "ghidra", "odd", ".codegraph", ".atl", "scratch",
    "texture_pack", "texture_dump", "texture_review", "glyph_packs", "dumps", "saves", "logs",
}

FORBIDDEN_EXT = {
    # disc images and executables of the title
    ".iso", ".xiso", ".xbe", ".xbx", ".exe", ".dll", ".so", ".dylib",
    # game archives, scripts and audio/video containers
    ".wad", ".pak", ".idx", ".lua", ".luac", ".xmv", ".bik", ".xwb", ".xsb", ".xgs",
    ".wav", ".mp3", ".ogg", ".mp4", ".avi",
    # images and textures
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".dds", ".tga", ".webp",
    # saves, dumps, logs
    ".sav", ".dmp", ".core", ".log", ".bin", ".raw",
}
FORBIDDEN_EXT_RE = re.compile(r"\.wad\d+$", re.IGNORECASE)

# Files that are allowed despite their extension (own or third-party, licensed assets).
ALLOWED_FILES = {
    "assets/icon/wonder-wheel.ico",  # drawn by tools/icon/make_icon.py
}

# Patterns that identify a private machine or person. The guard file itself is exempt.
PRIVATE_PATTERNS = [
    ("home path", re.compile(r"/home/(?!linuxbrew/|player/|u/|runner/|user/|jdoe\b)[A-Za-z0-9_.-]+")),
    ("macOS home path", re.compile(r"/Users/(?!Shared/)[A-Za-z0-9_.-]+")),
    ("Windows user path", re.compile(r"[A-Za-z]:\\\\?Users\\\\?(?!Public\b|jdoe\b)[A-Za-z0-9_.-]+", re.IGNORECASE)),
    ("tailnet/CGNAT IP", re.compile(r"\b100\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.\d{1,3}\.\d{1,3}\b")),
    ("private LAN IP", re.compile(r"\b(?:192\.168|10\.\d{1,3})\.\d{1,3}\.\d{1,3}\b")),
    ("e-mail address", re.compile(r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b")),
    ("scp to a host", re.compile(r"\bscp\s+(-\S+\s+)*\S*:\S*")),
]
# Third-party sources carry their authors' contact addresses; private markers are not checked there.
PRIVATE_EXEMPT_PREFIXES = ("launcher/third_party/", "runtime/third_party/")
SELF = "tools/check_public_tree.py"


def tracked_files(root):
    try:
        out = subprocess.run(["git", "-C", root, "ls-files", "-z"], capture_output=True, check=True,
                             stdin=subprocess.DEVNULL)
        files = [f for f in out.stdout.decode("utf-8", "replace").split("\0") if f]
        if files:
            return files
    except (OSError, subprocess.CalledProcessError):
        pass
    files = []
    for d, dirs, names in os.walk(root):
        dirs[:] = [x for x in dirs if x not in (".git", "build", "__pycache__")]
        for n in names:
            files.append(os.path.relpath(os.path.join(d, n), root).replace(os.sep, "/"))
    return files


def check(root):
    findings = []
    for rel in sorted(tracked_files(root)):
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue
        parts = rel.split("/")
        bad_dir = next((p for p in parts[:-1] if p.lower() in FORBIDDEN_DIRS), None)
        if bad_dir:
            findings.append((rel, "forbidden directory '%s/'" % bad_dir))
        ext = os.path.splitext(rel)[1].lower()
        if rel not in ALLOWED_FILES and (ext in FORBIDDEN_EXT or FORBIDDEN_EXT_RE.search(rel)):
            findings.append((rel, "forbidden file type '%s'" % (ext or parts[-1])))
        size = os.path.getsize(path)
        if size > MAX_BYTES:
            findings.append((rel, "file is %.1f MB (limit 5 MB)" % (size / 1048576)))
        if rel == SELF or rel.startswith(PRIVATE_EXEMPT_PREFIXES) or size > MAX_BYTES:
            continue
        with open(path, "rb") as f:
            data = f.read()
        if b"\0" in data[:8192]:
            continue  # binary (fonts, icon): no text markers to check
        text = data.decode("utf-8", "replace")
        for lineno, line in enumerate(text.splitlines(), 1):
            for label, rx in PRIVATE_PATTERNS:
                m = rx.search(line)
                if m and label == "e-mail address" and m.group(0).lower().endswith((".fx", ".fxh")):
                    m = None  # ReShade technique reference (Name@File.fx), not an address
                if m:
                    findings.append(("%s:%d" % (rel, lineno), "%s: %s" % (label, m.group(0))))
    return findings


def main(argv):
    root = os.path.abspath(argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(__file__), ".."))
    findings = check(root)
    for where, why in findings:
        print("%s: %s" % (where, why))
    if findings:
        print("check_public_tree: %d finding(s)" % len(findings))
        return 1
    print("check_public_tree: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
