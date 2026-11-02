# WarriorsReborn

A native Windows and Linux runtime that runs **your own copy** of *The Warriors* for the original
Xbox on a PC.

It loads the game's original `default.xbe` from your disc at its native addresses and runs the
game's own x86 code directly on the CPU. The runtime supplies what the console would: a
replacement for the Xbox kernel, and high-level implementations of the Direct3D 8, audio (XACT /
DirectSound) and input APIs, drawn with Vulkan and played through SDL2.

What it is not:

- **Not an emulator of the whole console.** There is no CPU, GPU or APU emulation; only the API
  boundary the game calls is reimplemented on the host.
- **Not a decompilation.** The repository contains no game code, decompiled or otherwise, and no
  game data. The game's code and assets only ever come from the disc you provide.

## Features

- **60 fps mode** (`--sim60`, experimental) with rate fixes for minigames and HUD timers authored
  for 30 fps, plus a frame-pacing lock for smooth presentation.
- **Widescreen** (16:9) reported to the game, so it widens its own view.
- **1080p and higher** output resolutions (`--resolution`, `--render-scale`).
- **Supersampling and MSAA** (`--supersample 2|4`, `--msaa 2|4|8`) and anisotropic filtering.
- **Sharpening**: contrast-adaptive sharpening of the finished frame (`--sharpen 1-100`).
- **HD shadows**: higher-resolution and optionally soft character shadow maps.
- **Shadow range** (`--shadow-range`): character shadows no longer fall on floors far below them.
- **Bloom** (`--bloom`): street lamps, neon and fire give off a soft glow; the HUD and text stay sharp.
- **Better rain** (`--rain-hd`): thin falling streaks at several distances instead of the flat
  rain overlay, lit by the street lamps, with splashes and ripples on the ground
  (`--no-rain-hd-splash` turns those off).
- **Ambient occlusion** (`--ssao`): soft contact shading in corners and under people and objects.
- **HUD styles**: cleaned-up and frosted-glass health bars (`--hud-hd`, `--hud-hd-glass`, `--hud-glass`).
- **Texture-pack support**: replacement textures loaded at run time (`--texture-pack DIR`).
- **Spanish UI** and the game's other languages (`--language en|es|fr|de|it`), with small text
  fixes applied in memory.
- **Launcher** (Dear ImGui) with a first-run wizard, presets and **ISO install**: pick your own
  Xbox disc image and the launcher checks and extracts it.
  - English or Spanish interface, with an adjustable interface size (Auto, 1x, 1.25x, 1.5x).
  - Controls page to rebind keyboard and mouse, and a step-by-step set-up for a controller that
    SDL maps wrongly or does not know (written to `gamecontrollerdb.txt`).
  - Controllers card: name each controller and choose which player (1-4) it is, or Auto
    (`--pad-slot`).
  - In-game button icons: the game's own (Xbox), keyboard and mouse key caps drawn from your key
    map, or Auto (follows the device you used last); an optional button-icon pack.
  - Automatic **save backups** before each session, with restore.
  - "Copy report for a bug": a plain-text report of the last session with personal paths removed.
- Keyboard and mouse controls (rebindable), SDL game controllers, fullscreen toggle.

## Install (Windows)

1. Download `WarriorsReborn-v0.1.2-windows-x64.zip` from the release page and unzip it into any
   folder (keep its files together).
2. Run `warriors-launcher.exe`.
3. In the first-run wizard, point it at **your own legally obtained** Xbox disc image (`.iso`).
   The launcher checks that it is The Warriors and copies the game files out of it once (about
   5.2 GB, next to the launcher by default). Your `.iso` is only read. Nothing is downloaded.
4. Pick a preset (**Performance** or **Quality**) and press **PLAY**.

Requirements:

- A legally obtained copy of *The Warriors* for the original Xbox, as a disc image (`.iso`/xiso)
  or an extracted disc folder that contains `default.xbe`.
- **Windows 10 or 11, 64-bit**.
- A GPU and driver with **Vulkan** support (Vulkan 1.0 or newer; current NVIDIA, AMD and Intel
  drivers include it).

