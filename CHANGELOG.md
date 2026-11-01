# Changelog

## v0.1.0 (beta) - 2026-11-01

First public release. Runs **your own** copy of *The Warriors* for the original Xbox natively on
Windows (and Linux from source). No game data is included.

### Runtime

- Native Xbox-to-PC runtime: the game's own `default.xbe` is loaded at its native addresses and its
  x86 code runs directly on the CPU, over a replacement Xbox kernel and high-level Direct3D 8,
  audio (XACT / DirectSound) and input APIs. No console emulation, no decompiled game code.
- Vulkan renderer, with SDL2 for the window, audio and controllers.
- 60 FPS mode (`--sim60`, experimental): the world update (AI, characters, cameras) really runs at
  60 Hz with half the time step, plus rate fixes for minigames and HUD timers authored for 30 fps
  and a frame-pacing lock.
- Widescreen (16:9), output resolutions up to 4K (`--resolution`, `--render-scale`), MSAA,
  supersampling, anisotropic filtering and contrast-adaptive sharpening (`--sharpen`).
- HD and soft character shadows; cleaned-up and frosted-glass HUD styles.
- Texture-pack support (`--texture-pack`, up to 8 layered packs) and texture dumping to build
  your own pack.
- In-game button icons for keyboard and mouse (key caps drawn from your key map) or Auto, which
  follows the device you used last.
- Rebindable keyboard and mouse, SDL game controllers (up to four, for co-op and versus), and
  `gamecontrollerdb.txt` read at start.

### Launcher

- Dear ImGui launcher with English and Spanish interface and an adjustable interface size.
- First-run wizard with ISO install: checks your own disc image and extracts it once.
- Presets (Performance, Quality) and Home, Game, Display, Controls and Options pages.
- Controls page with key rebinding and a step-by-step controller set-up for pads SDL maps wrongly
  (saved to `gamecontrollerdb.txt`); in-game button-icon style and an optional button-icon pack.
- Automatic save backups before each session, with restore.
- "Copy report for a bug": the last session's report on the clipboard with personal paths removed.

### Known issues

See [README.md](README.md#known-issues).
