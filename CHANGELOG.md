# Changelog

## v0.1.9.1 (beta) - 2026-11-27

- **Linux AppImage:** the launcher no longer says the game program is "Not found" when its profile
  already had another one saved (`TheWarriors.exe` from a Windows or Proton setup, or a path inside
  an earlier run of the AppImage). It now uses the `xbrun` inside the AppImage, which cannot be
  chosen by hand. (#9)

## v0.1.9 (beta) - 2026-11-26

An in-game settings menu, HDR10, photo mode, PlayStation button icons built from your own game, a
Linux AppImage, seven new example mods (a cheats menu among them), per-axis mouse sensitivity and a
calmer look in the rain.

### Display

- **In-game settings menu** with tabs: most settings apply live at the next frame; the ones that
  need a restart say so. Changes are kept in the launcher profile.
- **HDR10 output** (`--hdr`) and a colour **saturation** setting (`--saturation`), also on the
  launcher's Display page.
- **Bounce lighting** (`--ssgi`): screen-space indirect light, so lit walls tint what is near them.
- **Photo mode**: freeze the game, move the camera, take a maximum-quality PNG; the panel can be
  hidden for a clean shot.
- **Rain**: puddles only on flat ground (about an eighth of it, mostly asphalt), wet ground darkens
  far less, and reflections are soft outside the puddles. Neon reflections are a little dimmer.
- **64-bit renderer (experimental)**: an advanced launcher option that renders through a separate
  x86_64 process (`--render-process helper`).
- The game closes with a message instead of hanging when the graphics driver resets
  (`VK_ERROR_DEVICE_LOST`).

### Controls

- **PlayStation button icons** (ps-black, ps-white, ps-color, and "auto" with a PlayStation
  controller): the game draws them itself on its icon atlas at whatever size it uses (no download,
  no Python, no glyph pack; a glyph-packs folder still wins). New `--pad-icons`, and
  `--button-icons` accepts the PlayStation styles.
- **Mouse sensitivity per axis** (`mouse.sensitivity_x`, `mouse.sensitivity_y`) with launcher
  sliders. The default sensitivity is now 2.0 (the old 1.0 felt slow); `mouse.sensitivity` still
  scales both. (#7)

### Linux

- **AppImage** for desktop Linux and Steam Deck (`WarriorsReborn-0.1.9-x86_64.AppImage`): a
  64-bit launcher, the game's 32-bit side runs on its own bundled libraries (no i386 packages
  needed on the host) and draws through the 64-bit renderer. Saves, profiles and mods go to
  `WarriorsReborn-data/` next to the AppImage (or `$XDG_DATA_HOME/WarriorsReborn`). See
  docs/LINUX.md. Steam Deck is untested so far: reports welcome.

### Mods

- New example mods (all off by default):
  - **Cheats menu**: the game's own cheats from a menu (hold Back + LB, or F8) in a mission or in
    free roam.
  - **No combo slow motion**: single player plays the X+A, A, A combos at full speed, as co-op does. (#7)
  - **Cowboy keeps his hat**. (#7)
  - **Gang colours on the radar**.
  - **Jukebox**: pick the radio song at the hideout's boom box.
  - **Sparring ring**: after the story, fight any gang you beat in the hideout's cage room, with
    records per profile.
  - **A crate of our own**: an example of a mod with its own model and sounds.
- Dirty Fights 1.5: more headgear flies off, with proper knock-off poses.
- Protection money: Hot Dog Jimmy and the Gypsy pay too.
- Brutal fights: blood stays on the right fighter; cutscenes show the game's own amount.
- Cleon loses his bandana in rumble arenas too.
- Cleon's bandana and dirty clothes are in the release zip and the AppImage: the game builds
  the meshes and texture layers they need from your own game files the first time they load.
- For mod makers: a mod's own `.glb` meshes, PNG/DDS textures, WAV/OGG sounds and collision;
  `ModSound`, `ModRadarColor`, value menus (`ModMenuValues`) and an open-anywhere menu key
  (mod runtime 1.11). See docs/MODDING.md.

### Fixes

- No crash when going back from a Rumble to the hub, and no crash at boot with every bundled mod on
  (the mods' Lua memory is larger now).
- "Set Up": the machete guy can no longer leave the mission stuck (`--no-script-fixes` turns the
  mission fixes off).
- The Windows files carry version information (0.1.9, WarriorsReborn).

## v0.1.8 (beta) - 2026-11-18

Sharper and smoother pictures (temporal anti-aliasing and upscaling, dynamic resolution, frame
generation for 120 Hz screens), Steam Audio sound, a Lua mod layer with 11 example mods, and a
benchmark in the launcher.

### Display

- **Temporal anti-aliasing** (`--taa`, `--taa-sharpness`): smooths jagged and shimmering edges,
  fences and wires included, with motion for the camera and for skinned characters.
- **Temporal upscaling** (`--taa-upscale ultra|quality|balanced|performance`): the 3D scene renders
  smaller and the TAA history rebuilds the full size, with RCAS sharpening. `ultra` (1.3x per
  axis) is the Quality preset's choice.
- **Dynamic resolution** (`--dynres TARGET_MS|auto`, `--dynres-min`): the render size follows each
  frame's GPU time to hold 60 FPS. On in every preset that upscales.
- **Frame generation** (`--frame-gen`, `--frame-gen-of`): a frame between every two real ones, so a
  120 Hz screen shows 120 frames a second. Costs about 1.6 ms of GPU time per generated frame on an
  RTX 2060 at 1080p and adds about 10 ms of input latency. Off in every preset.
- **Shaders compiled off the render thread**: geometry pipelines are built in the background, so
  new places stutter less; the frame's geometry stays in video memory.
- **Ultra preset**: Quality at full resolution with the lamps' character shadows. Quality is now
  sized for an RTX 2050-class laptop GPU at 60 FPS.
- New looks, most of them on in Quality: ambient occlusion with XeGTAO (`--ssao-mode gtao`,
  Quality's default), volumetric fog (`--volfog`), eye adaptation and cutscene depth of field
  focused on the speaker (with the filmic look), damp night streets (`--damp-streets`), lamps and
  neon signs mirrored in wet ground (`--wet-light-streaks`, `--neon-reflections`), wet ground by
  surface material (`--materials`), screen-space reflections (`--reflections sssr`, opt-in), the
  level's own lights (`--level-lights`), world shadows, lights and lamp shadows stopped by walls,
  parallax relief and metal and glass highlights from a relief pack, better hair, skin, eyes and
  clothing on characters, and worn hats lit smoothly and rounder (`--hat-fix`, `--hat-subdiv`).
- Optional smooth animation for 30 FPS mode (`--anim-interp`).
- Fixes: no stripes on wet ground with upscaling, puddle reflections no longer pop, no light
  streaks through walls, the game's blur after a spray or mace in the face shows again, decals
  show through chain-link fences, steadier TAA on moving characters.

### Sound

- **Steam Audio** (`phonon.dll`, built from source): measured HRTF for headphones
  (`--audio-hrtf`), occlusion through the level's geometry, location-based reverb
  (`--audio-reverb-geo`) and sound around corners (`--audio-pathing`). Without the library every
  sound plays as before.
- Air absorption and wall occlusion for 3D sounds (`--audio-air`, `--audio-occlusion`).
- The game's pitch control works (the Flash high slows the sound), finished cues keep the
  console's stop latency, and the "Go" beeps of the Armies of the Night mini-game keep the
  console's cadence at 60 FPS.

### Gameplay

- **Lua mods** (`--mod`, `--mods`, `--no-mods`): your own scripts hook the game's Lua. 11 example
  mods ship in `mods/`, all off by default; see [docs/MODDING.md](docs/MODDING.md).
- **Mod safety**: the saves are backed up before a mod that changes saves runs (with restore in the
  launcher), mod folders are checked at load, and after a crash with mods on the launcher offers a
  run without them.
- Opt-in **smarter allies** (`--smart-allies`) and **more tactical enemies** (`--enemy-tactics`).
- Controller buttons can be remapped (`pad.BUTTON = CONTROL` in `warriors-input.cfg`).
- Fixes: no crash when many mods are enabled together (the game's Lua garbage collector no longer
  frees live tables while mods load); the gym workouts can be finished with keyboard and mouse; the Rumble character select no
  longer crashes (the memory registry holds 2048 nodes); with `--sim60` enemy counters, AI timers,
  scene camera cuts and cutscene props keep the console's timing.

### Launcher

- **Mods page**: turn each mod on or off per profile; save backups and a run without mods after a
  crash.
- **Run benchmark** (`--bench`): measures the chosen settings in a fixed street of mission 7 and
  writes a report.
- Toggles for every new option, the frame generation toggle for 120 Hz screens, and the temporal
  upscaling choice in Display > Image.
- Keyboard and mouse key icons without the white border, D-pad prompts with your keys, controller
  icons that follow the button remap, and the gym's mash key on the Controls page.
- Wizard fixes: PLAY stays off until the wizard is done, choice cards fit their text; neutral
  Spanish in every string.

### Tools

- `tools/texture_basis.py`: KTX2 (Basis Universal) texture packs, smaller on disk; the runtime
  loads `.ktx2` files.
- `tools/relief_from_albedo_ai.py`, `tools/material_shine.py`, `tools/parallax_height.py`: relief,
  shine and height maps for a texture pack from local open models (downloaded on first use).
- `tools/texture_unblend.py`: makes the dark panel of glowing sign textures (the Wonder Wheel sign)
  transparent.
- `tools/xbrun-slot`: one headless game run at a time.

### Known issues

- **Mods**: two example mods, Cleon's bandana (`40-cleon-bandana`) and dirty clothes
  (`88-dirty-clothes`), are not in the release zip: they need meshes or texture layers made from
  your own game files and come with the in-game asset builder in a later version. Two others work
  in part until then: Dirty fights without the glasses that break (`80-dirty-fights`) and Hideout
  trophies without Cleon's necklace (`20-hideout-trophies`). Their log says
  `[mods] <id>: optional meshes not installed (folder <f>); the mod runs without them`.
- More: see [README.md](README.md#known-issues).

## v0.1.7 (beta) - 2026-11-13

Voices you can hear again, solid cutscene bars, FSR 1 upscaling, lamp shadows and light shafts,
and the game's own brightness.

### Sound

- **3D voices and effects fade like on the Xbox**: positioned sounds now fade with distance along
  the game's own fade-out curves instead of a textbook law, so voices that were too quiet or
  inaudible a few meters away are heard again (`--no-rolloff-curves` brings back the old fade for
  comparison).
- The game's own **music and effects sliders** now work, and game sounds keep the console's 6 dB
  of headroom below the movies so loud scenes do not distort (`--no-xbox-headroom`).
- **Dialogue, music and effects volumes** (`--dialogue-volume`, `--music-volume`,
  `--sfx-volume`, -40 to +12 dB): on top of the game's own settings, also in the launcher.
- **Sounds pause** during loads and on the pause screen, as the game asks, and resume after.
- Less crackling: a little sound is kept queued ahead for the sound card (`--audio-lead-ms`,
  50 ms by default).

### Display

- **Cutscene black bars stay solid** (`--cinema-bars`, on by default): bloom, lamp halos, lights
  and the cinematic look no longer glow over the letterbox bars of the in-engine cutscenes.
- **FSR 1 upscaling** (`--fsr ultra|quality|balanced|performance`, `--fsr-sharpness`): for cards
  that cannot hold 60 FPS at the chosen size, the game renders smaller and AMD FidelityFX Super
  Resolution 1 upscales and sharpens it. Off in every preset but Iris Xe.
- **Per-pixel relief and highlights** (`--normal-maps`, `--normal-strength`,
  `--specular-strength`, `--normal-baked`): bumps, grooves and shine from normal and specular maps
  you add to your texture pack, lit by the world and character lighting. Opt-in; none ship.
- **The game's original brightness** (`--gamma-ramp`, on by default): the picture is as bright as
  on the Xbox, with the game's own gamma ramp and its in-game Brightness setting.
- **Light shafts** (`--light-shafts`, `--light-shafts-strength`, `--light-shafts-cheap`): street
  lamps hang a soft cone of light in the air, burning barrels a glow in their smoke, thicker in
  the rain. On in Quality.
- **Street lamp shadows** (`--lamp-shadows`, `--lamp-shadows-strength`): people near a lit lamp or
  burning barrel cast a ground shadow pointing away from it, one per person. On in Quality.
- **Refresh-rate match** (`--refresh-match`, on by default in fullscreen): a 144 or 165 Hz screen
  is switched to 120 Hz so motion stays even at 60 FPS, and switched back if no frame shows.

### Launcher

- **Iris Xe preset**: the settings measured at 60 FPS on an Intel Iris Xe laptop (FSR quality and
  most of the effects), recommended for Intel Iris Xe and Arc integrated graphics.
- The **Display page** is split into collapsible groups, with the FSR choice in the Image group
  and toggles for the original brightness, refresh-rate match, light shafts and street lamp
  shadows.
- A **Sound** card with the dialogue, music and effects sliders.
- A **black-frame warning** on Home when the game's 3D frames would be black (no usable Vulkan
  driver), and the reason in the bug report.
- The launcher no longer shows "Not responding" at startup: the graphics card check and the
  texture pack scan run in the background.

### Performance

- A level's textures are uploaded in one batch: about 0.1-0.2 s less per level entry.
- Lamp shadows only trace the lights in reach of each person.
- `--load-profile` prints where a level's load time goes.

### Tools

- `tools/glyphs/make_button_icons.py`: make your own button-icon pack (neutral icons drawn by code
  over the cells of your own copy's atlas) for the launcher's Controls page.
- `tools/normal_from_albedo.py`: stopgap relief and specular maps guessed from a texture pack, for
  `--normal-maps`, until you bake your own.
- `--audio-levels` prints the loudness of each playing sound, for volume bug reports.

### Fixes

- Textures the game rewrites while streaming are no longer cached wrong (`--no-tex-race-guard`
  for comparison), and textures it rewrites every frame are drawn instead of skipped.
- When Vulkan cannot start, the log says why the 3D frames are black.

### Known issues

See [README.md](README.md#known-issues).

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