Optional, not included: an HD texture pack (built with the tools in `tools/` from textures the
runtime dumps from **your own** copy, see "Optional tools") and a button-icon pack (your own edited
copy of the game's button-icon atlas, see `runtime/README.md`). Both are chosen in the launcher.

Linux (from source): x86_64 with 32-bit glibc (`libc6:i386`), the 32-bit SDL2 library
(`libsdl2-2.0-0:i386`) and a 32-bit Vulkan driver; run `launcher/build/warriors-launcher`.

See [`launcher/README.md`](launcher/README.md) for every launcher setting and
[`runtime/README.md`](runtime/README.md) for the runtime's command line, controls and design notes.
Every game option you can type in the launcher's **Extra arguments** field is listed in
[`docs/ARGUMENTS.md`](docs/ARGUMENTS.md).

## Verifying a download

Each release zip is built from its tag by GitHub Actions (`.github/workflows/release.yml`), which
records a signed build-provenance attestation for it. With the [GitHub CLI](https://cli.github.com/),
check that a zip you downloaded was built by this repository's workflow, and that it matches
`SHA256SUMS.txt` from the same release page:

```sh
gh attestation verify WarriorsReborn-v0.1.2-windows-x64.zip --repo mattiasribera/WarriorsReborn
sha256sum -c SHA256SUMS.txt
```

On Windows without these tools, compare the output of
`certutil -hashfile WarriorsReborn-v0.1.2-windows-x64.zip SHA256` with `SHA256SUMS.txt`. The zip
can also be rebuilt from source with `tools/package_release.py` (see `tools/README.md`).

## Known issues

- **60 FPS mode is new.** If a cutscene or minigame looks or plays wrong, switch the frame rate to
  **30** (Display page) and report it.
- An **AI-upscaled texture pack** can make the characters of the final level look white on some
  PCs. Not reproduced yet; if you see it, play that level without the texture pack.
- **Controller remapping on Windows**: XInput controllers (Xbox-style pads) share one mapping, so
  remapping the buttons of one XInput pad changes all XInput pads.
- The executables are **not code-signed**, so Windows SmartScreen or an antivirus may warn when you
  first run them. Choose "More info" > "Run anyway" only if you downloaded the zip from this
  project's release page and its SHA-256 matches `SHA256SUMS.txt`.

## Building from source

Everything is built with [zig](https://ziglang.org/) (tested with 0.17.0) from a Linux host; the
Windows binaries are cross-compiled.

```sh
# Linux host packages (Debian/Ubuntu names)
sudo dpkg --add-architecture i386
sudo apt install libc6:i386 libsdl2-2.0-0:i386 libvulkan-dev glslang-tools

# Runtime: Linux binary (xbrun) and test programs
make -C runtime -j4 VULKAN_INCLUDE=/usr/include GLSLANG=glslangValidator
# Runtime: Windows (TheWarriors.exe + xbrun-core.dll)
make -C runtime -j4 windows VULKAN_INCLUDE=/usr/include GLSLANG=glslangValidator

# Launcher: SDL2 development package once, then Linux and Windows builds
make -C launcher sdl2-devel
make -C launcher -j4
make -C launcher -j4 windows

# Tests (no game data needed; tests that need it are skipped)
python3 -m unittest discover -s tests -p 'test_*.py'

# Public-tree guard (also runs in CI)
python3 tools/check_public_tree.py
```

The `VULKAN_INCLUDE` and `GLSLANG` defaults point at a Homebrew installation; override them as
above when using distribution packages. Run the runtime directly with:

```sh
runtime/build/xbrun <disc>/default.xbe <disc> --hdd <save-dir> [options]
```

## Optional tools

These tools work only on **your own** copy of the game and write their output to local,
git-ignored folders. Nothing they produce belongs in this repository or in a release.

| Tool | Purpose |
|---|---|
| `tools/xdvdfs_extract.py` | List and extract files from an Xbox (XDVDFS) disc image |
| `tools/wad_extract.py` | Unpack the game's WAD archives from your disc |
| `tools/texture_upscale.py`, `tools/spandrel_infer.py` | Build an upscaled texture pack from textures the runtime dumped from your copy (Real-ESRGAN or PyTorch/spandrel models) |
| `tools/texture_review.py` | Review page to approve or reject upscaled textures |
| `tools/icon/make_icon.py` | Draws the application icon from geometry (no game artwork) |
| `tools/d3d_census.py`, `tools/scan_rw_chunks.py` | Developer diagnostics on your own `default.xbe` / disc files |
| `tools/ratediff/` | Developer diagnostic comparing 30 Hz and 60 Hz runs |

## Third-party software

| Component | Used by | Licence |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | launcher GUI (`launcher/third_party/imgui`) | MIT |
| [Inter](https://rsms.me/inter/) font | launcher GUI (`launcher/third_party/inter`) | SIL Open Font License 1.1 |
| [stb_image / stb_image_write](https://github.com/nothings/stb) | texture packs (`runtime/third_party/stb`) | MIT or public domain |
| [SDL2](https://www.libsdl.org/) | window, input, audio (loaded at run time, shipped as `SDL2.dll` on Windows) | zlib |
| [Vulkan](https://www.vulkan.org/) headers and loader | renderer | Apache-2.0 |

Optional tools may use Pillow, NumPy, PyTorch, spandrel and Real-ESRGAN, which are installed
separately under their own licences. The licence texts of bundled components are kept next to
their sources.

## Legal

- **No game files are included** in this repository or in its releases: no executable, code,
  scripts, text, textures, audio, video or fonts from the game. You must own a legitimate copy of
  *The Warriors* and supply it yourself.
- This project is an independent, non-commercial runtime. It is **not affiliated with, endorsed
  by or sponsored by Rockstar Games, Take-Two Interactive, Paramount or Microsoft**.
- *The Warriors* and all related names and marks are trademarks of their respective owners;
  "Xbox" is a trademark of Microsoft. They are used here only to identify the game and platform
  the runtime is compatible with.
- Do not open issues or pull requests containing game files, extracted assets, screenshots of
  game content or decompiled game code; they will be removed.

## Licence

Copyright (C) 2026 Mattias Ribera Rojas

WarriorsReborn is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version. It is distributed WITHOUT ANY WARRANTY; see
[LICENSE](LICENSE) for the full text.

This licence covers the code in this repository only. Third-party components keep their own
licences (see "Third-party software"). *The Warriors* and all of its content belong to their
owners and are not part of this project.
