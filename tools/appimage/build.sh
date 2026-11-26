#!/bin/sh
# Builds WarriorsReborn-<version>-x86_64.AppImage: the launcher (x86_64), the game runtime xbrun
# (i386) with its whole i386 runtime bundled, and the x86_64 render helper xbrun-render64. No game
# data goes in: players supply their own disc or ISO.
#
#   tools/appimage/build.sh [-o OUT_DIR] [--prebuilt]
#
#   -o OUT_DIR    where the .AppImage goes (default: the scratch folder below)
#   --prebuilt    take runtime/build/xbrun, runtime/build64/xbrun-render64 and
#                 launcher/build/linux64/warriors-launcher as they are (default: make them first)
#
# Environment:
#   RELEASE_VERSION   the version in the file name (default: RELEASE_VERSION of runtime/Makefile)
#   APPIMAGE_SCRATCH  downloads and the AppDir (default: $XDG_CACHE_HOME/warriorsreborn-appimage)
#   RUNTIME_MAKE_ARGS extra make arguments for runtime/ (e.g. VULKAN_INCLUDE=... GLSLANG=...)
#
# Every download is pinned and checked against its SHA-256: appimagetool and the AppImage runtime
# below, and the Ubuntu .deb packages of tools/appimage/i386-libs.lock and amd64-libs.lock
# (tools/appimage/lock_libs.py writes them). Needs: sh, curl, sha256sum, tar, python3, make and zig
# for the binaries, strip (binutils), and dpkg-deb or ar (binutils) with xz/zstd to unpack the packages.
set -eu

APPIMAGETOOL_VERSION=1.9.1
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
RUNTIME_VERSION=20251108
RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
RUNTIME_LICENSE_SHA256=aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1
DEB_URL=https://launchpad.net/ubuntu/+archive/primary/+files
# Example mods the release leaves out (as RELEASE_EXCLUDED_MODS of tools/package_release.py; none
# since v0.1.9: the game builds what the bandana and the dirty clothes need from assets.ini).
EXCLUDED_MODS=""

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
scratch=${APPIMAGE_SCRATCH:-${XDG_CACHE_HOME:-$HOME/.cache}/warriorsreborn-appimage}
out=""
prebuilt=0
while [ $# -gt 0 ]; do
    case "$1" in
    -o) out=$2; shift 2 ;;
    --prebuilt) prebuilt=1; shift ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "build.sh: unknown argument $1 (see --help)" >&2; exit 2 ;;
    esac
done
mkdir -p "$scratch"
out=${out:-$scratch}
mkdir -p "$out"
out=$(cd "$out" && pwd)

version=${RELEASE_VERSION:-$(sed -n 's/^RELEASE_VERSION *= *//p' "$repo/runtime/Makefile" | head -n 1)}
if [ -z "$version" ]; then
    echo "build.sh: no version: set RELEASE_VERSION (runtime/Makefile has none)" >&2
    exit 2
fi

fetch() {   # fetch URL FILE SHA256: download once into the scratch folder, always check
    if [ ! -f "$2" ] || ! echo "$3  $2" | sha256sum -c --quiet - 2>/dev/null; then
        curl -sSLf --retry 3 -o "$2.part" "$1"
        mv "$2.part" "$2"
    fi
    if ! echo "$3  $2" | sha256sum -c --quiet -; then
        echo "build.sh: $2: SHA-256 mismatch (expected $3)" >&2
        exit 1
    fi
}

unpack_deb() {   # unpack_deb DEB DIR
    mkdir -p "$2"
    if command -v dpkg-deb >/dev/null 2>&1; then
        dpkg-deb -x "$1" "$2"
    else
        member=$(ar t "$1" | grep '^data\.tar')
        case "$member" in
        *.zst) ar p "$1" "$member" | zstd -dc | tar -x -C "$2" ;;
        *.xz) ar p "$1" "$member" | xz -dc | tar -x -C "$2" ;;
        *) ar p "$1" "$member" | tar -x -C "$2" ;;
        esac
    fi
}

