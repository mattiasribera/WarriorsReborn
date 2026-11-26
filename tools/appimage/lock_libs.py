#!/usr/bin/env python3
"""Writes the lock files of the Ubuntu packages whose libraries the AppImage bundles, pinned by
version and SHA-256:

  i386-libs.lock    the whole i386 runtime of xbrun (the 32-bit game): ld-linux.so.2, libc, libm and
                    the libraries it opens at run time (libSDL2-2.0.so.0 for audio and pads,
                    libX11.so.6), with everything they link (DT_NEEDED), so the host needs no
                    32-bit libraries at all.
  amd64-libs.lock   a fallback x86_64 libSDL2-2.0.so.0 with the few libraries it links that a
                    desktop may lack (libXss, libsamplerate, libdecor), used by AppRun only when
                    the host has no x86_64 libSDL2 of its own (the launcher and xbrun-render64).

Run by hand on an Ubuntu x86_64 host with those libraries installed (multiarch i386 for the first):
the closure is read from the host's files and dpkg's file lists, nothing is installed. Each .deb is
downloaded from Launchpad into the scratch folder only to record its SHA-256.
tools/appimage/build.sh then downloads exactly those files and checks them, on any x86_64 Linux.

    python3 -I tools/appimage/lock_libs.py <scratch-dir>
"""
import hashlib
import os
import subprocess
import sys
import urllib.request

URL = "https://launchpad.net/ubuntu/+archive/primary/+files/{}"
HERE = os.path.dirname(os.path.abspath(__file__))
LOCKS = [
    # (lock file, dpkg architecture, library folders, root sonames, packages kept: None = all)
    ("i386-libs.lock", "i386",
     ["/usr/lib/i386-linux-gnu", "/lib/i386-linux-gnu", "/usr/lib/i386-linux-gnu/pulseaudio"],
     ["ld-linux.so.2", "libc.so.6", "libm.so.6", "libSDL2-2.0.so.0", "libX11.so.6"], None),
    ("amd64-libs.lock", "amd64", ["/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu"],
     ["libSDL2-2.0.so.0"], {"libsdl2-classic", "libxss1", "libsamplerate0", "libdecor-0-0"}),
]


def find_lib(dirs, soname):
    for d in dirs:
        p = os.path.join(d, soname)
        if os.path.exists(p):
            return p
    raise SystemExit(f"{soname}: not found in {dirs} (install the host's libraries to lock them)")


def needed(path):
    out = subprocess.run(["readelf", "-dW", path], capture_output=True, text=True, check=True).stdout
    return [line.split("[", 1)[1].split("]", 1)[0] for line in out.splitlines() if "(NEEDED)" in line]


def owner(path):
    real = os.path.realpath(path)
    for candidate in (real, real.replace("/usr/lib/", "/lib/", 1), path):
        r = subprocess.run(["dpkg", "-S", candidate], capture_output=True, text=True)
        if r.returncode == 0:
            # "libc6:i386: /usr/lib/i386-linux-gnu/libc.so.6": the package, and the file as the package spells it
            pkgs, inner = r.stdout.strip().splitlines()[0].split(": ", 1)
            return pkgs.split(",")[0].strip().split(":")[0], inner.lstrip("/")
    raise SystemExit(f"{path}: no package owns it")


def write_lock(scratch, name, arch, dirs, roots, keep):
    seen, todo, files = set(), list(roots), {}
    while todo:
        so = todo.pop()
        if so in seen:
            continue
        seen.add(so)
        path = find_lib(dirs, so)
        # the package of the file the soname link ends at (libsdl2-classic's sdl2-classic/... for SDL2)
        pkg, inner = owner(os.path.realpath(path))
        if keep is not None and pkg not in keep:
            continue
        files.setdefault(pkg, []).append((so, inner))
        todo.extend(needed(path))
    # A package whose /usr/share/doc/<pkg> is a link to another package's (libgcc-s1 -> gcc-16-base):
    # that package too, for its copyright file only ("-": no libraries).
    for pkg in list(files):
        doc = f"/usr/share/doc/{pkg}"
        if os.path.islink(doc):
            target = os.path.join("/usr/share/doc", os.readlink(doc), "copyright")
            files.setdefault(owner(target)[0], [])
    rows = []
    for pkg in sorted(files):
        ver = subprocess.run(["dpkg-query", "-W", "-f", "${Version}", f"{pkg}:{arch}"], capture_output=True, text=True,
                             check=True).stdout.strip()
        deb = f"{pkg}_{ver.split(':', 1)[-1]}_{arch}.deb"
        dest = os.path.join(scratch, deb)
        if not os.path.exists(dest):
            urllib.request.urlretrieve(URL.format(deb), dest)
        with open(dest, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        libs = ",".join(f"{so}={inner}" for so, inner in sorted(files[pkg])) or "-"
        rows.append(f"{pkg}\t{ver}\t{deb}\t{sha}\t{libs}")
        print(f"{deb} {sha}")
    with open(os.path.join(HERE, name), "w") as f:
        f.write(f"# Ubuntu {arch} packages bundled into the AppImage (tools/appimage/lock_libs.py writes this file).\n"
                "# package<TAB>version<TAB>deb<TAB>sha256<TAB>soname=path-in-deb[,...]; each .deb comes from\n"
                "# " + URL.format("<deb>") + "\n")
        f.write("\n".join(rows) + "\n")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    os.makedirs(sys.argv[1], exist_ok=True)
    for lock in LOCKS:
        write_lock(sys.argv[1], *lock)


if __name__ == "__main__":
    main()
