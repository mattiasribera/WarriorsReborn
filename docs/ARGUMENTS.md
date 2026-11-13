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
| Game | the game executable, the disc folder, `--hdd`, `--language`, `--texture-pack`, `--sound-pack`, `--spray-time`, `--no-text-fixes`, `--skip-intro` (Skip intro videos, on in every preset), `--mesh-pack` (the `characters-hd` folder, when it holds models) |
| Display | `--resolution` / `--render-scale`, `--widescreen`, `--fullscreen` (and `--no-refresh-match`), `--msaa`, `--aniso`, `--supersample`, `--fsr` (FSR 1 upscaling, off in every preset), `--sharpen`, `--bloom` (and `--bloom-strength`), `--rain-hd` (and `--rain-hd-puddles`), `--ssao` (and `--ssao-strength`), `--char-lighting`, `--world-lighting`, `--fire-light`, `--lamp-shadows`, `--light-shafts` (and `--light-shafts-cheap`), `--contact-shadows`, `--lamp-lights` (and `--lamp-lights-strength`), `--neon-glow` (and `--neon-glow-strength`, plus the `neon-masks` folder as a mask pack), `--normal-maps` (and `--normal-strength`, `--specular-strength`, `--normal-baked`; opt-in, off in every preset), `--filmic`, `--sky-hd`, `--sharp-faces` (and `--sharp-faces-over-pack`, shown only with a texture pack; off in every preset), `--draw-distance` and `--prop-detail`, `--fx-scale`, `--no-movie-hd`, `--no-gamma-ramp` (Game's original brightness, on in every preset), `--soft-particles` (opt-in, off in every preset), `--fps-overlay` (On-screen counter, off in every preset; with `--fps-overlay-items` and `--fps-overlay-corner`), `--no-sharp-fonts` (only when Sharp text is off; on in every preset), `--shadow-scale`, `--shadow-soft`, `--shadow-range`, `--frames-in-flight`, `--present` (vsync on/off), `--sim60` (frame rate 60), the HUD style (`--hud-hd`, `--hud-hd-glass`, `--hud-glass` and the `--hud-hd-*` values) |
| Controls | `--button-icons`, `--no-rumble`, `--pad-slot`, the button-icon pack (as a `--texture-pack`) |
| Options (advanced) | `--renderer`, `--present`, `--no-render-targets`, `--audio`, `--video`, `--stats`, `--load-profile`, `--trace`, `--no-gpu-skinning`, `--no-gpu-vertex-programs`, `--no-direct-geometry`, `--no-frame-lock`, `--no-rate-fixes`, `--spawn-scale`, `--warp`, `--input-config` |

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
| `--refresh-match` | on | In fullscreen, a screen whose refresh rate is not a multiple of 60 Hz (144, 165 Hz) is switched to its highest multiple of 60 at the same resolution (144 -> 120 Hz) so motion stays even. The switch happens once the first frame is on screen; if the game then shows no frame within 3 s (or gets minimized on its own) it goes back to desktop fullscreen for the session. The screen goes back at exit and while the game is minimized. `--no-refresh-match` keeps the screen's rate. |
| `--msaa N` | off | Multisample antialiasing, N samples per pixel (2, 4, 8; up to what the GPU supports). |
| `--aniso N` | off | Anisotropic texture filtering up to N (clamped to the GPU limit). |
| `--supersample N` | 1 (off) | Render at N x the size (2 or 4) and filter it down. Smooths every edge and texture; combines with `--msaa`. Costs N² x the pixels. |
| `--sharpen N` | 0 (off) | Contrast-adaptive sharpening of the finished frame, 0-100. |
| `--fsr MODE` | `off` | AMD FidelityFX Super Resolution 1, for GPUs that cannot hold 60 fps at the chosen size: the game renders below the output size (`--resolution` / `--render-scale`) and FSR upscales (EASU) and sharpens (RCAS) the frame to it. `ultra` renders at 1/1.3 of the size per axis, `quality` 1/1.5, `balanced` 1/1.7, `performance` 1/2 (e.g. `--resolution 1920x1080 --fsr quality` renders 960x720 and shows 1440x1080). Replaces `--supersample` and `--sharpen`: both are ignored, with a warning. The HUD and text are upscaled with the scene; `--fps-overlay`, the game's brightness ramp and `--dump-frame` are at the output size. |
| `--fsr-sharpness F` | 0.2 | RCAS sharpening of `--fsr`, in stops: 0 is the sharpest, each stop halves it, up to 2. |
| `--fps-overlay` | off | A small frame counter in a corner, for example `60 FPS  16.6 ms`: presented frames per second and the mean frame time, averaged over half a second. Drawn last, over the finished image (after bloom, the cinematic look and sharpening), at the window's own pixels. Almost free. `--no-fps-overlay` turns it off again. |
| `--fps-overlay-corner C` | `tl` | Corner of `--fps-overlay`: `tl` (top left), `tr`, `bl` or `br`. |
| `--fps-overlay-items LIST` | `fps,ms` | What `--fps-overlay` shows: any of `fps`, `ms`, `cpu`, `gpu`, comma separated, always shown in that order, for example `60 FPS  16.6 ms  CPU 34%  GPU 71%`. `cpu`: this game process's CPU use over the same half second, where 100% means every logical core of the machine is busy (so one fully busy thread on a 12-thread CPU reads 8%). `gpu`: the game's own GPU work per frame (Vulkan timestamps around each frame's commands) over the frame period; it is not the whole GPU's utilization (other programs, the compositor and the copy to the window are not counted), and it reads `GPU --` when the GPU has no timestamps. It only chooses the items: `--fps-overlay` turns the counter on. |
| `--bloom` | off | Light bleeds from street lamps, neon, fire and the game's glow sprites into the 3D scene. The HUD and text are drawn after it and do not glow, and neither do the lamps' own halo and light-cone sprites (they already are the glow and look as without bloom). `--no-bloom` turns it off again. |
| `--bloom-strength F` | 0.6 | How much glow `--bloom` adds, 0.1-3. |
| `--bloom-threshold F` | 0.75 | How bright a pixel must be to glow with `--bloom`, 0-1 (lower: more of the scene glows). Fire, neon and flares glow from about four fifths of it. |
| `--rain-hd` | off | Better rain: while it rains, the game's flat rain overlay is replaced by thin falling streaks at several distances. Walls, cars and people in front of the camera hide the streaks behind them (the see-through parts of fences and wire do not), near streaks are longer, wider, fainter and faster than far ones, the streaks catch the scene's light (brighter near lamps, faint in the dark), and the slant, speed and colour follow the game's own rain. The HUD is drawn over the rain (a see-through minimap shows it behind). When the game starts or stops its rain (for example when the player walks into or out of a building), the streaks, splashes and wet ground fade in or out over half a second of game time instead of appearing or vanishing at once. Without rain nothing changes. `--no-rain-hd` turns it off again. |
| `--rain-hd-density F` | 1 | How many streaks and splashes `--rain-hd` draws, 0.25-4 (times the game's own amount, which changes as the rain grows). |
| `--rain-hd-splash` | on | With `--rain-hd`: drops landing on the ground show tiny splashes (two or three droplets thrown up for a moment) and, on flat ground, rings that widen and fade. Only on surfaces that face up (streets, car roofs, crate tops), never on walls, people or the HUD; they catch the scene's light (clear near lamps, faint in the dark), fade with distance and stay in place on the ground when the camera moves. The game's own few splashes are kept. Ground under a roof gets none (see `--rain-hd-cover`). `--no-rain-hd-splash` keeps the streaks without them. |
| `--rain-hd-wet` | on | With `--rain-hd`, while it rains: the ground looks wet. Surfaces that face up (streets, pavements, car roofs) get a little darker and glossy and reflect the bright parts of the scene (street lamps and their halos, lit windows, signs, car lights) as soft, vertically smeared reflections, stronger at grazing angles, fading towards the screen edges. Only what is on screen can be reflected (screen-space reflections), so reflections fade out where their source leaves the screen. Walls, people and the HUD are not changed, and the ground right next to people is as wet as the rest. `--no-rain-hd-wet` keeps the ground dry. |
| `--rain-hd-cover` | on | With `--rain-hd`: places under a roof stay dry. The solid world around the camera (about 48 x 48 units) is also seen from straight above each rain frame, and wherever something lies over a point (a roof, an awning, a bridge) the streaks, splashes and wet ground are left out there, with soft edges so drips at a roof's edge do not look cut. So the inside of a building seen through an open door has no rain. People do not count as cover (the street under and around them is as wet as elsewhere); moving props still do. `--no-rain-hd-cover` lets the rain fall everywhere, as before. |
| `--rain-hd-puddles` | off | With `--rain-hd` and its wet ground: puddles form in low spots of the open street (patches fixed in the world, more where the ground lies below its surroundings, such as along kerbs). They are darker than the wet asphalt around them, reflect the whole scene rather than only its lights, and ripple where drops land. Never under a roof, on walls, on people or over the HUD. On in the launcher's Balanced and Quality presets (about 0.05 ms at 1080p). `--no-rain-hd-puddles` turns them off again. |
| `--rain-hd-wet-strength F` | 1.8 | With `--rain-hd-wet`: scales the reflections and the darkening of wet ground, 0.1-4 (2 = twice as bright reflections and darker ground; the darkening stops at 60%). Other values are rejected (exit code 2). |
| `--ssao` | off | Ambient occlusion: corners, wall and floor joints, the ground under cars, props and people, and the folds of clothes and faces darken a little, so the world and its characters sit together instead of looking evenly lit. Only the solid world is darkened: smoke, fire, lamp halos and light cones, `--rain-hd` streaks, the HUD and text are drawn after it and keep their colours. `--no-ssao` turns it off again. |
| `--contact-shadows` | off | Contact shadows: the ground right under and around people, cars and props gets a short, soft shadow from above, so they stand on it instead of floating. Only the ground and the tops of things darken; people themselves, effects, lamp glows, rain and the HUD never do. Works with or without `--ssao`. On in the launcher's Quality preset (0.3-0.7 ms at 1080p on an RTX 2060). `--no-contact-shadows` turns it off again. |
| `--contact-shadows-strength F` | 0.6 | How dark `--contact-shadows` gets, 0.1-2 (at most 90% darker). Other values are rejected (exit code 2). |
| `--ssao-strength F` | 0.8 | How dark `--ssao` makes occluded places, 0.1-2. |
| `--ssao-radius F` | 0.8 | How far around a point `--ssao` looks for what occludes it, in world units, 0.1-8 (larger: broader, softer shading). |
| `--char-lighting` | off | Character lighting: the lamp lights (the lights of the title's lamp halos, fire and neon, as for `--lamp-lights`) fall on people per pixel, following the shape of their bodies and faces (smooth normals from each character's own mesh, a soft wrap and the lamp falloff), instead of the flatter light `--lamp-lights` alone gives them. Only character draws of the 3D scene are lit: the world, effects, the HUD and text never are, and a frame without lamps looks as without it. Works without `--lamp-lights` too (then only people catch the lamps' light). On in the launcher's Quality and Balanced presets, off in Performance (about 0.03-0.16 ms at 1080p on an RTX 2060). `--no-char-lighting` turns it off again. |
| `--world-lighting` | off | Per-pixel scene lighting: the lamp lights (the lights of the title's lamp halos, fire and neon, as for `--lamp-lights`) fall on walls, floors, buildings and props per pixel, following their own surface normals (N.L and the lamp falloff), added to the game's baked light before texturing, so the night mood, the baked shadows and the fog stay. Only the world's and props' draws of the 3D scene are lit (the game's world and object vertex programs); people (see `--char-lighting`), effects, the HUD and text are not, and a frame without lamps looks as without it. Lights are not blocked by walls: a lamp near a corner also lights the wall's far side. Works without `--lamp-lights` too; with it, the lamp pass then lights only the characters (or nothing, with `--char-lighting`). On in the launcher's Quality preset only (it replaces the lamp pass's world light, at about the same GPU cost; recording the world's normals costs about 0.2 ms of CPU per 30000 world vertices). `--no-world-lighting` turns it off again. |
| `--world-lighting-strength F` | 1 | How bright `--world-lighting` is, 0.1-3. Other values are rejected (exit code 2). |
| `--fire-light` | off | Fire light: the burning fire barrels cast a warm (about 2000 K), gently flickering light on the walls, floor and people near them (a few metres). The game draws its fires as flame sprites; their quads become fire lights (the nearest 8 to the camera), whose brightness, reach and position flicker smoothly with the game clock (the same at 30 and 60 fps, still while paused). They join the lamp lights of `--world-lighting` (walls, floors, props), `--char-lighting` (people) and `--lamp-lights`; without one of those it does nothing. With it, a fire is no longer also a steady white lamp light. On in the launcher's Quality preset only. `--no-fire-light` turns it off again. |
| `--fire-light-strength F` | 1 | How bright `--fire-light` is, 0.1-3. Other values are rejected (exit code 2). |
| `--lamp-shadows` | off | Shadows from street lamps: people near a lit street lamp or burning barrel cast a shadow on the ground pointing away from that light, fading with the distance from them, on top of the game's own shadow (which always falls from one light above). Uses the lamps of `--lamp-lights` and the fires of `--fire-light` (works without them too); the 8 lights nearest the camera count, and each person casts one lamp shadow, from the lamp that lights them most; walking from one lamp to the next crossfades the shadow from one to the other. On in the launcher's Quality preset only. `--no-lamp-shadows` turns it off again. |
| `--lamp-shadows-strength F` | 0.6 | How dark the `--lamp-shadows` shadows are where one nearby light is fully blocked, 0.1-1. Other values are rejected (exit code 2). |
| `--soft-particles` | off | Opt-in (the launcher's Display page; no preset turns it on). Soft particles: smoke, fire, dust and lamp haze fade out where they meet the ground, walls and people instead of ending in a hard straight line. Only particles that face the camera are softened; ground decals (blood, scorch marks, rain ripples), quads fixed on walls such as graffiti tags, the glow and overlay sprites drawn on top of everything and the HUD are unchanged. `--no-soft-particles` turns it off again. |
| `--soft-particles-distance F` | 0.5 | Over how many world units in front of a surface `--soft-particles` fade a particle out, 0.05-4 (larger: softer, but small puffs near the ground fade more). |
| `--draw-distance N` | 1 | The far plane and the fog reach up to N times as far as the game's, 1-4 (1 = as shipped). Cutscene cameras keep their own distance, and the view still only reaches as far as the world has streamed in. The launcher's Display page offers Original, Far, Farther and Maximum (1, 1.5, 2, 3). |
| `--prop-detail N` | 1 | Small props such as bins, crates and debris stay visible up to N times as far, 1-8 (1 = as shipped). |
| `--fog-breathing` | off | Keep the game's habit of pulling the fog in for a moment after a slow frame. By default the fog stays steady. |
| `--fx-scale N` | 1 | Effect pools, 1-4 with decimals (1 = as shipped): more blood, debris, smoke, glass shards and sparks can be on screen at once, and ground blood lasts through long fights. Implies `--big-heap`. The launcher's Display page offers Normal, More and Maximum (1, 2, 4). |
| `--big-heap` | off | Give the game's main memory heap more room (77.5 MB instead of 45.4 MB), so larger effect pools do not take memory from the streamed world. Implied by `--fx-scale` above 1. |
| `--movie-hd` | on | Play the disc's 1280x720 movies instead of the 640x480 ones, drawn at the output resolution and fitted to 16:9 (letterboxed at 4:3). `--no-movie-hd` plays the original movies. |
| `--gamma-ramp` | on | Show the picture as bright as on the Xbox: the game's own gamma ramp (the console applies it on the TV output: levels a little brighter, menus a little darker) and the in-game Brightness setting. Applied last, after every other picture option, to the 3D scene, HUD, menus and movies; the on-screen counter is not affected. `--no-gamma-ramp` shows the frames as rendered (darker levels, the Brightness setting does nothing), as before this option. |
| `--cinema-bars` | on | Keep the black letterbox bars of the in-engine cutscenes solid black. The game draws them as part of the 3D scene, so bloom, lamp halos and the cinematic look (`--filmic`) used to glow over them and lights showed through the bars. They are now drawn after the scene and its effects, before the subtitles. With `--widescreen` the game's bars are kept as they are: on the 16:9 picture they give a 2.34:1 scope look and never cover the HUD or subtitles. `--no-cinema-bars` draws them where the game does. Costs nothing measurable. |
| `--skip-intro` | off | Start at the title screen (PRESS START) without the three boot movies: the logos (`logo`, `plogo`) and the opening movie (`l1_in`). The title is ready in about a second instead of about 54 (some 3200 frames). Only these three plays at start-up are left out: the story levels' movies (the first mission opens with `l99_in`) and the title screen's demo, which after about a minute idle plays the opening movie, play as before. On in every launcher preset ("Skip intro videos" on the Game page). `--no-skip-intro` turns it off again. |
| `--filmic` | off | Cinematic look: the 3D scene is graded like the 1979 film, with warm highlights and a red-orange glow (the glow needs `--bloom`), teal shadows, deep blacks and a light vignette. The HUD, text, menus and movies are unchanged. On in the launcher's Balanced and Quality presets. `--no-filmic` turns it off again. |
| `--filmic-strength F` | 1 | How strong `--filmic` is, 0-2 (0 = the scene unchanged). |
| `--filmic-grain F` | 0 (off) | Film grain with `--filmic`, 0-1. |
| `--filmic-vignette F` | 0.25 | How much `--filmic` darkens the corners, 0-1 (0.25: about 6% darker). |
| `--filmic-lut FILE.cube` | built-in grade | Use a `.cube` 3D LUT (2-64 per side) as the `--filmic` grade. A file that cannot be read is logged and the built-in grade is kept. |
| `--sky-hd` | off | Enhanced sky: the night sky is redrawn sharp at any resolution in each level's own colours, with stars, drifting clouds (overcast while it rains), a moon and lit windows on the distant skyline. On in the launcher's Balanced and Quality presets. `--no-sky-hd` turns it off again. |
| `--lamp-lights` | off | Street lamps, fires and neon light up the ground, walls and people around them (the game itself has no moving lights in its world). Lights fade in and out smoothly. On in the launcher's Quality preset. `--no-lamp-lights` turns it off again. |
| `--lamp-lights-strength F` | 1 | How bright `--lamp-lights` are, 0.1-3. |
| `--light-shafts` | off | Light shafts: each street lamp hangs a soft cone of scattered light in the air below it, and with `--fire-light` each burning barrel a glow rising in its smoke. The cones are thicker while the game's rain falls (eased in over about a second). The lights are those of `--lamp-lights` (the game's lamp halos, tracked across frames; it does not need `--lamp-lights` on). Each view ray is followed only up to the visible surface, so walls, people and the ground in front of a cone hide it; screen-fixed dithering keeps it free of banding and steady when the camera moves. A pass at half the output size (measured about +0.6 ms at 1280x720 with MSAA 2 on an RTX 2060, so roughly 0.5-1 ms at 1080p) added before the effects, halos, rain and HUD. On in the launcher's Quality preset only. `--no-light-shafts` turns it off again. |
| `--light-shafts-strength F` | 1 | How thick `--light-shafts` are, 0.1-3. Other values are rejected (exit code 2). |
| `--light-shafts-cheap` | off | A coarser `--light-shafts` pass: a quarter of the output size and half the steps per ray (roughly an eighth of the scattering cost, about 0.1-0.3 ms at 1080p estimated; meant for integrated graphics). `--no-light-shafts-cheap` goes back to the full pass. |
| `--neon-glow` | off | Lit and neon signs glow at night: textures with an emission mask (`<hash>.emit.png` / `.emit.dds` in a texture pack, see [TEXTURE_PACKS.md](TEXTURE_PACKS.md#7-emission-masks-neon-glow)) keep their lit tubes, bulbs and panels at their own colour through fog and darkness and feed the bloom. Without masks nothing glows. On in the launcher's Balanced and Quality presets (with the `neon-masks` folder next to the game as a mask pack). `--no-neon-glow` turns it off again. |
| `--neon-glow-strength F` | 1.25 | How bright the lit parts of the signs get, 0.1-3. |
| `--neon-glow-bloom F` | 1 | How much the lit parts feed the bloom (`--bloom`), 0-1. |
| `--neon-glow-flicker F` | 0.04 | A gentle per-sign brightness wobble, 0-0.3 (0 = steady). |
| `--normal-maps` | off | Per-pixel relief and highlights from companion maps in a texture pack: `<hash>_n.png` / `.dds` (tangent-space normal map, **OpenGL convention**: green = up the image, as Blender and Substance export by default; a DirectX map needs its green channel inverted; flat is 128, 128, 255) and `<hash>_s.png` / `.dds` (specular: **R = intensity**, **G = gloss** from broad and dull to tight and sharp; B and alpha unused), named after the texture's hash like its replacement, next to it or in a pack of their own (pass that folder as one more `--texture-pack`). `.dds` maps are BC1, BC3 or BC7 (BC7 suits normal maps) with a full mip chain. The relief is shaded by the per-pixel lights of `--char-lighting` (people) and `--world-lighting` (walls, floors, props), which carry the lamps of `--lamp-lights` and the fires of `--fire-light`; the screen-space `--lamp-lights` pass itself, and draws that are not lit per pixel, do not use the maps. A texture without maps looks exactly as before. Logs `[normal-maps] N textures with relief, M with specular` once the packs are scanned. `tools/normal_from_albedo.py` makes stopgap maps from a PNG pack. `--no-normal-maps` turns it off again. |
| `--normal-strength F` | 1 | How strongly the normal maps tilt the surface, 0-4 (0 = flat, 1 = as authored). |
| `--specular-strength F` | 1 | How bright the highlights of the specular maps are, 0-4 (0 = none). |
| `--normal-baked F` | 1 | How much the relief shades the game's own (baked, directionless) light, 0-1: grooves and slopes darken by up to (1 + cos) / 2 of the tilt, flat parts keep their colour. 0 leaves the baked light untouched. |
| `--sharp-faces` | off | Character skins (faces and clothes) at twice the detail, upscaled from your own game's textures on a background thread the first time each one is drawn. A texture pack's version of a skin wins. On in the launcher's Balanced and Quality presets. `--no-sharp-faces` turns it off again. |
| `--sharp-faces-over-pack` | off | With `--sharp-faces` and `--texture-pack`, character skins are upscaled even when the pack replaces them, so faces stay sharp while every other texture (and the same texture drawn on the world) keeps the pack's version. Built-in replacements still win. The launcher offers it only when Sharp faces is on and a texture pack is chosen; off in every preset. `--no-sharp-faces-over-pack` turns it off again. |
| `--shadow-scale N` | 1 | Character shadow maps at N x their size (1-4): sharper shadow edges, more GPU memory. |
| `--shadow-soft` | off | Softer character shadow edges. |
| `--shadow-range` | off | Character shadows no longer fall on floors far below the character (for example from the hideout walkway onto the floor under it). The shadow under the feet stays as it is. `--no-shadow-range` turns it off again. |
| `--shadow-range-dist F` | 3 | How far below the character, in the game's world units (a character is about 1.8 tall), a shadow still falls with `--shadow-range`, 0.5-50. It fades out between F and 1.5 x F. |
| `--no-render-targets` | off | Skip the game's own render-target passes. Removes the character shadows. |
| `--present MODE` | `auto` | How frames reach the screen: `auto` (Vulkan swapchain, mailbox when available, else fifo), `mailbox`, `fifo`, `immediate`, or `readback` (frame copied back and shown through SDL; slower, for systems where the swapchain fails). |
| `--renderer vulkan\|none` | `vulkan` | Host renderer. `none` is a CPU-only path without 3D output (only the movies show; menus and game are black). It is also used when Vulkan cannot start: then the log says `[renderer] ERROR: 3D frames will be black: <reason>` and on Windows a message box asks to update the graphics card driver. |
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
| `--audio-lead-ms N` | 50 | How much sound (0 to 500 ms) is kept queued ahead for the sound card. Raise it if the sound crackles; lower it for less delay. `0` is the earlier behavior, which crackles on most systems. |
| `--resampler sinc\|linear` | `sinc` | How game sounds are converted to the 48 kHz output: `sinc` is a high-quality resampler without the harsh, grainy edge on high sounds; `linear` is the earlier plain interpolation, for comparison. |
| `--reverb` | on | The room reverb the game describes for each place, rebuilt on the PC; positioned (3D) sounds send to it. Music, cutscene dialogue and movies stay dry. `--no-reverb` turns it off. |
| `--doppler` | on | Doppler pitch on positioned (3D) sounds: a source moving toward you sounds higher, one moving away lower. `--no-doppler` turns it off. |
| `--rear-cue` | on | Positioned (3D) sounds behind you are slightly muffled, so you can tell them from sounds in front (stereo panning alone cannot). `--no-rear-cue` turns it off. |
| `--sound-pack DIR` | none | Play `DIR/<sound name>.wav` instead of that game sound, e.g. `DIR/vags/character/...wav` with the paths `tools/audio_extract` writes. WAV only (PCM 8/16/24/32-bit or 32-bit float, mono or stereo, any rate); FLAC is not supported. Needs the disc folder's `XBoxWad.idx` (the sound names). See the runtime README, "Sound packs". |
| `--dialogue-volume DB` | 0 | Louder or quieter voices: extra level in dB (-40 to +12) on the game's spoken lines (the crew, enemies, passers-by and the story lines, also in the cutscenes made with the game's engine). The movies keep their own sound, dialogue in them included. |
| `--music-volume DB` | 0 | Extra level in dB (-40 to +12) on the game's music, on top of its own music setting. |
| `--sfx-volume DB` | 0 | Extra level in dB (-40 to +12) on every other game sound (hits, steps, cars, ambience), on top of its own effects setting. |
| `--sfx-3d-volume DB` | 0 | Experimental: extra level in dB (-40 to +12) on every positioned (3D) sound effect. Dialogue, music and movies are unchanged. |
| `--no-rolloff-curves` | off | Fade positioned (3D) sounds with distance by the plain textbook law instead of the game's own fade-out curves, as earlier builds did (much quieter voices a few meters away). For comparison. |
| `--no-xbox-headroom` | off | Play game sounds 6 dB louder than the Xbox did (the console keeps them 6 dB below movies so loud scenes do not distort). For comparison. |
| `--no-xact-controls` | off | Ignore the volume and fade controls the game sets on its sound cues (for comparison). |

## Input & controls

| Option | Default | Effect |
| --- | --- | --- |
| `--input-config FILE` | `warriors-input.cfg` in the current folder, if present | Keyboard and mouse key map. An unreadable file exits with code 2. |
| `--pad-mappings FILE` | `gamecontrollerdb.txt` in the program's folder and the current folder | SDL controller mappings, as the launcher's controller set-up writes them. An unreadable file is logged, not fatal. |
| `--pad-slot N=GUID[#K]` | none | The controller with that SDL GUID (32 hex digits, as in `gamecontrollerdb.txt`) is player N (1-4, game port N-1), at start and whenever it is plugged in again during play. `#K` picks the Kth of identical controllers (same GUID), counted in the order they were attached; without it any of them. Repeatable, one per player; naming a player again replaces it. Controllers without a player take the ports nobody was given, in the order they were attached, and a chosen player's free port only when no other port is left. The launcher passes one per controller that has a player set on the Controls page. |
| `--button-icons xbox\|kbm\|auto` | `xbox` | In-game button prompts: Xbox controller glyphs, key caps and mouse drawings for the key map, or following the last used device. With `kbm`, the tutorial texts that name the sticks in words name the keyboard and mouse instead: the left stick as the keyboard with its movement keys (e.g. "keyboard (WASD)", the noun alone when a direction is unbound), the right stick as the mouse, and the command tips that say to click the left stick name its key ("Click the C key"). With `auto` that choice is made once, when the game first reads such a text (at the profile load): the keyboard and mouse unless a controller is connected or was the last device used. A few sentences share their wording with tips of the other kind and keep its text (see runtime/README.md, Text fixes). Not affected by `--no-text-fixes`. |
| `--sharp-fonts`, `--no-sharp-fonts` | on | Sharp text: the menu, HUD, mission and subtitle letters are rebuilt at 4x from the game's own font atlases the first time they show (a background thread; the old letters show for a moment), so they stay crisp at 1080p and above. Button icons and other art keep their look; a texture pack that replaces a font atlas wins. `--no-sharp-fonts` shows the letters as shipped. |
| `--no-rumble` | off | No controller vibration. |

## Language & text

| Option | Default | Effect |
| --- | --- | --- |
| `--language CODE` | `en` | Game language for menu and HUD text: `en`, `es`, `fr`, `de`, `it`. Voices are not translated. |
| `--no-text-fixes` | off | Use the text tables as shipped (by default known mistranslations are corrected). `--text-fixes` turns the fixes back on. |

## Textures & HUD

| Option | Default | Effect |
| --- | --- | --- |
| `--texture-pack DIR` | none | Replace each texture that has a `DIR/<hash>.png` (any size). Repeatable, up to 8; the first pack that has a texture wins. The launcher's button-icon pack is passed this way. See [TEXTURE_PACKS.md](TEXTURE_PACKS.md). |
| `--texpack-sync` | off | Debugging: read and decode texture pack files (and neon masks) on the drawing thread when first drawn, as older builds did, instead of on the loader threads (which draw the game's own texture until the replacement is uploaded, within a per-frame upload budget). See [TEXTURE_PACKS.md](TEXTURE_PACKS.md). |
| `--no-texpack-prefetch` | prefetch on | The texture pack loaders read only textures being drawn, not the ones a level load registers ahead of time. |
| `--no-tex-race-guard` | guard on | Diagnostic: the game may reuse a texture's memory (streaming) after a draw was recorded and before it is drawn. By default such a draw is skipped for that one frame (a texture the game rewrites every frame is drawn from its current bytes on the following frames) and nothing is cached under the old key, so the old bytes never stick to the texture (one cause of lasting black or gray streaks); this option caches them as older builds did. Either way each case is logged as `[tex-race] ...` (at most 8 lines per 600 frames) with a total at exit. |
| `--texpack-budget MB` | 1024 | Maximum video memory (MiB, 32 to 4096) for the texture cache with a texture pack. When the driver reports a memory budget (`VK_EXT_memory_budget`) the cache is capped lower to fit what the rest of the game leaves free; see [TEXTURE_PACKS.md](TEXTURE_PACKS.md). |
| `--mesh-pack DIR` | none | Experimental: replace each skinned character mesh that has a `DIR/<hash>.wmesh` (a smoothed mesh written by `tools/mesh_pack`, same skeleton and weights, so every animation still works). Off by default.  The launcher adds `--mesh-pack characters-hd` by itself when the `characters-hd` folder next to it holds `.wmesh` files (unless you already typed a `--mesh-pack`). |
| `--dump-textures DIR` | off | Write each unique texture once as `DIR/<hash>.png` plus `DIR/manifest.csv`, the starting point for a texture pack. See [TEXTURE_PACKS.md](TEXTURE_PACKS.md). |
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
| `--load-profile` | off | For each level entered, print one `[load]` line: the time from the level load request to the level's first frame, split into the game's own frame time, disc reads (archives, audio, movies), texture creation, shader pipeline builds and texture pack reads. Useful in reports about slow loading. |
| `--trace` | off | Log every kernel call, file open and replaced call. Large output; for bug reports when asked. |
| `--log-limit N` | unlimited | With `--trace`, log only the first N calls per kernel function. (`--quiet` is accepted and is the default.) |
| `--vp-threads N` | auto | Worker threads for vertex processing: auto is CPU cores - 1 (at most 7); 0 turns them off. |
| `--gpu-skinning`, `--no-gpu-skinning` | on | Character skinning on the GPU: the game's character vertex programs (bones, lighting, fog) run in a compute pass on the graphics card instead of on the CPU, with the same picture, which saves several ms of CPU per frame in crowded fights. Other vertex programs, and graphics cards without compute support, stay on the CPU. With `--char-lighting` the characters' smooth normals are built on the GPU too. `--no-gpu-skinning` runs everything on the CPU as before. |
| `--gpu-vertex-programs`, `--no-gpu-vertex-programs` | on | With GPU skinning on: the game's other vertex programs (the streets, buildings and props) run on the graphics card too instead of on the CPU. This is the largest CPU saving: about 9 ms of CPU per frame in the hideout and 3 ms in fights on the test PC, while the graphics card works about 1.5 ms more per frame. The picture is the same except for a few isolated pixels on polygon edges, as with GPU skinning. `--no-gpu-vertex-programs` keeps them on the CPU (characters stay on the GPU). `--world-lighting` lights them the same either way. |
| `--direct-geometry`, `--no-direct-geometry` | on | The renderer writes each frame's vertices straight into the graphics card's upload buffer instead of a separate copy that is uploaded at the end of the frame. Same picture, about 1-2 ms less CPU per frame in busy scenes. `--no-direct-geometry` goes back to the copy. |
| `--no-pipeline-cache` | cache on | By default the compiled shader pipelines are kept in `pipeline-cache.bin` and `pipeline-keys.bin` in the `--hdd` folder (beside the saves, never seen by the game; safe to delete). On the next start the pipelines used last time are built again on a low-priority background thread, so places already visited do not stutter while their effects are compiled. The cache is dropped automatically when the graphics card or driver changes, or when the file is damaged. `--no-pipeline-cache` compiles every pipeline when it is first needed, as before, and writes no files. With `--stats`, a `[pipeline]` line every 60 s counts the pipelines still compiled while playing and the longest renderer frame. |

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
| `--audio-levels` | off | Print one `[levels]` line per second of sound: the loudness (RMS dBFS) of the whole mix, of the reverb and of each game sound playing (by its cue number in hex; `100000` is movie audio). For bug reports about volume. |
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
