# Changelog

## v0.1.6 (beta) - 2026-11-07

Lamps and fires that light the whole scene, glowing neon, much less CPU per frame and a launcher
that knows your graphics card.

### Display

- **World lighting** (`--world-lighting`, `--world-lighting-strength`): the lamp lights fall on
  walls, floors, buildings and props per pixel, following their surfaces, added to the game's
  baked light so the night mood, shadows and fog stay.
- **Character lighting** (`--char-lighting`): the same lights fall on people per pixel, following
  the shape of their bodies and faces.
- **Fire light** (`--fire-light`, `--fire-light-strength`): burning fire barrels cast a warm,
  gently flickering light on the walls, floor and people near them, the same at 30 and 60 FPS.
- **Neon glow** (`--neon-glow`, `--neon-glow-strength`, `--neon-glow-bloom`,
  `--neon-glow-flicker`): lit and neon signs keep their own colour through fog and darkness and
  feed the bloom. It needs emission masks that you make from your own copy (see
  [docs/TEXTURE_PACKS.md](docs/TEXTURE_PACKS.md)); none ship, and without masks nothing glows.
- **Show FPS** (`--fps-overlay`, `--fps-overlay-items`, `--fps-overlay-corner`): a small counter
  in a corner with any of FPS, frame time, CPU and GPU use. No preset turns it on.
- **Sharp faces over a texture pack** (`--sharp-faces-over-pack`): faces stay sharp even when the
  texture pack replaces them.

### Texture packs

- Pack files are read and decoded on loader threads with a per-frame upload budget, so a level
  no longer stalls while its textures load (`--texpack-sync` restores the old behaviour).
- The texture cache fits the memory the graphics card reports as free (`VK_EXT_memory_budget`),
  capped by `--texpack-budget MB`.
- BC1/BC3/BC7 `.dds` packs, made with `tools/texture_compress.py`: smaller and faster to load.
  Keyboard and mouse icons also work with a `.dds`-only pack.
- New guide: [docs/TEXTURE_PACKS.md](docs/TEXTURE_PACKS.md), building your own pack step by step.

### Performance

- **GPU vertex programs** (`--gpu-skinning`, `--gpu-vertex-programs`, on by default): the game's
  character, street, building and prop vertex programs run on the graphics card instead of the
  CPU, with the same picture. The launcher keeps the world's programs on the CPU for integrated
  and entry-level cards, where the GPU is the limit.
- **Pipeline cache** (on by default, `--no-pipeline-cache` turns it off): compiled pipelines are
  kept in the `--hdd` folder and those used last time are prepared in the background on the next
  start, so places already visited do not stutter.
- Frame vertices are written straight into the upload buffer (`--no-direct-geometry` goes back),
  and waiting game threads are woken one by one instead of all at once.
- **Skip intro** (`--skip-intro`, on in every preset): the title screen is ready in about a second
  instead of about 54.

### Launcher

- **Graphics card detection** through Vulkan (name, driver, video memory and its budget, BC
  texture support) with a tier table, and a **recommended preset** shown on the wizard's graphics
  step and the Display page. Any preset can still be picked; saved profiles are never changed.
- **Video-memory meter**: how much of the card's memory the chosen settings and packs need.
- Toggles for world lighting, character lighting (Balanced and Quality), fire light (Quality),
  neon glow (Balanced and Quality, with a `neon-masks` folder next to the game), Show FPS and
  sharp faces over a pack.

### Diagnostics

- Crash breadcrumbs and HLE, Vulkan and audio error lines in the log, so a bug report shows what
  happened last.
- Always-on `[hitch]` lines for slow frames and a `[level-summary]` per level in the log;
  `--tri-check` logs exploded or non-finite vertices.

### Fixes

- Mission 14 starts on Windows again (the guest memory arena stays usable).
- 60 FPS pacing keeps its 60 Hz grid after a late wake-up, and with `--sim60` half-frame poses
  are sampled inside their key interval (no pops on the ground).
- Draws whose vertices read past their stride no longer show garbage with the vertex-program
  workers.

