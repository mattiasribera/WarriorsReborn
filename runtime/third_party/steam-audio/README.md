# Steam Audio (third-party)

Valve's spatial audio library, [Steam Audio](https://github.com/ValveSoftware/steam-audio) v4.8.1,
Apache License 2.0 (`LICENSE.md`). WarriorsReborn uses it for the HRTF headphone render, occlusion
and transmission through the level's geometry and the location-based reverb.
The runtime loads the library at run time (`src/steam_audio.c`), like
SDL2: without it every sound plays exactly as before.

## What is here

| File | What |
| --- | --- |
| `build.sh` | fetches the pinned sources, patches them and builds both 32-bit libraries with zig |
| `patches/steam-audio-zig-i386.patch` | our changes to Steam Audio (see its header) |
| `patches/flatbuffers-clang.patch` | one FlatBuffers 1.12 header fix for current clang |
| `include/phonon.h`, `include/phonon_version.h` | Steam Audio's public C header, unchanged (the runtime is compiled against it) |
| `LICENSE.md`, `TRADEMARK_RIGHTS.md` | Steam Audio's licence and trademark notice, unchanged |
| `THIRDPARTY.md` | the notices of the code and data inside the built library |

The source is fetched at build time instead of being committed: the core library's source is
22 MB, 19 MB of it the CIPIC HRTF table (`cipic_124.inl`). `build.sh` fetches exact commits and
checks them:

| Source | Commit |
| --- | --- |
| Steam Audio (tag v4.8.1) | `0da18255cca520771f363ee01f100572b39a308e` |
| FlatBuffers (1.12) | `6df40a2471737b27271bdd9b900ab5f3aec746c7` |
| PFFFT | `e0bf595c98ded55cc457a371c1b29c8cab552628` |
| zlib (1.3.1) | `51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf` |
| MySOFA | `dd315a8ec1fee7193d40e4a59b12c5590a4a918c` |

(the dependency commits are the ones Steam Audio's own `core/build/dependencies.json` pins).

## Building

    make steam-audio          # in runtime/: both platforms
    third_party/steam-audio/build.sh linux|windows|all [OUT]

Needs git, cmake, make and zig 0.17; the first run downloads about 30 MB. Results:
`build/steam-audio/linux-x86/libphonon.so` (about 6.6 MB) and
`build/steam-audio/windows-x86/phonon.dll` (about 7.2 MB). Configuration: Intel IPP, Embree, AVX,
GPU backends, tests, samples and docs off; SSE2 baseline (`-msse2 -mfpmath=sse`); zlib, PFFFT and
MySOFA linked in statically; zig's libc++ linked in statically (the Linux library needs only glibc,
the Windows one only the UCRT, like the runtime).

Why not the official binaries: the official `linux-x86/libphonon.so` is built with
`-march=native` and executes AVX-512 instructions while creating the HRTF, so it crashes (SIGILL) on
most CPUs; the official Windows DLL is 46 MB with Intel IPP inside. The runtime also accepts the
official `phonon.dll` (it looks up both the undecorated and the MinGW `name@N` export names).

## Shipping

Put the library next to the runtime, with the two notice files:

- Linux: `libphonon.so` next to `xbrun`;
- Windows: `phonon.dll` next to `TheWarriors.exe`;
- `LICENSE.md` and `THIRDPARTY.md` (copied next to each library by `build.sh`).

A development build also finds it in `build/steam-audio/<platform>/` next to the program.

Credit text (plain text only; `TRADEMARK_RIGHTS.md` allows naming the origin, not implying
endorsement, and no logos): "Spatial audio: Steam Audio (Apache 2.0). HRTF: CIPIC HRTF Database,
Copyright (c) 2001 The Regents of the University of California."