# ---- the binaries ----
if [ "$prebuilt" = 0 ]; then
    # shellcheck disable=SC2086
    make -C "$repo/runtime" ${RUNTIME_MAKE_ARGS:-} build/xbrun build64/xbrun-render64
    make -C "$repo/launcher" linux64
fi
xbrun=$repo/runtime/build/xbrun
render64=$repo/runtime/build64/xbrun-render64
launcher=$repo/launcher/build/linux64/warriors-launcher
for f in "$xbrun" "$render64" "$launcher"; do
    [ -x "$f" ] || { echo "build.sh: missing $f (build it, or drop --prebuilt)" >&2; exit 1; }
done

# ---- the AppDir, from scratch every time ----
appdir=${scratch:?}/AppDir
case "$appdir" in
*/AppDir) [ -d "$appdir" ] && rm -rf -- "$appdir" ;;
esac
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib/warriorsreborn" "$appdir/usr/lib/i386-linux-gnu" \
    "$appdir/usr/lib/x86_64-fallback" "$appdir/usr/share/warriorsreborn" "$appdir/usr/share/doc/warriorsreborn" \
    "$appdir/usr/share/licenses"

install -m 0755 "$here/AppRun" "$appdir/AppRun"
install -m 0755 "$launcher" "$appdir/usr/bin/warriors-launcher"
install -m 0755 "$here/xbrun.sh" "$appdir/usr/bin/xbrun"
install -m 0755 "$render64" "$appdir/usr/bin/xbrun-render64"
install -m 0755 "$xbrun" "$appdir/usr/lib/warriorsreborn/xbrun"
# no debug info in the image: smaller, and no build machine paths (DW_AT_comp_dir); symbols stay
strip --strip-debug "$appdir/usr/bin/warriors-launcher" "$appdir/usr/bin/xbrun-render64" \
    "$appdir/usr/lib/warriorsreborn/xbrun"
install -m 0644 "$here/warriorsreborn.desktop" "$appdir/warriorsreborn.desktop"
# the port's own geometric icon (tools/icon/make_icon.py, no game art), 256 px
python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import make_icon as m
open(sys.argv[2], "wb").write(m.png(m.render(256)))' "$repo/tools/icon" "$appdir/warriorsreborn.png"

# the bundled libraries: soname=path-in-deb from each locked package
bundle() {   # bundle LOCK DEST
    while IFS="$(printf '\t')" read -r pkg ver deb sha libs; do
        case "$pkg" in ''|'#'*) continue ;; esac
        fetch "$DEB_URL/$deb" "$scratch/debs/$deb" "$sha"
        tree=$scratch/debs/unpacked/${deb%.deb}
        [ -d "$tree" ] || unpack_deb "$scratch/debs/$deb" "$tree"
        old_ifs=$IFS
        IFS=,
        for entry in $libs; do
            [ "$entry" = "-" ] || install -m 0755 "$tree/${entry#*=}" "$2/${entry%%=*}"
        done
        IFS=$old_ifs
        mkdir -p "$appdir/usr/share/licenses/$pkg"
        if [ -L "$tree/usr/share/doc/$pkg" ]; then   # its copyright is another locked package's
            echo "See ../$(readlink "$tree/usr/share/doc/$pkg")/copyright" > "$appdir/usr/share/licenses/$pkg/copyright"
        else
            install -m 0644 "$tree/usr/share/doc/$pkg/copyright" "$appdir/usr/share/licenses/$pkg/copyright"
        fi
        printf '%s %s %s\n' "$pkg" "$ver" "$sha" >> "$appdir/usr/share/licenses/PACKAGES.txt"
    done < "$1"
}
mkdir -p "$scratch/debs/unpacked"
: > "$appdir/usr/share/licenses/PACKAGES.txt"
bundle "$here/i386-libs.lock" "$appdir/usr/lib/i386-linux-gnu"
bundle "$here/amd64-libs.lock" "$appdir/usr/lib/x86_64-fallback"

