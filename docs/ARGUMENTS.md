# Command-line options

The game executable (`TheWarriors.exe` on Windows, `xbrun` on Linux) takes the game's
`default.xbe`, the extracted disc folder and then any number of options:

```text
TheWarriors.exe <disc>\default.xbe <disc> [options]
```

## Where to type them

- **Launcher:** Game page → **Extra arguments**. The text is added at the end of the command
  line exactly as typed (quote values that contain spaces).
- **Command line:** after `TheWarriors.exe <disc>\default.xbe <disc>`.

The launcher already passes most options from its own pages. Set those there instead of
repeating them in Extra arguments:

| Launcher page | Options it passes |
| --- | --- |
| Game | the game executable, the disc folder, `--hdd`, `--language`, `--texture-pack`, `--spray-time`, `--no-text-fixes` |
| Display | `--resolution` / `--render-scale`, `--widescreen`, `--fullscreen`, `--msaa`, `--aniso`, `--supersample`, `--sharpen`, `--bloom` (and `--bloom-strength`), `--rain-hd`, `--ssao` (and `--ssao-strength`), `--shadow-scale`, `--shadow-soft`, `--shadow-range`, `--frames-in-flight`, `--present` (vsync on/off), `--sim60` (frame rate 60), the HUD style (`--hud-hd`, `--hud-hd-glass`, `--hud-glass` and the `--hud-hd-*` values) |
| Controls | `--button-icons`, `--no-rumble`, the button-icon pack (as a `--texture-pack`) |
| Options (advanced) | `--renderer`, `--present`, `--no-render-targets`, `--audio`, `--video`, `--stats`, `--trace`, `--no-frame-lock`, `--no-rate-fixes`, `--spawn-scale`, `--warp`, `--input-config` |

Extra arguments come last. For an option that takes one value, the last one given wins, so an
extra argument overrides the launcher's setting. `--texture-pack` adds a pack instead: the
launcher's packs come first and win where they have the same texture.

Invalid values print the usage text and exit with code 2.

## Display & quality

