#!/bin/sh
# Fetches Steam Audio v4.8.1 and its bundled dependencies at pinned commits, patches them for zig and
# i386, and builds the 32-bit library for Linux (libphonon.so) and Windows (phonon.dll) from source.
#
#   build.sh [linux|windows|all] [OUT]      (default: all, OUT = runtime/build/steam-audio)
#
# Results: OUT/linux-x86/libphonon.so and OUT/windows-x86/phonon.dll (stripped), next to the copied
# LICENSE.md and THIRDPARTY.md. Sources land in OUT/src (git partial clones checked against their
# commit ids), build trees in OUT/build-*. Needs git, cmake, make, zig 0.17 and a network connection
# for the first fetch; nothing is downloaded once OUT/src is complete.
#
# Build choices (README.md): IPP, Embree, AVX, the GPU backends, tests, benchmarks, samples and docs
# off; SSE2 baseline (-msse2 -mfpmath=sse: no -march=native, the official linux-x86 build's AVX-512
# crash); deps zlib, PFFFT and MySOFA static; FlatBuffers headers plus a host flatc.
set -eu

TARGETS=${1:-all}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${2:-$HERE/../../build/steam-audio}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
JOBS=${STEAM_AUDIO_JOBS:-3}

STEAM_AUDIO_URL=https://github.com/ValveSoftware/steam-audio.git
STEAM_AUDIO_SHA=0da18255cca520771f363ee01f100572b39a308e   # tag v4.8.1
FLATBUFFERS_SHA=6df40a2471737b27271bdd9b900ab5f3aec746c7     # core/build/dependencies.json pins
PFFFT_SHA=e0bf595c98ded55cc457a371c1b29c8cab552628
ZLIB_SHA=51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf
MYSOFA_SHA=dd315a8ec1fee7193d40e4a59b12c5590a4a918c

# fetch DIR URL SHA [SPARSE PATHS...]: a blobless partial clone of exactly SHA (only the listed paths
# checked out when given); an existing checkout at another commit is refused.
fetch() {
    dir=$1 url=$2 sha=$3
    shift 3
    if [ -e "$dir/.git" ]; then
        have=$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo none)
        [ "$have" = "$sha" ] || { echo "build.sh: $dir is at $have, expected $sha (remove it to refetch)" >&2; exit 1; }
        return 0
    fi
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
    if [ $# -gt 0 ]; then git -C "$dir" sparse-checkout set --no-cone "$@"; fi
    git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$sha"
    git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
    have=$(git -C "$dir" rev-parse HEAD)
    [ "$have" = "$sha" ] || { echo "build.sh: $url fetched $have, expected $sha" >&2; exit 1; }
}

SRC=$OUT/src
fetch "$SRC/steam-audio" "$STEAM_AUDIO_URL" "$STEAM_AUDIO_SHA" /LICENSE.md /TRADEMARK_RIGHTS.md /core/CMakeLists.txt /core/THIRDPARTY.md /core/build/ /core/git/ /core/src/core/
fetch "$SRC/flatbuffers" https://github.com/google/flatbuffers.git "$FLATBUFFERS_SHA"
fetch "$SRC/pffft" https://github.com/marton78/pffft.git "$PFFFT_SHA"
fetch "$SRC/zlib" https://github.com/madler/zlib.git "$ZLIB_SHA"
fetch "$SRC/mysofa" https://github.com/hoene/libmysofa.git "$MYSOFA_SHA"

# Patches, once per checkout (a stamp file in the checkout's .git directory).
patch_once() {
    dir=$1 patch=$2
    stamp="$dir/.git/wr-$(basename "$patch").applied"
    [ -e "$stamp" ] && return 0
    git -C "$dir" apply --whitespace=nowarn "$patch"
    touch "$stamp"
}
patch_once "$SRC/steam-audio" "$HERE/patches/steam-audio-zig-i386.patch"
patch_once "$SRC/flatbuffers" "$HERE/patches/flatbuffers-clang.patch"
patch_once "$SRC/zlib" "$SRC/steam-audio/core/build/zlib.patch"