### Known issues

See [README.md](README.md#known-issues).

## v0.1.5 (beta) - 2026-11-04

A cinematic look, a real night sky, lamps that light the street, and sharper text and faces.

### Display

- **Cinematic look** (`--filmic`): the streets are graded like the 1979 film, with warm lights
  and a red glow, teal shadows and deep blacks. The HUD, text and menus stay as they are.
  `--filmic-strength`, `--filmic-grain`, `--filmic-vignette` and `--filmic-lut FILE.cube` tune it.
- **Enhanced sky** (`--sky-hd`): a sharper night sky in each level's own colours, with a sparse,
  hazy star field, drifting clouds, a moon and lit windows on the distant skyline.
- **Lamp lights** (`--lamp-lights`, `--lamp-lights-strength`): street lamps, fires and neon light
  up the ground, walls and people near them.
- **Contact shadows** (`--contact-shadows`, `--contact-shadows-strength`): the ground right under
  people, cars and props gets a short, soft shadow, so they stand on it instead of floating.
- **Puddles** (`--rain-hd-puddles`, with better rain): puddles form in low spots of the open
  street, reflect the scene and ripple where drops land. Never under roofs, on walls or on people.
- **Sharp text** (on by default): menu, HUD, mission and subtitle letters are rebuilt at 4x from
  the game's own font when they first show, so they stay crisp at 1080p and above. Button icons
  keep their look. `--no-sharp-fonts` shows the letters as shipped.
- **Sharp faces** (`--sharp-faces`): character faces and clothes at twice the detail, made from
  your own game's textures the first time each one appears. A texture pack's version wins.
- **Soft particles are back as an option** (`--soft-particles`, Display page): smoke, fire and
  dust fade out where they meet the ground and walls. Graffiti tags and other sprites fixed on
  walls now stay sharp at any view angle. No preset turns it on.

### Audio

- **Room reverb** (`--reverb`, on by default): the reverb each place describes is now heard, and
  positioned sounds send to it. `--no-reverb` turns it off.
- **Doppler** (`--doppler`, on by default): moving sound sources change pitch as they pass.
- **Front/back cue** (`--rear-cue`, on by default): sounds behind you are slightly muffled, so you
  can tell them from sounds in front.

### Launcher

- **Balanced preset** between Performance and Quality: MSAA 2x, soft shadows, bloom, better rain
  with puddles, a farther draw distance, more effects, the cinematic look, the enhanced sky and
  sharp faces. **Quality** adds ambient occlusion, contact shadows and lamp lights. Sharp text is
  on in every preset.
- **First-run preset from your graphics card**: a new profile starts on Performance for
  integrated graphics, Balanced for a card with less than 4 GB, and Quality for 4 GB and more
  (Balanced when the card cannot be identified). The wizard and Home say which preset was picked
  and why. Saved profiles are never changed.
- New toggles on the Display page for every option above.

### Performance

- Less CPU time per frame: the vertex-program workers no longer spin while waiting, guest memory
  checks are answered from a page map on Linux, and the depth projection is fitted once per frame.

### Fixes

- With the 60 FPS mode (`--sim60`), litter (its timings, wind and tumbling) and the airborne
  fail-safe now run at game speed instead of twice as fast.
- Removed the known issue about an AI-upscaled texture pack making the characters of the final
  level look white: a texture that cannot be created no longer leaves characters untextured.

### Known issues

See [README.md](README.md#known-issues).

## v0.1.4 (beta) - 2026-11-04

HD movies, cleaner sound, a farther view and more effects on screen.

### Display

- **HD movies** (`--movie-hd`, on by default): the disc's 1280x720 movies play instead of the
  640x480 ones, drawn sharp at your resolution. Turn **HD movies** off on the Display page (or use
  `--no-movie-hd`) for the original movies.
- **Draw distance** (Display page: Original, Far, Farther, Maximum; `--draw-distance`,
  `--prop-detail`): the fog and the scenery reach farther, and small objects such as bins and
  crates stay visible farther away. Original is the game as it shipped.
- **Steady fog**: the fog no longer creeps closer for a moment after a slow frame.
  `--fog-breathing` brings the old behaviour back.
- **Effects and particles** (Display page: Normal, More, Maximum; `--fx-scale`): more blood,
  debris, smoke, glass and sparks can be on screen at once, and blood stays on the ground through
  long fights.
- **Ambient occlusion** costs less, most of all with supersampling, and fades out on glowing
  lamps and neon instead of darkening them.
- **Supersampling** is now off in both presets: it costs four times the pixels and slowed most
  PCs. You can still turn it on yourself.
- The cigarette prop has a new, sharper built-in texture (a hand-rolled look, 64x64 instead of
  16x16), also with a texture pack.

### Audio

- **Cleaner sound**: voices, effects and music are converted to the 48 kHz output with a
  high-quality resampler instead of plain linear interpolation, so high sounds no longer pick up
  a harsh, grainy edge. `--resampler linear` restores the old conversion.
- **Sound packs** (`--sound-pack DIR`, or **Sound pack** on the Game page): play your own WAV
  files in place of game sounds, matched by the sound's name. `tools/audio_extract` writes the
  game's sounds as named WAVs from your own disc to start from.

### Characters (experimental)

- **Smoothed character models** (`--mesh-pack DIR`): characters can be drawn with rounder,
  smoothed meshes that keep their skeleton, so every animation still works. Make them from your
  own copy of the game with `tools/mesh_pack` and put the `.wmesh` files in the new
  `characters-hd` folder next to the launcher: the launcher then uses them by itself and the Home
  page shows how many models it found. The folder ships empty; no models are included.

### Fixes

- With the 60 FPS mode (`--sim60`), climbers no longer pop about a metre for one frame at the
  start of every climb.

### Known issues

See [README.md](README.md#known-issues).

## v0.1.3 (beta) - 2026-11-02

Wet streets and rain that behaves better.

### Display

- **Wet streets** (`--rain-hd-wet`, on with `--rain-hd`): while it rains, streets, pavements and
  car roofs get a little darker and glossy and reflect street lamps, windows and signs. The HUD
  and text are never reflected. `--rain-hd-wet-strength F` (0.1-4, default 1.8) makes the
  reflections and darkening stronger or weaker; `--no-rain-hd-wet` turns wet streets off.
- **Rain fades in and out**: when the rain starts or stops, the streaks, splashes and wet streets
  fade over half a second instead of switching on and off at once.
- **No rain under roofs** (`--rain-hd-cover`, on with `--rain-hd`): streaks, splashes and wet
  ground stay out from under roofs, awnings and bridges. `--no-rain-hd-cover` turns this off.
- The curved tops of characters, such as shoulders, no longer look wet.
- No more light patches around characters in the rain: the street under and next to them is
  as wet as elsewhere, with no pale footprint, rim or bright spot at the neck.

## v0.1.2 (beta) - 2026-11-02

Better-looking rain and verifiable release downloads.

### Display

- **Better rain** (`--rain-hd`) looks more natural: the streaks catch the scene's light (bright
  near street lamps, faint in the dark), each streak sits at its own distance instead of in flat
  layers, and see-through fences and wire no longer cut rain-free panels out of it.
- **Splashes and ripples** (`--rain-hd-splash`, on with `--rain-hd`): drops landing on streets,
  car roofs and crate tops throw up tiny splashes, and rings widen and fade on flat ground. They
  stay in place when the camera moves. `--no-rain-hd-splash` keeps the streaks without them.
- The game window is now titled **WarriorsReborn**.

### Releases

- Release zips are now built by GitHub Actions from the tag (`.github/workflows/release.yml`),
  with every toolchain and dependency download pinned and checked against its SHA-256, and come
  with a build-provenance attestation. To check that a zip you downloaded was built by this
  repository's workflow, run
  `gh attestation verify WarriorsReborn-v0.1.2-windows-x64.zip -R mattiasribera/WarriorsReborn`
  (see [Verifying a download](README.md#verifying-a-download)).
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
