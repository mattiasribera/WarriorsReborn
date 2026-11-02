# Changelog

## Unreleased

### Releases

- Release zips are now built by GitHub Actions from the tag (`.github/workflows/release.yml`),
  with every toolchain and dependency download pinned and checked against its SHA-256, and come
  with a build-provenance attestation. See [Verifying a download](README.md#verifying-a-download).
- New `tools/package_release.py` assembles the zip and `SHA256SUMS.txt` the same way locally.

## v0.1.1 (beta) - 2026-11-02

New picture options and better support for several controllers.

### Display

Four new options on the Display page. Choosing the **Quality** preset turns all of them on;
**Performance** leaves them off, so the game looks and runs as in v0.1.0.

- **Bloom** (`--bloom`): street lamps, neon signs and fire give off a soft glow. The HUD and text
  stay sharp, and the lamps' halos keep their original look. A slider sets the strength.
- **Shadow range** (`--shadow-range`): character shadows no longer fall on floors far below them,
  for example from the hideout walkway onto the floor underneath.
- **Better rain** (`--rain-hd`): while it rains, the flat rain overlay is replaced by thin
  streaks falling at several distances. Walls, cars and people in front of the camera hide the
  streaks behind them.
- **Ambient occlusion** (`--ssao`): corners, joints between walls and floors, and the ground
  under cars, props and people get soft contact shading. A slider sets the strength.

Every option is listed in [docs/ARGUMENTS.md](docs/ARGUMENTS.md).

### Controllers

- New Controllers card on the Controls page: every connected controller with a name you can
  change and the player it is (Player 1 to 4, or Auto).
- A controller with a chosen player goes back to that player when you plug it in again during
  the game. Identical controllers are told apart by the order they were connected.
- New `--pad-slot N=GUID[#K]` option for the same thing from the command line.

### Fixes

- The launcher's install texts are shorter and plainer.
- Test fixes: the text-fix test reads its output as Latin-1, and the texture-pack test runs in a
  fresh temporary folder, so it passes from any directory and on a second run.

### Known issues

See [README.md](README.md#known-issues).

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