# zig compiler wrappers for CMake (one executable per role).
TOOLS=$OUT/tools
mkdir -p "$TOOLS"
wrap() {
    printf '#!/bin/sh\nexec zig %s "$@"\n' "$2" > "$TOOLS/$1"
    chmod +x "$TOOLS/$1"
}
wrap cc-host "cc"
wrap cxx-host "c++"
wrap cc-linux-x86 "cc -target x86-linux-gnu.2.17"
wrap cxx-linux-x86 "c++ -target x86-linux-gnu.2.17"
wrap cc-windows-x86 "cc -target x86-windows-gnu"
wrap cxx-windows-x86 "c++ -target x86-windows-gnu"
wrap ar "ar"
wrap ranlib "ranlib"
wrap rc "rc"
# The MinGW headers spell it windows.h; Steam Audio includes <Windows.h>.
mkdir -p "$TOOLS/winshim"
printf '#include <windows.h>\n' > "$TOOLS/winshim/Windows.h"

toolchain() {   # toolchain PLATFORM -> the CMake toolchain file's path
    tc=$TOOLS/$1.cmake
    case $1 in
    linux-x86) sys=Linux ;;
    windows-x86) sys=Windows ;;
    esac
    cat > "$tc" <<EOF
set(CMAKE_SYSTEM_NAME $sys)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_COMPILER $TOOLS/cc-$1)
set(CMAKE_CXX_COMPILER $TOOLS/cxx-$1)
set(CMAKE_AR $TOOLS/ar)
set(CMAKE_RANLIB $TOOLS/ranlib)
set(CMAKE_RC_COMPILER $TOOLS/rc)
set(CMAKE_C_FLAGS_INIT "-msse2 -mfpmath=sse")
set(CMAKE_CXX_FLAGS_INIT "-msse2 -mfpmath=sse")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
EOF
    echo "$tc"
}

cmake_build() {   # cmake_build SRC BUILD TARGET [cmake args...]
    s=$1 b=$2 t=$3
    shift 3
    cmake -S "$s" -B "$b" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Wno-dev "$@" > "$b.configure.log" 2>&1 \
        || { tail -40 "$b.configure.log" >&2; exit 1; }
    cmake --build "$b" --target "$t" -j "$JOBS" > "$b.build.log" 2>&1 || { tail -60 "$b.build.log" >&2; exit 1; }
}

DEPS=$SRC/steam-audio/core/deps
CORE=$SRC/steam-audio/core

# Host flatc (FindFlatBuffers looks under deps/flatbuffers/bin/linux-x64) and the headers.
if [ ! -x "$DEPS/flatbuffers/bin/linux-x64/flatc" ]; then
    echo "[steam-audio] building flatc (host)"
    cmake_build "$SRC/flatbuffers" "$OUT/build-flatc" flatc -DCMAKE_C_COMPILER="$TOOLS/cc-host" -DCMAKE_CXX_COMPILER="$TOOLS/cxx-host" \
        -DFLATBUFFERS_BUILD_FLATLIB=OFF -DFLATBUFFERS_BUILD_FLATHASH=OFF -DFLATBUFFERS_BUILD_TESTS=OFF "-DCMAKE_CXX_FLAGS=-w -Wno-error"
    mkdir -p "$DEPS/flatbuffers/bin/linux-x64" "$DEPS/flatbuffers/include"
    cp "$OUT/build-flatc/flatc" "$DEPS/flatbuffers/bin/linux-x64/"
    cp -R "$SRC/flatbuffers/include/flatbuffers" "$DEPS/flatbuffers/include/"
fi