| Option | Default | Effect |
| --- | --- | --- |
| `--resolution WxH` | 640x480 | Render the 3D scene at the largest 4:3 size inside WxH (16:9 with `--widescreen`), e.g. `1920x1080` renders 1440x1080. Also the initial window size. |
| `--render-scale N` | 1 | Render at N x 640x480 (1-8). `--resolution` wins when both are given. |
| `--widescreen` | off | Report a 16:9 TV to the game: wider field of view and HUD layout, presented as 16:9. |
| `--fullscreen` | off | Start in desktop fullscreen. F11 or Alt+Enter toggles it while playing. |
| `--msaa N` | off | Multisample antialiasing, N samples per pixel (2, 4, 8; up to what the GPU supports). |
| `--aniso N` | off | Anisotropic texture filtering up to N (clamped to the GPU limit). |
| `--supersample N` | 1 (off) | Render at N x the size (2 or 4) and filter it down. Smooths every edge and texture; combines with `--msaa`. Costs N² x the pixels. |
| `--sharpen N` | 0 (off) | Contrast-adaptive sharpening of the finished frame, 0-100. |
| `--bloom` | off | Light bleeds from street lamps, neon, fire and the game's glow sprites into the 3D scene. The HUD and text are drawn after it and do not glow, and neither do the lamps' own halo and light-cone sprites (they already are the glow and look as without bloom). `--no-bloom` turns it off again. |
| `--bloom-strength F` | 0.6 | How much glow `--bloom` adds, 0.1-3. |
| `--bloom-threshold F` | 0.75 | How bright a pixel must be to glow with `--bloom`, 0-1 (lower: more of the scene glows). Fire, neon and flares glow from about four fifths of it. |
| `--rain-hd` | off | Better rain: while it rains, the game's flat rain overlay is replaced by thin falling streaks at several distances. Walls, cars and people in front of the camera hide the streaks behind them (the see-through parts of fences and wire do not), near streaks are longer, wider, fainter and faster than far ones, the streaks catch the scene's light (brighter near lamps, faint in the dark), and the slant, speed and colour follow the game's own rain. The HUD is drawn over the rain (a see-through minimap shows it behind). Without rain nothing changes. `--no-rain-hd` turns it off again. |
| `--rain-hd-density F` | 1 | How many streaks and splashes `--rain-hd` draws, 0.25-4 (times the game's own amount, which changes as the rain grows). |
| `--rain-hd-splash` | on | With `--rain-hd`: drops landing on the ground show tiny splashes (two or three droplets thrown up for a moment) and, on flat ground, rings that widen and fade. Only on surfaces that face up (streets, car roofs, crate tops), never on walls, people or the HUD; they catch the scene's light (clear near lamps, faint in the dark), fade with distance and stay in place on the ground when the camera moves. The game's own few splashes are kept. Covered ground (under a roof) is not known, so it gets drops too. `--no-rain-hd-splash` keeps the streaks without them. |
| `--ssao` | off | Ambient occlusion: corners, wall and floor joints, the ground under cars, props and people, and the folds of clothes and faces darken a little, so the world and its characters sit together instead of looking evenly lit. Only the solid world is darkened: smoke, fire, lamp halos and light cones, `--rain-hd` streaks, the HUD and text are drawn after it and keep their colours. `--no-ssao` turns it off again. |
| `--ssao-strength F` | 0.8 | How dark `--ssao` makes occluded places, 0.1-2. |
| `--ssao-radius F` | 0.8 | How far around a point `--ssao` looks for what occludes it, in world units, 0.1-8 (larger: broader, softer shading). |
| `--shadow-scale N` | 1 | Character shadow maps at N x their size (1-4): sharper shadow edges, more GPU memory. |
| `--shadow-soft` | off | Softer character shadow edges. |
| `--shadow-range` | off | Character shadows no longer fall on floors far below the character (for example from the hideout walkway onto the floor under it). The shadow under the feet stays as it is. `--no-shadow-range` turns it off again. |
| `--shadow-range-dist F` | 3 | How far below the character, in the game's world units (a character is about 1.8 tall), a shadow still falls with `--shadow-range`, 0.5-50. It fades out between F and 1.5 x F. |
| `--no-render-targets` | off | Skip the game's own render-target passes. Removes the character shadows. |
| `--present MODE` | `auto` | How frames reach the screen: `auto` (Vulkan swapchain, mailbox when available, else fifo), `mailbox`, `fifo`, `immediate`, or `readback` (frame copied back and shown through SDL; slower, for systems where the swapchain fails). |
| `--renderer vulkan\|none` | `vulkan` | Host renderer. `none` is a CPU-only path without 3D output; used automatically when Vulkan cannot start. |
| `--video sdl\|x11\|null` | `sdl` | Window backend. `x11` is Linux only (remote displays); `null` opens no window. |

## Frame rate & timing

| Option | Default | Effect |
| --- | --- | --- |
| `--sim60` | off | Experimental 60 FPS: the world (characters, AI, cameras) updates every 1/60 s at the same game speed. `--no-sim60` turns it off again. |
| `--frames-in-flight N` | 2 | Swapchain present only: frames the GPU may still be drawing while the next is prepared (1-3). 1 waits for each frame. |
| `--no-frame-lock` | off | Let the game's timer run on real time instead of one 60 Hz step per presented frame. |
| `--no-rate-fixes` | off | Keep the HUD minigames' per-frame steps as authored for 30 fps (by default they are adjusted for 60 Hz). `--rate-fixes` turns them back on. |
| `--spray-time PCT` | 110 | Graffiti spray can duration in percent of the original, 100-200. Applied together with the rate fixes. |

## Audio

| Option | Default | Effect |
| --- | --- | --- |
| `--audio sdl\|null` | `sdl` | Audio output. `null` plays nothing (the mixer still runs). |
| `--sfx-3d-volume DB` | 0 | Experimental: extra level in dB (-40 to +12) on every positioned (3D) sound effect. Dialogue, music and movies are unchanged. |
| `--no-xact-controls` | off | Ignore the volume and fade controls the game sets on its sound cues (for comparison). |

## Input & controls

| Option | Default | Effect |
| --- | --- | --- |
| `--input-config FILE` | `warriors-input.cfg` in the current folder, if present | Keyboard and mouse key map. An unreadable file exits with code 2. |
| `--pad-mappings FILE` | `gamecontrollerdb.txt` in the program's folder and the current folder | SDL controller mappings, as the launcher's controller set-up writes them. An unreadable file is logged, not fatal. |
| `--pad-slot N=GUID[#K]` | none | The controller with that SDL GUID (32 hex digits, as in `gamecontrollerdb.txt`) is player N (1-4, game port N-1), at start and whenever it is plugged in again during play. `#K` picks the Kth of identical controllers (same GUID), counted in the order they were attached; without it any of them. Repeatable, one per player; naming a player again replaces it. Controllers without a player take the ports nobody was given, in the order they were attached, and a chosen player's free port only when no other port is left. The launcher passes one per controller that has a player set on the Controls page. |
| `--button-icons xbox\|kbm\|auto` | `xbox` | In-game button prompts: Xbox controller glyphs, key caps and mouse drawings for the key map, or following the last used device. With `kbm`, the tutorial texts that name the sticks in words name the keyboard and mouse instead: the left stick as the keyboard with its movement keys (e.g. "keyboard (WASD)", the noun alone when a direction is unbound), the right stick as the mouse, and the command tips that say to click the left stick name its key ("Click the C key"). With `auto` that choice is made once, when the game first reads such a text (at the profile load): the keyboard and mouse unless a controller is connected or was the last device used. A few sentences share their wording with tips of the other kind and keep its text (see runtime/README.md, Text fixes). Not affected by `--no-text-fixes`. |
| `--no-rumble` | off | No controller vibration. |

## Language & text

| Option | Default | Effect |
| --- | --- | --- |
| `--language CODE` | `en` | Game language for menu and HUD text: `en`, `es`, `fr`, `de`, `it`. Voices are not translated. |
| `--no-text-fixes` | off | Use the text tables as shipped (by default known mistranslations are corrected). `--text-fixes` turns the fixes back on. |

## Textures & HUD

| Option | Default | Effect |
| --- | --- | --- |
| `--texture-pack DIR` | none | Replace each texture that has a `DIR/<hash>.png` (any size). Repeatable, up to 8; the first pack that has a texture wins. The launcher's button-icon pack is passed this way. |
| `--dump-textures DIR` | off | Write each unique texture once as `DIR/<hash>.png` plus `DIR/manifest.csv`, the starting point for a texture pack. |
| `--hud-hd` | off | Restyle the target health bar (top right): thicker, outlined, gradient fill. The rage meter is unchanged. |
| `--hud-hd-height F` | 1 | Target bar height scale, 0.5-4. Turns on `--hud-hd`. |
| `--hud-hd-opacity F` | 1 | Target bar opacity, 0-1. Turns on `--hud-hd`. |
| `--hud-hd-frost N` | 0 (off) | Blur the background behind the target bar, radius 0-32. Turns on `--hud-hd`. |
| `--hud-hd-glass` | off | Glass target bar preset (recommended): transparent frosted track, translucent vivid fill, thin outline. Options after it override its values. |
| `--hud-hd-track-opacity F` | `--hud-hd-opacity` | Opacity of the bar's empty part, 0-1. Turns on the glass style. |
| `--hud-hd-fill-opacity F` | `--hud-hd-opacity` | Opacity of the bar's fill, 0-1. Turns on the glass style. |
| `--hud-hd-vivid F` | 0 | Keep a translucent fill's color strength, 0-1. Turns on the glass style. |
| `--hud-glass` | off | Alternative style: a low-opacity target bar on a frosted-glass panel. |
| `--hud-glass-blur N` | 4 | `--hud-glass` blur radius, 1-32. Turns on `--hud-glass`. |
| `--hud-glass-opacity F` | 0.45 | `--hud-glass` bar opacity, 0-1. Turns on `--hud-glass`. |
| `--hud-glass-tint AARRGGBB` | `30ffffff` | `--hud-glass` tint color and strength. Turns on `--hud-glass`. |

## Performance diagnostics

| Option | Default | Effect |
| --- | --- | --- |
| `--stats` | off | Every 60 presented frames, print the frame rate and where a frame's time goes (game, drawing, GPU wait, present), the input latency and the frame pacing. Useful in bug reports about stutter or low FPS. |
| `--trace` | off | Log every kernel call, file open and replaced call. Large output; for bug reports when asked. |
| `--log-limit N` | unlimited | With `--trace`, log only the first N calls per kernel function. (`--quiet` is accepted and is the default.) |
| `--vp-threads N` | auto | Worker threads for vertex processing: auto is CPU cores - 1 (at most 7); 0 turns them off. |

## Saves & paths

| Option | Default | Effect |
| --- | --- | --- |
| `--hdd DIR` | `hdd` in the current folder | The emulated hard disk: saves and game settings. |

## Developer & debugging

These options are **for development**. They change how the game runs, write large files, or
exit early. Most players never need them.

| Option | Default | Effect |
| --- | --- | --- |
| `--warp MISSION[:CHECKPOINT]` | off | Loading a profile starts story mission 1-23 (or a level name such as `level81`; `level95` is the hub) at the checkpoint (default 1). Once only. **Saving is disabled for the whole session.** |
| `--scene ID` | off | Play one in-engine scene alone (the game's scene test mode). Implies `--warp`, so saving is disabled. |
| `--spawn-scale N` | 1 | Experiment: multiply how many enemies the scripts keep alive (1-8, capped per spawner and by the character pool). |
| `--frames N` | unlimited | Exit after N presented frames. |
| `--dump-frame FILE.bmp` | off | On exit, write the last presented frame as a BMP. Forces the readback present path. |
| `--dump-every N` | off | With `--dump-frame`: also write `FILE.bmp.<frame>.bmp` every N frames. |
| `--audio-dump FILE.wav` | off | Write the audio mix to a WAV file (16-bit stereo 48 kHz). |
| `--audio-log N` | 0 (32 with `--trace`) | Log the first N calls per audio function plus sound bank and cue events. |
| `--input-script SPEC` | off | Scripted controller input for unattended runs: `FRAME:[pN/]CONTROLS[:FRAMES],...`. See `runtime/README.md`. |
| `--rate-probe FILE[:MS]` | off | Every MS ms (default 100) of game time, snapshot the game's data section into FILE, for `tools/ratediff`. |
| `--rate-probe-range ADDR:SIZE` | none | With `--rate-probe`: one more memory range to snapshot, or `@PTR[+OFF]:SIZE` behind a pointer. Repeatable. |
| `--poke ADDR:HEX[/EXPECT]` | none | Write bytes into game memory before it starts; with `/EXPECT` only when the original bytes match (otherwise exit 2). Repeatable. |
| `--watchdog SEC` | off | After SEC seconds, print every game thread's position and exit with code 4. |
| `--win-thread-probe` | off | Print thread diagnostics at startup phases. |
| `--symbols FILE` | none | Symbol file (`LIB__Name = 0xADDR` lines): names in crash reports and for `--hook`/`--skip`. |
| `--hook SYMBOL\|0xADDR` | see below | Success hook: report and exit when the game reaches it. Repeatable. |
| `--skip SYMBOL:POPBYTES[:RET]` | none | Patch a game function to return RET immediately. Needs `--symbols`. Repeatable. |
| `--no-hle-d3d` | off | Do not replace the graphics entry points; stop at the hook instead (default hook `D3D8__Direct3D_CreateDevice`, needs `--symbols`). |
| `--no-hle-audio` | off | Do not replace the audio entry points (the game then faults). |
| `--no-hle-input` | off | Do not replace the input entry points (the game sees no controller). |
| `--no-default-skips` | off | With `--no-hle-input`, do not skip the USB start-up function. |
| `--fake-mmio` | off | Experimental: back some console hardware registers with memory. |
| `--patch-fs` | off (always on Windows) | Rewrite the game's thread-block accesses as the Windows build does (Linux: for testing). |

More detail on each option is in [`runtime/README.md`](../runtime/README.md).