# the example mods: mod.ini, .lua and .col files and the mod's own assets in own/ (.glb, .png,
# .dds, .wav, .ogg), as in the Windows zip (tools/package_release.py)
for mod in "$repo"/mods/*/; do
    name=$(basename "$mod")
    case " $EXCLUDED_MODS " in *" $name "*) continue ;; esac
    [ -f "$mod/mod.ini" ] || continue
    mkdir -p "$appdir/usr/share/warriorsreborn/mods/$name"
    for f in "$mod"*.ini "$mod"*.lua "$mod"*.col; do
        [ -f "$f" ] && install -m 0644 "$f" "$appdir/usr/share/warriorsreborn/mods/$name/"
    done
    for f in "$mod"own/*.glb "$mod"own/*.png "$mod"own/*.dds "$mod"own/*.wav "$mod"own/*.ogg; do
        [ -f "$f" ] || continue
        mkdir -p "$appdir/usr/share/warriorsreborn/mods/$name/own"
        install -m 0644 "$f" "$appdir/usr/share/warriorsreborn/mods/$name/own/"
    done
done

# documents and licences of our own
for doc in docs/LINUX.md docs/ARGUMENTS.md docs/MODDING.md README.md CHANGELOG.md; do
    [ -f "$repo/$doc" ] && install -m 0644 "$repo/$doc" "$appdir/usr/share/doc/warriorsreborn/"
done
for lic in LICENSE THIRD_PARTY_NOTICES.md; do
    [ -f "$repo/$lic" ] && install -m 0644 "$repo/$lic" "$appdir/usr/share/licenses/"
done
mkdir -p "$appdir/usr/share/licenses/inter-font" "$appdir/usr/share/licenses/dear-imgui"
for f in "$repo"/launcher/third_party/inter/*LICENSE* "$repo"/launcher/third_party/inter/OFL*; do
    [ -f "$f" ] && install -m 0644 "$f" "$appdir/usr/share/licenses/inter-font/"
done
[ -f "$repo/launcher/third_party/imgui/LICENSE.txt" ] && \
    install -m 0644 "$repo/launcher/third_party/imgui/LICENSE.txt" "$appdir/usr/share/licenses/dear-imgui/"

# ---- package ----
tools=$scratch/tools
mkdir -p "$tools"
fetch "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-x86_64.AppImage" \
    "$tools/appimagetool-$APPIMAGETOOL_VERSION-x86_64.AppImage" "$APPIMAGETOOL_SHA256"
fetch "https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_VERSION/runtime-x86_64" \
    "$tools/runtime-$RUNTIME_VERSION-x86_64" "$RUNTIME_SHA256"
fetch "https://raw.githubusercontent.com/AppImage/type2-runtime/$RUNTIME_VERSION/LICENSE" \
    "$tools/type2-runtime-$RUNTIME_VERSION-LICENSE" "$RUNTIME_LICENSE_SHA256"
mkdir -p "$appdir/usr/share/licenses/appimage-type2-runtime"
install -m 0644 "$tools/type2-runtime-$RUNTIME_VERSION-LICENSE" "$appdir/usr/share/licenses/appimage-type2-runtime/LICENSE"
chmod 0755 "$tools/appimagetool-$APPIMAGETOOL_VERSION-x86_64.AppImage"

target=$out/WarriorsReborn-$version-x86_64.AppImage
# extract-and-run: no FUSE needed on the build machine (CI containers)
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tools/appimagetool-$APPIMAGETOOL_VERSION-x86_64.AppImage" --no-appstream \
    --runtime-file "$tools/runtime-$RUNTIME_VERSION-x86_64" "$appdir" "$target"
echo "build.sh: $target ($(wc -c < "$target") bytes)"