build_platform() {
    p=$1
    tc=$(toolchain "$p")
    lib=$DEPS/zlib/lib/$p/release
    if [ ! -e "$lib/libz.a" ]; then
        echo "[steam-audio] $p: zlib"
        cmake_build "$SRC/zlib" "$OUT/build-$p-zlib" zlibstatic -DCMAKE_TOOLCHAIN_FILE="$tc" -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        mkdir -p "$lib" "$DEPS/zlib/include"
        # libz.a on Linux, libzlibstatic.a on Windows: the same archive, kept as libz.a
        cp "$(find "$OUT/build-$p-zlib" -maxdepth 1 -name 'lib*.a' | head -1)" "$lib/libz.a"
        cp "$SRC/zlib/zlib.h" "$OUT/build-$p-zlib/zconf.h" "$DEPS/zlib/include/"
    fi
    lib=$DEPS/pffft/lib/$p/release
    if [ ! -e "$lib/libpffft.a" ]; then
        echo "[steam-audio] $p: pffft"
        # pentium4 = the SSE2 baseline (upstream asks for sandybridge/AVX here)
        cmake_build "$SRC/pffft" "$OUT/build-$p-pffft" PFFFT -DCMAKE_TOOLCHAIN_FILE="$tc" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DPFFFT_USE_TYPE_DOUBLE=OFF -DPFFFT_USE_FFTPACK=OFF -DPFFFT_USE_BENCH_GREEN=OFF -DPFFFT_USE_BENCH_KISS=OFF \
            -DPFFFT_USE_BENCH_POCKET=OFF -DTARGET_C_ARCH=pentium4 -DTARGET_CXX_ARCH=pentium4 -DPFFFT_USE_SIMD=ON
        mkdir -p "$lib" "$DEPS/pffft/include"
        cp "$OUT/build-$p-pffft"/libpffft.a "$lib/"
        cp "$SRC/pffft/pffft.h" "$DEPS/pffft/include/"
    fi
    lib=$DEPS/mysofa/lib/$p/release
    if [ ! -e "$lib/libmysofa.a" ]; then
        echo "[steam-audio] $p: mysofa"
        cmake_build "$SRC/mysofa" "$OUT/build-$p-mysofa" mysofa-static -DCMAKE_TOOLCHAIN_FILE="$tc" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DCODE_COVERAGE=OFF -DADDRESS_SANITIZE=OFF -DMATH=m \
            -DZLIB_ROOT="$DEPS/zlib/lib/$p/release" -DZLIB_INCLUDE_DIR="$DEPS/zlib/include" -DZLIB_LIBRARY="$DEPS/zlib/lib/$p/release/libz.a"
        mkdir -p "$lib" "$DEPS/mysofa/include"
        cp "$OUT/build-$p-mysofa"/src/libmysofa.a "$lib/"
        cp "$SRC/mysofa/src/hrtf/mysofa.h" "$DEPS/mysofa/include/"
        [ -e "$OUT/build-$p-mysofa/src/mysofa_export.h" ] && cp "$OUT/build-$p-mysofa/src/mysofa_export.h" "$DEPS/mysofa/include/"
    fi
    echo "[steam-audio] $p: core (a few minutes at -j$JOBS)"
    shim=
    [ "$p" = windows-x86 ] && shim="-I$TOOLS/winshim"
    cmake_build "$CORE" "$OUT/build-$p-core" phonon -DCMAKE_TOOLCHAIN_FILE="$tc" \
        -DSTEAMAUDIO_ENABLE_IPP=OFF -DSTEAMAUDIO_ENABLE_EMBREE=OFF -DSTEAMAUDIO_ENABLE_AVX=OFF -DSTEAMAUDIO_ENABLE_MKL=OFF \
        -DSTEAMAUDIO_BUILD_TESTS=OFF -DSTEAMAUDIO_BUILD_BENCHMARKS=OFF -DSTEAMAUDIO_BUILD_SAMPLES=OFF -DSTEAMAUDIO_BUILD_ITESTS=OFF \
        -DSTEAMAUDIO_BUILD_DOCS=OFF -DSTEAMAUDIO_BUILD_CSHARP_BINDINGS=OFF -DSTEAMAUDIO_STATIC_RUNTIME=ON \
        "-DCMAKE_CXX_FLAGS=-msse2 -mfpmath=sse $shim -Wno-everything" "-DCMAKE_C_FLAGS=-msse2 -mfpmath=sse -Wno-everything" -DCMAKE_SHARED_LINKER_FLAGS=
    mkdir -p "$OUT/$p"
    case $p in
    linux-x86) f=$(find "$OUT/build-$p-core" -name 'libphonon.so' -type f | head -1); dst=libphonon.so ;;
    windows-x86) f=$(find "$OUT/build-$p-core" -iname '*phonon.dll' -type f | head -1); dst=phonon.dll ;;
    esac
    [ -n "$f" ] || { echo "build.sh: $p: no library built" >&2; exit 1; }
    cp "$f" "$OUT/$p/$dst"
    zig objcopy --strip-all "$OUT/$p/$dst" 2>/dev/null || true
    cp "$HERE/LICENSE.md" "$HERE/THIRDPARTY.md" "$OUT/$p/"
    # the generated public header: the runtime's own declarations are checked against it
    cp "$CORE/src/core/phonon.h" "$OUT/build-$p-core/src/core/phonon_version.h" "$OUT/$p/" 2>/dev/null || true
    echo "[steam-audio] $p: $OUT/$p/$dst ($(wc -c < "$OUT/$p/$dst") bytes)"
}

case $TARGETS in
linux) build_platform linux-x86 ;;
windows) build_platform windows-x86 ;;
all) build_platform linux-x86; build_platform windows-x86 ;;
*) echo "usage: build.sh [linux|windows|all] [OUT]" >&2; exit 2 ;;
esac
