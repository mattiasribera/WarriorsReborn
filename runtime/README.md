# xbrun: runtime proof of concept

Loads the original Xbox `default.xbe` into a 32-bit Linux (or Windows, see below) process at its native
addresses, replaces the Xbox kernel (`xboxkrnl.exe`) imports with host
implementations, and runs the game's own x86 code. The Direct3D 8 API boundary is
emulated on the host (HLE): the title creates its device, clears and swaps, and
the frames appear in an SDL2 window. The Bink intro movies are drawn by a CPU stand-in
for their YUV pixel shader; the 3D draws are rendered by a Vulkan renderer with the title's own
vertex programs (CPU-interpreted), pixel-shader register combiners over up to four textures, alpha
blending, depth and cull state, fixed-function lighting for FVF draws (SetLight/SetMaterial and the
D3D lighting render states), per-vertex fog and the `DrawVerticesUP` screen quads. Audio: Bink movie
sound, XACT music, voices and effects through a host mixer (SDL2 device and/or a WAV dump). Input: up to four
gamepads (ports 0-3); port 0 is driven by the keyboard and mouse, the first SDL game controller or a
frame-based script, ports 1-3 by further SDL game controllers (local 2-player).
With `--no-hle-d3d` it runs until a hooked Direct3D entry point is reached.

No game data lives in this directory. The XBE, the extracted disc and the symbol
file are inputs; `build/` (binary, symbols, logs, emulated HDD) is gitignored.

## Build

Requires `zig` (tested 0.17.0) and the host's 32-bit glibc (`libc6:i386`,
`/lib/ld-linux.so.2`). Everything is a dynamically linked i386 glibc binary
(`x86-linux-gnu.2.34`) so the runtime can `dlopen` the host i386 graphics
libraries; no i386 dev packages are needed to build. The renderer also needs the Vulkan
headers (`VULKAN_INCLUDE`, default the Homebrew `vulkan-headers` include directory) and
`glslangValidator` (`GLSLANG`, Homebrew `glslang`) on the build host; the Windows target uses
the same two (cross-compiled from Linux, nothing to install on Windows). The runtime is non-PIE and
linked at `0x60000000` so the XBE range (`0x10000`..`0x5d5000`) stays free.
`make clean` also removes `build/default.syms`; rerun `make symbols`.

Observed address space (default run, ASLR on): XBE `0x10000`..`0x5d5000`,
guest arena from `0x10000000`, `xbrun` at `0x60000000`, the brk heap a few MB
above it (randomized, inside the arena range but far above where guest
allocations reach), the contiguous window `0x80000000`..`0x88000000`, and
`ld.so`, `libc`, pthread stacks and host `mmap`s top-down from about
`0xf5000000` (below the `0xFD000000`+ MMIO ranges). `dlopen`ed libraries land
in that top-down region too.

```sh
make -C runtime                 # build/xbrun, build/xbeinfo and the *_test binaries (hle_audio, audio, hle_d3d, hle_input, yuv, ...)
make -C runtime symbols         # build/default.syms via XbSymbolDatabaseCLI (XBE=..., XBSYMDB=...)
python3 tools/xdvdfs_extract.py "iso/Warriors, The (USA).xiso.iso" extract-all extracted/disc
```

## Run

```sh
cd runtime/build
./xbrun ../../extracted/default.xbe ../../extracted/disc --symbols default.syms [options]
```

| Option | Effect |
| --- | --- |
| `--symbols FILE` | `LIB__Name = 0xADDR` lines (XbSymbolDatabaseCLI output); symbolizes crashes and resolves hooks |
| `--hook SYM\|0xADDR` | success hook; with `--no-hle-d3d` the default is `D3D8__Direct3D_CreateDevice` when symbols are given |
| `--no-hle-d3d` | do not replace the D3D8 entry points; stop at the hook instead (the original D3D8 code would program the NV2A) |
| `--frames N` | exit 0 after N presented frames |
| `--dump-frame FILE` | on exit, write the last presented frame as a 24-bit BMP (at the renderer's output size; forces the readback present path) |
| `--video sdl\|x11\|null` | video backend (default `sdl`; with a remote `DISPLAY` such as `ssh -X` it uses plain Xlib `x11` first, because SDL2 exits when the server rejects MIT-SHM; falls back to `null` when no window can be opened) |
| `--renderer vulkan\|none` | host renderer (default `vulkan`: Vulkan device presenting to the SDL window, or read back into the presented frame, see `--present`; falls back to `none`, the CPU-only path, with a one-line notice when Vulkan cannot be initialized) |
| `--resolution WxH` | render at the largest 4:3 (16:9 with `--widescreen`) size inside WxH, e.g. `1920x1080` renders 1440x1080 and presents it pillarboxed; the title keeps its 640x480 device (viewports, screen-space HUD quads and the CPU-drawn movie frames are scaled); also the initial window size |
| `--render-scale N` | render at N x 640x480 (1-8); `--resolution` wins when both are given. Default: 640x480 |
| `--supersample N` | supersampling (2 or 4; default 1 = off): render at N x the size `--resolution` / `--render-scale` choose and box-filter the frame down to it (successive 2:1 linear blits, each an exact 2x2 average), before present or readback. Antialiases every edge, alpha-tested cutout and texture shimmer, which `--msaa` does not; the two combine (MSAA is resolved at the internal size). The window, `--dump-frame` and `--widescreen` framing keep the output size; HUD quads, glass ops and the character shadow maps are drawn at the internal size. Halved when the internal size exceeds the GPU's image limit; the startup line `[renderer] supersample Nx (internal WxH), ...` shows what is in use. Cost: N² x the pixels (4x / 16x the fill rate and target memory); measured GPU time per frame (`--stats` GPU wait, readback path, RTX 2060 shared with another GPU job, Quick Rumble fight, 1440x1080 output): 3.3 ms off, 5.5 ms at 2x, 6.6 ms at 2x with `--msaa 4`, 8.2 ms at 4x (4x MSAA alone: 4.8 ms). Suggested for a 4 GB laptop GPU such as an RTX 2050 at 1080p: `--resolution 1920x1080 --supersample 2 --shadow-scale 2 --shadow-soft`; if it drops frames, `--msaa 4` instead of `--supersample 2` |
| `--sharpen N` | contrast-adaptive sharpening of the finished frame, 0-100 (default 0 = off: the frame is byte-identical to a run without it). A full-screen pass at the output size after the supersampling downsample, before the swapchain blit or the readback (so `--dump-frame` shows it): the filter of AMD FidelityFX CAS (no scaling), written for this renderer (`src/renderer/shaders/cas.frag`, no AMD code): a 3x3 neighbourhood, a cross filter with a negative lobe weighted by how much room the local min/max leave (strong on soft detail, gentle on edges that are already high-contrast), normalized so flat areas are unchanged; strength = CAS sharpness N/100 (peak -1/8 to -1/5). The startup line `[renderer] sharpening (contrast-adaptive) N` confirms it; a device that cannot create the pass logs that and renders without. Cost (offscreen, RTX 2060, submit-to-fence time with the readback copy): 1080p 1.00 -> 1.19 ms per frame, 4K 3.67 -> 4.13 ms |
| `--bloom`, `--bloom-strength F`, `--bloom-threshold F` | bloom of the 3D scene (default off: frames and pipelines exactly as without it; strength 0.1-3, default 0.6; threshold 0-1, default 0.75; `--no-bloom` turns it off again). The scene ends at the frame's last depth-tested back-buffer draw (`src/bloom.c`: the title draws its HUD, text and menu overlays after the scene and without depth test; frames without a 3D draw, menus and movies, get no bloom); there the renderer splits its pass (`renderer_bloom`, as the HUD glass does) and runs the scene through a bright pass with a soft knee on the brightest channel, 13-tap downsamples (Karis-weighted first level) from half the internal size down to 1/16 of it (4 levels: the glow stays close to the light), 3x3 tent upsamples added level by level (RGBA16F), and screens the result onto the scene (scene + glow x (1 - scene): bright areas brighten without clipping into flat white) with one bilinear tap per pixel before the HUD is drawn; CAS (`--sharpen`) and the supersampling downsample come after it. Light-aware: while bloom is on, the target's otherwise unused alpha channel is a light mask for the scene draws: additive draws (fire, neon, flares) add their alpha, opaque draws clear it (shader constant, no blending), alpha blending fades it (the lamps' halo sprites included: they already are the glow, and treating them as lights bloomed every lamp into a large disk); where it is set the threshold drops by up to a fifth and the glow is up to 1.3x stronger; the bright pass gives at most white per channel. Halo draws (`bloom_halo_draw`: the lamps' halo and light-cone particle quads, SRC_ALPHA/INV_SRC_ALPHA, depth test on, depth write off, FVF 0x142, triangle list; centred on the shade rim, the shade hides their upper half and blooming the rest hung a ball under each lamp) are replayed right after the split (`bloom_order`, in their order, after the last back-buffer clear before it), so the bright pass never sees them and they look as with bloom off; with bloom off the replay order is unchanged. Vulkan 1.0 only (RGBA16F is a required blendable, filterable color format; B8G8R8A8 otherwise). The startup line `[renderer] bloom on (strength S, threshold T)` confirms it. Cost (GPU timestamps around the split, RTX 2060, offscreen): about 0.33 ms per frame at 640x480 and 0.54 ms at 1920x1440 (level51, averaged over 1400 frames; the 3x3-tent composite this replaced cost 0.95 ms at 1920x1440) |
| `--rain-hd`, `--rain-hd-density F` | better rain (default off: no pipeline, frames exactly as without it; density 0.25-4, default 1; `--no-rain-hd` turns it off again). The title's rain (Lua `StartRain`; `OE_Rain` queues it and the particle renderer draws it) is two full-screen quads with a scrolling streak texture; `src/rain.c` recognises them by render state and geometry (FVF 0x142 triangle list of 4 vertices, no depth test, SRC_ALPHA/INV_SRC_ALPHA, every vertex at clip z 0.5, w 1 and outside the screen, one colour, edges along the texture axes; no code addresses) and reads their colour and alpha, tiling, slant (the quad's v edge) and per-frame UV motion (fall and drift, summed while the layer keeps being drawn). With the option on, the replay draws one `renderer_rain` in place of the first layer and skips the layers: four depth slices from 1.5 to 24 view units, far first, each a full-target triangle at the slice's NDC depth (`A - B / d`, A and B fitted per frame from the depth-tested scene draws: clip z = A w - B), depth tested against the scene and not written, so nearer walls and characters hide the farther slices. The fragment shader draws a grid of procedural streaks in the tilted frame (one per cell at most, random place, a motion-blurred tail fading behind a brighter head, coverage-weighted width so sub-pixel streaks fade instead of shimmering); cell size, streak size and scroll speed scale with 1 / distance (near: long, wide, faint and fast, far: fine, dim and slow). Each streak also has its own distance between its slice and the next farther one (`gl_FragDepth`, `depth_greater` so the early depth test stays valid) and is sized by it, so the scene hides streaks one by one instead of in flat layers. Two passes per slice (the nearest: the lit pass only): a little flat colour and the veil (alpha blend), then a lit pass (DST_COLOR / ONE: the scene behind a streak brightened by up to twice), so streaks catch the scene's light, bright near lamps and bloom, faint in the dark. Opacity follows the title's layer alpha (its fade in and out), density follows the layers' tiling times `--rain-hd-density`. The title draws most of its world blended with depth writes and alpha test GREATER 1, so see-through fence, wire and crate texels write depth; in a rain frame each such draw before the rain is replayed with its alpha test raised to GREATER 0x20 (`rain_soft_depth`: texels under an eighth opaque neither write depth nor add their faint colour), so they no longer cut flat rain-free panels out of the slices. No title texture is used. Bloom: the layers have no depth test, so they come after the bloom split and the streaks are never bloomed. The startup line `[renderer] better rain on (density D)` and, at the first rain, `[rain] the title's rain (...) is drawn as procedural rain` confirm it. Wet surfaces are not changed. Splashes and ripples (`--rain-hd-splash`, on by default; `--no-rain-hd-splash`): in a rain frame the replay calls `renderer_rain_splash` right after the end of the opaque world (the SSAO split point; one shared pass split when `--ssao` runs too) with the frame's depth projection, the camera (the view and projection matrices of the last depth-tested fixed-function draw with a perspective projection: particles, sky; the title's world is z-up and its projection right-handed) and the title's clock (`rain_splash_build`). The renderer reads the depth (as SSAO: sampled in place, sample 0 with MSAA) and, per pixel, rebuilds the view position, the normal (depths 2 pixels away) and a flatness test (4-12 pixels, about 5 cm away); only surfaces facing up within about 40 degrees and flat (streets, roofs, crate tops; not walls or the curved tops of characters) go on. This is worked out at the output size (the internal size divided by the supersampling factor) into an R16G16 mask, then laid over the target bilinearly; with `--ssao` it runs inside the SSAO composite's pass. Their world position falls in a grid of ground cells (0.5 units, smaller above one impact per cell and period: about 0.24 at the default rate) each with one chance of an impact per 0.6 s at a random phase, place and size (30 impacts per square unit and second times the streaks' density), so the drops stay fixed in the world and only the title's clock moves them (same speed at 30 and 60 fps). The surface tests (normal, flatness: 8 more depth reads) run only where a drop shows. An impact throws 2-3 droplets up and out for 0.16 s (tiny spheres tested against the pixel's view ray, so they are seen in front of the ground behind them) and, on flat ground, a ring (up to about 0.09 units) widens over 0.5 s with a fainter inner ring; sub-pixel features are widened to a pixel and dimmed; everything fades out between 9 and 22 view units. Two composites: one brightens what lies under them (DST_COLOR / ONE, like the lit streaks: clear near lamps), one adds a little flat colour (ONE / ONE) so they do not vanish on the dark night ground; they sit under the particles, rain streaks and HUD drawn after them. Cost (RTX 2060, Quality preset: 1920x1080 widescreen, MSAA 4, supersample 2, bloom, SSAO, level51 ch3): about +0.4 ms. Covered ground is not known (screen space): drops land there too. The title's own `RainDrops` (Lua `CfgRaindrops`: 1-3 `sub_splash`/`sub_ripple` particle effects per update on its own list of ground triangles near the camera) are left as they are. The shader is the splash mode of `shaders/ssao_comp.frag` (specialization constant 0 = 1). The line `[rain] splashes and ripples after the opaque world (...)` at the first rain confirms it. |
| `--ssao`, `--ssao-strength F`, `--ssao-radius F` | screen-space ambient occlusion of the opaque world (default off: the depth target, frames and pipelines exactly as without it; strength 0.1-2, default 0.8; radius 0.1-8 world units, default 0.8; `--no-ssao` turns it off again). The opaque world ends at the frame's last depth-tested, depth-writing back-buffer draw after its last depth clear (`src/ssao.c`, `ssao_split`; measured in the hideout, level51 and its in-engine cutscene: the title draws its whole opaque world in one run, blending enabled but depth written, then the depth-write-off particles, smoke, fire, lamp halos and cones, then the HUD, text and rain layers without depth test); there the replay calls `renderer_ssao` with the frame's depth projection (clip z = A w - B, fitted per frame from the depth-tested draws, as for `--rain-hd`) and the camera's x/y scale (read from the world-view-projection constants of the last opaque draw's vertex program, or the fixed-function projection). The renderer splits its pass (as bloom does), samples the depth target (single-sample or sample 0 of the multisampled one) at half the internal size, rebuilds view positions and normals from it, takes 12 taps on a disc of the radius around each point (golden-angle spiral rotated per pixel), counts nearer surfaces within the radius by the cosine to the normal less a bias, fading with distance (a character far in front of a wall casts no halo on it), blurs the result twice (9 taps, depth-aware), upsamples it with the full-size depth (taps from another surface are left out) and multiplies 1 - strength x occlusion onto the color (rgb only: the bloom light mask in alpha is kept; the far plane and unoccluded pixels keep their exact color). Bloom, rain, the HUD and everything else after the split are drawn over it unchanged. Vulkan 1.0: needs the depth format sampled at the MSAA sample count (`sampledImageDepthSampleCounts`) and R16G16_SFLOAT (required); otherwise `[renderer] ambient occlusion unavailable on this device` and frames are drawn without it. The startup line `[renderer] ambient occlusion on (strength S, radius R)` and, at the first 3D frame, `[ssao] occlusion after the opaque world (...)` confirm it. Cost (RTX 2060, offscreen, hideout): about 0.32 ms per frame at 640x480, 1.0 ms at 1920x1080 with MSAA 4, about 2 ms with supersample 2 on top (3840x2160 internal) |
| `--msaa N` | N samples per pixel (2, 4, 8; the largest the GPU supports up to N), resolved before present; alpha-tested cutouts are not antialiased (no alpha-to-coverage) |
| `--aniso N` | anisotropic texture filtering up to N (clamped to the GPU limit; default off) |
| `--no-render-targets` | skip the title's own render-target passes (the character shadows) |
| `--shadow-scale N` | character shadow maps (the title's 1024x512 shadow render target, created at that size times the render height / 480) at N x that size (1-4, default 1): sharper shadow edges. Memory: a D16 map, 2 bytes per texel (1440x1080 output: 2304x1152 = 5 MiB at 1, 21 MiB at 2, 85 MiB at 4; clamped to the GPU's image limit) |
| `--shadow-soft` | soft character shadow edges: the shadow compare is filtered over 3x3 bilinear taps one texel apart (a 4x4-texel tent) instead of one 2x2 tap; the blur width is in shadow-map texels, so it narrows as `--shadow-scale` grows. Measured with `--shadow-scale 2 --shadow-soft` (1440x1080, RTX 2060, see `--supersample`): GPU time 3.3 -> about 4.5-5 ms per frame; `--shadow-scale 4 --shadow-soft` 5.5 ms |
| `--shadow-range`, `--shadow-range-dist F` | character shadows stop short of floors far below the caster (default off: the title's shadows, frames and pipelines exactly as without it; distance 0.5-50 world units, default 3; `--no-shadow-range` turns it off again). The title's shadow map holds only the characters, seen from a light above, and every world draw is a receiver, so a character on the hideout walkway also shadows the floor far below it. With this option the depth-stage compare (`geom.frag` `shadow_tap`, also under `--shadow-soft`) converts the receiver depth and each stored caster depth to the light's view depth and fades a failed compare to lit between F and 1.5 x F behind the caster (smoothstep). The light projection comes from the draws themselves: the depth stage's coordinates satisfy r = A q - B for a perspective light, fitted once per frame in the HLE (`src/shadow_range.c`) and passed per draw (`RPixelShader.shadow_depth`); a frame without a fit keeps the last one, and before any fit (or with an orthographic light) the compare is the original. F is measured from the top of the caster (the light looks down), so it must exceed a character's height plus a jump: the default 3 (a character is about 1.8 tall) keeps the shadow of standing, jumping and knocked-down characters on the ground under them. The startup line `[renderer] shadow range F (...)` confirms it. Cost: one vector divide per shadow tap, no pass or pipeline beyond a specialization constant |
| `--widescreen` | report a 16:9 TV to the title (the `XC_VIDEO_FLAGS_WIDESCREEN` setting `XGetVideoFlags` reads): it widens its camera field of view and HUD layout for anamorphic frames, which are presented 16:9 |
| `--language en\|es\|fr\|de\|it` | the console language reported to the title (`XC_LANGUAGE`, read by `XGetLanguage`; default `en`): it loads that language's string tables (`config_strings_<code>`) for menu and HUD text (cutscene subtitles not yet checked); voices are not translated. Any other value exits with the list of valid codes |
| `--present auto\|mailbox\|fifo\|immediate\|readback` | `auto` (default): a Vulkan swapchain in the SDL window (mailbox when the driver has it, else fifo), the frame is blitted (scaled, pillarboxed) into the swapchain image with no CPU readback; the others force that present mode, or `readback`, the read-back frame shown through SDL's renderer. Readback is also used with `--video x11\|null`, `--dump-frame`, a remote `DISPLAY`, and whenever the surface or swapchain cannot be made (logged as `[video] present: ...`). Known issue: the NVIDIA Linux driver crashes inside `vkGetPhysicalDeviceSurfaceSupportKHR` on an X server that is not NVIDIA's (Xvfb); use `readback` there |
| `--frames-in-flight N` | swapchain path (`--present auto\|mailbox\|fifo\|immediate`): how many frames the GPU may still be executing while the host records the next one (1-3, default 2). The host waits for a frame's fence only when its resources (command buffer, descriptor pool, geometry, program and base-image buffers) come round again N frames later, so command recording overlaps GPU execution; `1` is the old behavior (submit, present, wait). Texture uploads and new render targets still wait for the GPU. The readback path (`--present readback`, `--video x11\|null`, `--dump-frame`) always waits for its frame, so its output does not depend on N. The startup line `[video] present: Vulkan swapchain ... <mode>, ... images (no readback), N frames in flight` shows the present mode and N in use; `--stats` reports the fence wait as GPU wait |
| `--hud-hd` | restyle the target's health bar (top right, e.g. a car or enemy being hit) as a cleaned-up bar: thicker (grows down from its top edge to 4.3 % of its width), dark outline, the game's fill color with a vertical gradient, dark translucent empty part (off by default; see [HUD bars](#hud-bars)). `--hud-hd-height F` scales the height (0.5-4, default 1), `--hud-hd-opacity F` makes the bar translucent (0-1, default 1; the outline at sqrt(F) so the edge stays readable), `--hud-hd-frost N` blurs the background seen through the bar inside exactly its own rect, outline included (radius 0-32 in 640x480 pixels, default 0 = off; square corners, no margin, tint or rim); each turns it on. The rage meter under the character name is never changed |
| `--hud-hd-glass` | glass target bar preset (recommended look): the empty part very transparent with a light frosted tint over a blurred background, the fill translucent but vivid (it keeps the game's hue), a thin soft frame outline; sets track opacity 0.25, fill opacity 0.6, vivid 1, frost 4 (options after it override them; see [HUD bars](#hud-bars)) |
| `--hud-hd-track-opacity F`, `--hud-hd-fill-opacity F`, `--hud-hd-vivid F` | the glass style's controls (each turns on `--hud-hd` in the glass style): opacity of the empty part and of the fill (0-1, default `--hud-hd-opacity`), and color compensation of the translucent fill (0-1, default 0 = off; 1 gives back the saturation and brightness the fill's translucency takes away) |
| `--hud-glass` | draw the target's health bar as a low-opacity bar on a frosted-glass panel: blurred background, light tint and rim (off by default; see [HUD bars](#hud-bars)). Tunables, each also turning it on: `--hud-glass-blur N` (blur radius in 640x480 pixels, 1-32, default 4), `--hud-glass-opacity F` (bar opacity 0-1, default 0.45; the empty part of the bar at half of it), `--hud-glass-tint AARRGGBB` (tint color and strength, default `30ffffff`) |
| `--fullscreen` | start in desktop fullscreen; F11 or Alt+Enter toggle fullscreen in the SDL window (the swapchain follows the new size) |
| `--skip SYM:POP[:RET]` | patch a guest function to `mov eax,RET; ret POP` |
| `--audio sdl\|null` | audio output (default `sdl`: SDL2 loaded at run time, any driver SDL picks, e.g. PulseAudio/PipeWire/ALSA; falls back to `null` with a one-line notice when SDL2 or a device is unavailable). The mixer runs in both cases |
| `--audio-dump FILE.wav` | write the mix (16-bit stereo 48 kHz) to a WAV file; the header is updated every second and at exit |
| `--audio-log N` | log the first N calls per XACT/DirectSound entry point plus `[xact]` lines for banks, cues and notifications (default: 32 with `--trace`, else 0) |
| `--no-xact-controls` | A/B test: ignore the cue volume and fade controls ("V", "F") the title sets in `Prepare`/`PlayEx` and `SetParameterControl`, so every XACT cue plays at its sound entry volume (the behavior before they were applied). Logged as `[audio] xact controls: on` or `off` |
| `--sfx-3d-volume DB` | experimental: extra level in dB (-40 to +12, decimals allowed, default 0) on every positioned (3D) XACT sound effect; 2D cues (cutscene dialogue and music), Bink movies and streams are unchanged. Logged as `[audio] 3D sound-effect volume: ... dB`; an invalid value exits with status 2 |
| `--no-hle-audio` | do not replace the XACT/DirectSound entry points (reproduces the MCPX APU MMIO fault) |
| `--no-hle-input` | do not replace the XAPI input entry points; `XInitDevices` (USB/OHCI init) is then skipped with `ret 8` and the title sees no controller |
| `--input-script SPEC` | scripted pad input for headless runs (see Input below) |
| `--input-config FILE` | key map file (see Input below); default `warriors-input.cfg` in the current directory when present; an unreadable FILE exits with status 2 |
| `--pad-mappings FILE` | SDL controller mappings read instead of `gamecontrollerdb.txt` in the program's folder and the current directory (see Input below); an unreadable FILE is logged, not fatal |
| `--stats` | every 60 presented frames, print the presented rate and where a frame's time goes: guest frame time without the 60 Hz pacing sleep (and its draw recording: vertex fetch, vertex programs, lighting; vertices per frame), time Swap waited for the host, host render time split into command recording, GPU wait, readback and swapchain (image acquire + queue present), and present time (the SDL/x11 backend); and a second line with the input latency of frames that read the pad: age of the pad state at `XInputGetState` (window events are pumped every 4 ms and after each present), read to Swap, Swap to taken by the host, and render plus present; a third line splits the recording per recorded draw (µs/draw): setup (declaration, primitive expansion, index range, stream checks), state (base image, fog, lighting, matrices), pixel shader, textures (stage headers and texel keys), vertices (fetch and conversion), vertex programs (the interpreter), tail (indices, operation), plus the guest-memory checks made while recording (count per draw and their time; on Linux each is a `mincore` call); every 300 presented frames a `[pacing]` line: world updates per presented frame (0/1/2+, from the step counter at 0x5825fc, whose even values run the world update) and how many frames each world state stays on screen (1/2/3/4+, share held exactly 2), plus the title clock (`[[0x4aca1c]+0x40]`, ms) against host time as a game-speed check; a total line at `--frames`. With `--sim60` every step is a world update and the line counts it so: 1 update per frame and holds of 1 frame are the 60 Hz cadence |
| `--no-rate-fixes` | keep the HUD minigames' per-frame steps the title authored for 30 fps (see Rate fixes under Design; on by default). `--rate-fixes` turns them back on |
| `--no-frame-lock` | `QueryPerformanceCounter` on plain real time (see Frame lock under Design; the lock is on by default) |
| `--sim60` | experimental, off by default: the world (characters, AI, cameras) updates on every 1/60 s step with half the time step instead of every other step, so motion advances on every presented frame at the same game speed (see 60 Hz world under Design). `--no-sim60` turns it off again |
| `--rate-probe FILE[:MS]` | diagnostic: every MS (default 100) of game time (the title ms clock `[[0x4aca1c]+0x40]`, checked at each Swap) write a snapshot of the XBE's `.data` section (`.bss` included) into FILE, for the rate diff (see Rate diff under Design) |
| `--rate-probe-range ADDR:SIZE` | with `--rate-probe`: one more range to snapshot (up to 47), or `@PTR[+OFF]:SIZE` for one behind a pointer (re-read at each snapshot); hex with `0x` or decimal, multiples of 4 |
| `--poke ADDR:HEX[/EXPECT]` | diagnostic: write the bytes HEX at guest ADDR once the XBE is loaded, before the title starts (up to 64, 32 bytes each); with `/EXPECT` only when the original bytes match, otherwise xbrun exits with 2. Used by the cutscene sweep (`tools/cutscene_sweep/`) to start a chosen level and chapter from the profile load |
| `--warp MISSION[:CHECKPOINT]` | loading a profile starts story mission MISSION (1-23, the game's own numbering) at CHECKPOINT (a chapter, default 1) instead of the Coney hub; a level name works too (`--warp level81:3`, `level95` = the hub). One-shot: later level loads are the game's own. Saving is disabled for the whole session. An invalid value exits with code 2. See Mission warp under Design |
| `--no-default-skips` | with `--no-hle-input`, do not skip `XAPILIB__XInitDevices` (reproduces the OHCI MMIO fault) |
| `--fake-mmio` | experimental: back APU/ACI/USB MMIO with RAM plus a few "ready" bits (`src/apufake.c`) |
| `--patch-fs` | rewrite the title's `fs:[KPCR]` instructions as the Windows build must (`src/fspatch.c`); on Linux only to exercise that path |
| `--hdd DIR` | emulated hard disk (default `hdd/` in the working directory) |
| `--watchdog SEC` | after SEC seconds, sample every guest thread (EIP, backtrace) and exit 4 |
| `--trace` | log every kernel call, file open/symlink and HLE call (off by default) |
| `--log-limit N` | with `--trace`, log only the first N calls per kernel ordinal (`--quiet` is accepted and is the default) |

Default output (about 25 lines for a 90-frame run): load and patch summaries, thread
creation, `[file]` warnings (unmapped or missing paths), `[d3d] frame N presented`
(frames 1-3 and every 60th), the calls of one steady-state frame (`calls during frame 2`),
one-time notices, and either `[d3d] exit: frame limit reached` / `window closed` with
per-function D3D call counts (exit 0), `POC SUCCESS: reached <symbol> ... from <caller>`
(`--no-hle-d3d`, exit 0), a `[fatal]` report for unimplemented imports (exit 3), or a
`=== FAULT` report with EIP, fault address, registers, symbolized stack, EBP backtrace and
the last kernel calls (the ring of recent calls is kept even without `--trace`).

With `--trace`: `[krnl] t<thread> <ordinal> Name(args hex) -> ret [from caller]` for every
kernel call, `[file]` for every open and symlink, `[hle] Name(args hex) -> ret [from caller]`
for HLE audio calls (first 32 per function), and `[d3d] Name(args) -> ret [from caller]` for
every D3D call up to the first Swap and then the first call of each function.

Headless: `SDL_VIDEODRIVER=offscreen` (or `dummy`, or `--video null`) plus
`--frames N --dump-frame build/frame.bmp`.

### Audio

Sound plays through SDL2 (i386 build; `sudo apt install libsdl2-2.0-0:i386 libpulse0:i386
libasound2t64:i386` — `libasound2:i386` on older releases). SDL picks PulseAudio/PipeWire
when a server is reachable and ALSA otherwise; force one with `SDL_AUDIODRIVER=pulseaudio`
(or `pipewire`, `alsa`). WSL2 with WSLg: WSLg exports `PULSE_SERVER=unix:/mnt/wslg/PulseServer`,
so `./xbrun ... --symbols default.syms` plays through Windows; if no device opens, the run
continues silently (`[audio] falling back to the null backend`). Headless checks:
`--audio null --audio-dump build/run.wav`, then `python3 tools/wav_stats.py build/run.wav`.

3D sound effects (crickets, dogs, traffic and other positioned emitters) use a constant-power
pan (0.707 per ear when centered). The Xbox DSP image of 3D voices (HRTF, reverb) is not
emulated, so their real level is unknown; `--sfx-3d-volume DB` (experimental) trims them when
ambience drowns in-engine cutscene dialogue. Suggested values to compare: `0`, `-6`, `-10`.

### Input

The title always sees a standard controller in port 0 (player 1), plus one in ports 1-3 for
each further SDL game controller (see 2-player below). Keyboard and mouse (window focus; `sdl` and
`x11` backends) use a PC layout by default and always drive player 1:

| Key / button | Pad | Key / button | Pad |
| --- | --- | --- | --- |
| W A S D | left stick (move) | mouse (captured) | right stick (camera) |
| left mouse, R | X | I J K L | right stick |
| right mouse, Space | A | arrows | D-pad |
| F | B | E | Y |
| Q | left trigger | Left Shift | right trigger |
| Left Ctrl | Black | Tab | White |
| C | left stick click | middle mouse, V | right stick click |
| Enter | Start | Backspace | Back |
| G (hold) | spin the left stick clockwise | | |
| F1 | capture / release the mouse | Escape | release the mouse; quit when it is free |

Why this layout: the title's tutorial strings (`extracted/lua_src`) name the face-button
actions light attack, heavy attack / charge / select, grab and pick up / climb / interact; in
Xbox face-button positions those are X, A, B and Y, hence left mouse = light attack, right
mouse = heavy attack, F = grab, E = interact. The shoulder actions (target, "pull and hold";
sprint; block; hold for the crew menu) map to the Xbox triggers and Black/White, but which one
lands on which Xbox control is not verified; if an action sits on an awkward key, rebind it in
the key map file.

Spin (G): while held, the left stick turns the way car theft counts as clockwise (up, left, down, right in XInput axes) at `spin.rate`
turns per second (default 2, about 7 reads per quadrant at 60 Hz), over W A S D and a
controller; each press starts at up, and on release the stick follows W A S D again. It is
for the car-theft minigame, which asks for clockwise stick turns per screw.

Mouse look: click in the window to capture the pointer (that click is not passed to the game).
While captured the pointer is hidden and its motion drives the right stick: the speed (1500
counts/s = full deflection at sensitivity 1) is smoothed over `mouse.smoothing` seconds, so the
stick eases back to centre when the mouse stops, and any motion deflects at least
`mouse.deadzone` so slow moves are not lost in the title's stick dead zone. F1 toggles the
capture, Escape releases it (Escape quits only while the pointer is free; a held Escape does not
quit), and losing focus releases it and every held key. Right-stick keys override the mouse,
and a moving mouse overrides a controller's right stick.

- `x11`: `XGrabPointer` confined to the window with a blank cursor, and `XWarpPointer` back to
  the centre whenever the pointer leaves the middle half of the window. Under WSLg (XWayland on
  Weston, shown through RDP) warps and confinement may be ignored or approximate: the pointer
  then stops at the window edge or the screen edge and the camera stops turning until the mouse
  moves back. If that happens, try `--video sdl` (relative mouse mode) or use the I J K L keys.
  Held keys use Xkb detectable auto-repeat when the server supports it (`[input] x11 key
  auto-repeat: detectable (Xkb)`), otherwise the synthetic release/press pairs are filtered.
- `sdl`: `SDL_SetRelativeMouseMode`; key repeat events are ignored.

Key map file: `--input-config FILE`, or `warriors-input.cfg` in the directory xbrun is started
from. Lines change the built-in layout one key at a time:

```
key.w = left_stick_up       # key.NAME = CONTROL
mouse.right = a             # mouse.left|middle|right|x1|x2 = CONTROL
key.space = none            # unbind
mouse.sensitivity = 1.0     # 0.01 .. 100
mouse.invert_y = false      # true: mouse down looks up
mouse.deadzone = 0.15       # 0 .. 0.9
mouse.smoothing = 0.05      # seconds, 0 .. 1
key.g = spin_left_stick     # hold to spin the left stick clockwise
spin.rate = 2               # turns per second, 0.25 .. 10
```

Key names: `a`-`z`, `0`-`9`, `space enter backspace tab up down left right lshift rshift lctrl
rctrl lalt ralt f2`-`f12 minus equals lbracket rbracket backslash semicolon apostrophe grave comma
period slash` (`escape` and `f1` are reserved). Controls: `a b x y black white start back
left_trigger right_trigger left_thumb right_thumb dpad_up dpad_down dpad_left dpad_right
left_stick_up left_stick_down left_stick_left left_stick_right right_stick_up right_stick_down
right_stick_left right_stick_right spin_left_stick`, or the `--input-script` names below. Names are
case-insensitive and `#` starts a comment. Invalid lines are reported (`[input] FILE:LINE:
reason (line ignored)`) and the rest still applies. `warriors-input.cfg.example` restates every
default.

With the `sdl` backend SDL game controllers are used too (A/B/X/Y, Back, Start, sticks,
triggers, D-pad; LB = White, RB = Black); hot-plug is picked up. The first controller is player 1
and is merged with the keyboard and mouse.

Controller mappings: before the controllers are opened the runtime reads SDL GameController
mapping lines (`GUID,name,a:b0,...,platform:Linux,`, the format of the community
`gamecontrollerdb.txt`) from `gamecontrollerdb.txt` in the program's folder and, when it is another
one, in the current directory; `--pad-mappings FILE` reads that file instead. The log says
`[input] N controller mappings from gamecontrollerdb.txt (program folder)`. The launcher's
Controls page writes this file ("Set up this controller") for a pad SDL maps wrongly (LT read as
LB, A and B swapped...) or does not know; lines for another platform are skipped.

#### 2-player (story co-op, split-screen, Rumble versus)

Connect a second controller (XInput or anything else SDL2 knows as a game
controller), before starting the game or while playing:

- Controllers take ports in the order they are connected: the first is port 0 (player 1, together
  with the keyboard and mouse), the second port 1 (player 2), then ports 2 and 3. The log shows
  each assignment: `[input] controller "<name>" -> port N` (the game calls port N "controller port
  N+1").
- A controller keeps its port while connected. Unplugged, its port is freed and the game sees the
  removal as on the console (in co-op it pauses with "A controller has been disconnected from
  controller port 2..."); plugged back in, it takes its previous port again if that is still free,
  else the lowest free port. A fifth controller waits until a port is free.
- Story: while player 1 is walking around in a mission, press Start on the second controller and
  player 2 drops in; the screen splits into two halves, each with its own HUD and radar.
- The keyboard and mouse always play player 1, and so does the first controller connected. With
  one controller, keyboard and controller both drive player 1; player 2 needs the second
  controller.
- Quick Rumble: choose 1 ON 1 (or War Party), then "Vs." under Game Type; player 2 presses Start
  on the second controller when asked ("Player 2 press START").
- Rumble (vibration) is not output yet, for either player.

`--input-script FRAME:[pN/]CONTROLS[:FRAMES][,...]` holds CONTROLS from Swap number FRAME (the
counter `--frames` uses) for FRAMES frames (default 6; at most 1024 steps). CONTROLS are names joined by `+`: `up
down left right start back lthumb rthumb a b x y black white lt rt`, and full stick deflections
`lsup lsdown lsleft lsright rsup rsdown rsleft rsright`. Example: `--input-script
3200:start:10,3400:down,3500:a` (each step logs one `[input] frame N: script ...` line). A `pN/`
prefix (N = 0-3, default 0) sends the step to port N, and `FRAME:pN/plug` / `FRAME:pN/unplug`
(N = 1-3) connect or disconnect a scripted controller in port N from that frame on, e.g.
`6000:p1/plug,8200:p1/start:2` (player 2 joins a story mission once player 1 is walking).

### Texture dumps and replacement packs

Everything below is derived from your own copy of the game: keep dumps, packs and review pages
under `extracted/` (gitignored) and never commit, upload or share them.

- `--dump-textures DIR` writes every unique texture a draw samples once, as `DIR/<hash>.png`
  (level 0, RGBA8) plus `DIR/manifest.csv` (`hash,format,w,h,levels,linear,has_alpha,draws,
  first_frame`). Runs into the same directory merge (known textures are not rewritten, draw
  counts add up), so dump several scenes one after the other.
- `--texture-pack DIR` replaces every texture whose hash has a `DIR/<hash>.png` (any size; the
  mip chain is generated on load, uploaded as RGBA8; LIN_* textures work too because their
  coordinates are normalized by the original size). Files are read the first time the texture is
  drawn; with a pack the renderer's texture cache budget is 2 GiB instead of 512 MiB.
- `--texture-pack` can be given up to 8 times to layer packs: the first pack that has a hash
  wins, hashes only in a later pack still load from it (e.g. `--texture-pack <button-icon pack>
  --texture-pack texture_pack/<pack>` puts your own button-icon atlas over an upscaled pack).
  The log has one `[texpack] N replacement textures in DIR` line per pack and, with several,
  a `[texpack] K packs, first match wins: ...` line.

Button-icon pack: the button prompts are cells of the font/icon atlas (pack key
`886a4c14094e9597`), so a one-file pack with your own edited copy of that atlas (made from your
dump or your pack's file, at its size) given as the first `--texture-pack` replaces the in-game
button icons.

- Hash: FNV-1a 64 over {format code, width, height} and the level 0 texels as stored in guest
  memory (row padding of linear textures excluded), so it does not depend on addresses, runs or
  the mip count (`src/texpack.h`).

AI upscaling (Linux server with a Vulkan GPU, needs numpy): unpack the official
`realesrgan-ncnn-vulkan-20220424-ubuntu.zip` from the Real-ESRGAN v0.2.5.0 GitHub release into
`~/tools/realesrgan`, then:

```sh
make -C runtime                          # also builds runtime/build/pngraw used by the scripts
cd runtime/build   # or any work dir with xbrun and default.syms; xbrun creates hdd/ there
D=../../extracted/disc
./xbrun $D/default.xbe $D --symbols default.syms --video null --audio null --frames 3700 \
    --input-script 3200:start:10,3500:start:10 --dump-textures ../../extracted/texture_dump
# ... more runs (gameplay scripts) with the same --dump-textures directory ...
cd ../..
python3 tools/texture_upscale.py                     # -> extracted/texture_pack/realesrgan-x4plus
python3 tools/texture_review.py                      # -> extracted/texture_review/index.html
```

`texture_upscale.py` skips textures under 16 px, LIN_* and UI/font-like textures by default
(`--include-linear`, `--include-ui`, `--min-size`, `--skip-list FILE`), wrap-pads every texture
before upscaling (seamless tiling), upscales alpha separately and limits results to 2048 px
(`--max-size`). It only processes textures not yet in the pack (`--force` redoes them), so new
dumps can be added later. `--model realesr-animevideov3` is about 6x faster and keeps more fine
grain on ground/wall textures than the default `realesrgan-x4plus`.

Review: open `extracted/texture_review/index.html` in a browser (copy the folder to your PC, it
is self-contained), compare original and upscaled textures (size slider, click for a zoomed
side-by-side view), mark textures Approve/Reject and press "Download review.json". Apply it with
`python3 tools/texture_upscale.py --reject review.json`, which removes the rejected textures
from the pack (they then show the original) and keeps skipping them in later runs.

Windows: copy the pack folder (e.g. `extracted/texture_pack/realesrgan-x4plus`) to
`C:\Warriors\texture_pack` and run with it, e.g. from PowerShell
`TheWarriors.exe C:\Warriors\disc\default.xbe C:\Warriors\disc --hdd C:\Warriors\hdd --texture-pack
C:\Warriors\texture_pack` (the log reports `[texpack] N replacement textures` at the first 3D
frame and how many were replaced at exit).

#### Texture pack v2: DXT cleanup + game-texture upscaler (PyTorch)

The title's textures are DXT1/3/5 compressed, and Real-ESRGAN sharpens the 4x4 compression
blocks. Pack v2 first runs a 1x model trained to remove DXT artifacts, then a 4x model trained
on game textures, through `tools/spandrel_infer.py` ([spandrel](https://github.com/chaiNNer-org/spandrel)
loads the ESRGAN/RGT architectures). Detail DXT threw away cannot come back; the models
reconstruct plausible texture instead of blocks.

1. A Python 3.12 venv with CUDA PyTorch (NVIDIA GPU; uses ~2-5 GB VRAM). With
   [uv](https://docs.astral.sh/uv/) (keeps working when the system Python is newer than the
   PyTorch wheels):

   ```sh
   export TMPDIR=~/tools/.cache/tmp UV_CACHE_DIR=~/tools/.cache/uv; mkdir -p $TMPDIR
   uv venv --python 3.12 ~/tools/upscale-venv
   uv pip install --python ~/tools/upscale-venv/bin/python torch spandrel numpy
   ~/tools/upscale-venv/bin/python -c "import torch; print(torch.cuda.is_available())"   # True
   ```

2. Download the models yourself from their OpenModelDB pages into an empty directory
   `~/tools/upscale-models/` (keep the file names; compare the sha256 with the page):

   | Model | Page | License | File |
   |---|---|---|---|
   | 1x-DEDXT | https://openmodeldb.info/models/1x-DEDXT (MEGA link; `megatools dl URL` works) | WTFPL | `1x_DEDXT.pth` |
   | 1x-DXTDecompressor-Source-V3 | https://openmodeldb.info/models/1x-DXTDecompressor-Source-V3 (Google Drive) | WTFPL | `1x-DXTDecompressor-Source-V3.pth` |
   | 4x-GameAI-2-0 | https://openmodeldb.info/models/4x-GameAI-2-0 (Google Drive) | WTFPL | `4x-GameAI-2-0.pth` |
   | 4x-Textures-GTAV-rgt-s (optional) | https://openmodeldb.info/models/4x-Textures-GTAV-rgt-s (GitHub release, take the `.safetensors`) | CC-BY-4.0 | `4xTextures_GTAV_rgt-s.safetensors` |

   Model files are only read as weights: `.safetensors` with safetensors, `.pth` with
   `torch.load(weights_only=True)`; nothing in them is executed.

3. Build a pack with a chain (names are matched to the files ignoring case, `-` and `_`).
   Run the tool with the venv's Python (it needs numpy; the inference itself runs in a child
   process of the same venv, `--python`):

   ```sh
   V=~/tools/upscale-venv/bin/python
   $V tools/texture_upscale.py --backend spandrel --chain 1x-DEDXT,4x-GameAI-2-0
   #   -> extracted/texture_pack/1x-DEDXT+4x-GameAI-2-0   (about 4 min on an RTX 2060)
   $V tools/texture_upscale.py --backend spandrel --chain 1x-DXTDecompressor-Source-V3,4x-GameAI-2-0
   $V tools/texture_upscale.py --backend spandrel --chain 1x-DXTDecompressor-Source-V3,4x-Textures-GTAV-rgt-s
   #   -> about 13 min (the RGT model runs in fp32)
   python3 tools/texture_review.py   # one column per pack under extracted/texture_pack/
   ```

   Selection, wrap padding, separate alpha, `--max-size`, `--reject` and incremental runs work
   as for the ncnn backend; `--name` sets the pack directory name, `--tile` the inference tile
   size (lower it if the GPU runs out of memory), `--gpu N` picks the CUDA device. Work files go
   to a `.work-*` directory under `extracted/texture_pack/` (not `/tmp`) and are removed after
   the run.

4. Review: the page shows original | realesrgan-x4plus | each v2 pack side by side (click a
   texture for a zoomed view of all columns). Choose the pack under "Decisions for" before
   approving/rejecting; the downloaded `review.json` applies to that pack
   (`$V tools/texture_upscale.py --backend spandrel --chain ... --reject review.json`).
   Run the game with the pack you prefer: `--texture-pack extracted/texture_pack/<pack>`.

   What to expect (Coney yard at 1920x1080): all three chains remove the DXT blocks and keep the
   ground's grit where realesrgan-x4plus washes it out. The GameAI chains also over-sharpen some
   wood and metal (crates get dark, high-contrast "inked" grain); the GTAV chain is the most
   balanced but the slowest to build. DEDXT and DXTDecompressor give nearly the same result in
   front of GameAI.

## Windows (native, 32-bit)

The same runtime cross-compiles to a 32-bit Windows program that runs under WOW64 on 64-bit
Windows 10/11, rendering with the GPU's Vulkan driver (no WSL, no lavapipe):

```sh
make -C runtime windows   # build/TheWarriors.exe, build/xbrun-core.dll, build/fspatch_test.exe
```

Files to put in one folder on the Windows PC (for example `C:\Warriors\xbrun\`):

| File | From |
| --- | --- |
| `TheWarriors.exe` | `runtime/build/` (4 KB loader; earlier releases called it `xbrun.exe`) |
| `xbrun-core.dll` | `runtime/build/` (the runtime) |
| `SDL2.dll` | **32-bit** SDL2: the `SDL2-2.x.y-win32-x86.zip` asset of an SDL **2** release at https://github.com/libsdl-org/SDL/releases (not `win32-x64`, not SDL3) |
| `default.syms` | optional: `runtime/build/default.syms` from `make symbols` (symbolized crash reports) |
| `warriors-input.cfg` | optional key map (see Input); read from the working directory |

`vulkan-1.dll` is not copied: the GPU driver installs the 32-bit Vulkan loader in
`C:\Windows\SysWOW64` along with its 32-bit Vulkan driver (NVIDIA, AMD and Intel drivers ship
both). Nothing else is needed: the C runtime is the Universal CRT that Windows 10/11 include,
and libc++ is linked in (see Third-party software below). The game itself stays where you extracted it (your own disc); for example
with the extracted disc in `C:\Warriors\disc` (the folder that holds `default.xbe`):

```bat
cd C:\Warriors\xbrun
TheWarriors.exe C:\Warriors\disc\default.xbe C:\Warriors\disc --symbols default.syms --hdd C:\Warriors\hdd
```

All options work as on Linux (drop `--symbols default.syms` if you did not copy it). The
console shows the same log; the first lines should include `[load] fs: KPCR accesses
rewritten: 513`, `[video] backend sdl`, `[audio] SDL2 device open (driver ...)` (SDL usually picks
`wasapi`) and `[renderer] Vulkan on <your GPU>` and `[video] present: Vulkan swapchain ... (no readback)`. Keyboard, mouse and an XInput gamepad (through SDL's game
controller support) work as described under Input. `fspatch_test.exe` is a self-contained check
of the Windows `fs:` path (prints `fspatch_test: all checks passed`).

### Third-party software

No game files are included: each player supplies their own copy of the game (the launcher
extracts it from their own disc image). What the Windows build ships or compiles in:

| Component | Where | License | License text |
| --- | --- | --- | --- |
| SDL 2.32.10 | `SDL2.dll`, shipped next to the game (loaded at run time) | zlib | `LICENSE.txt` of the SDL2 release (`licenses/SDL2-LICENSE.txt` in the package) |
| stb_image v2.30, stb_image_write v1.16 | compiled into `xbrun-core.dll` (texture packs) | MIT or public domain | `third_party/stb/LICENSE` |
| Vulkan-Headers 1.4.363 (Khronos) | headers compiled into `xbrun-core.dll` | Apache-2.0 or MIT | Vulkan-Headers `LICENSE.md` |
| LLVM libc++, libc++abi, libunwind (zig 0.17.0) | linked statically into `xbrun-core.dll` | Apache-2.0 with LLVM exception | zig `lib/libcxx`, `lib/libcxxabi`, `lib/libunwind` `LICENSE.TXT` |
| mingw-w64 runtime support code (zig 0.17.0) | startup and support code in `TheWarriors.exe` and `xbrun-core.dll` | ZPL 2.1 (some parts public domain or BSD) | zig `lib/libc/mingw/COPYING` |

Not shipped, provided by the system: the Universal C Runtime (`api-ms-win-crt-*`, part of
Windows 10/11) and the Vulkan loader `vulkan-1.dll` with the Vulkan driver (installed by the GPU
driver). `glslangValidator` only compiles the port's own shaders at build time. The Linux build
uses the system's SDL2, Vulkan loader and X11 libraries.

HD output: for a 1080p monitor, render at 1440x1080 with 4x MSAA and
16x anisotropic filtering, presented through the swapchain (F11 toggles fullscreen):

```bat
TheWarriors.exe C:\Warriors\disc\default.xbe C:\Warriors\disc --symbols default.syms --hdd C:\Warriors\hdd --resolution 1920x1080 --msaa 4 --aniso 16 --fullscreen --stats
```

Add `--widescreen` for 16:9 (renders 1920x1080), and compare `--present readback` (the old
path) or `--present fifo` against the default in the `[stats] ... input latency` lines.
The SDL2.dll must be 2.0.6 or newer (`SDL_Vulkan_*`); with an older one the log says
`[video] no Vulkan window` and the readback path is used.

How it differs from Linux (details in `src/platform/win32.c` and
`src/fspatch.h`):

- **Loader**: `TheWarriors.exe` is linked without the C runtime at the fixed base `0x10000` with a
  7 MB uninitialized placeholder, marked large-address-aware and not relocatable
  (`tools/pe_flags.py`, since zig has no flag for these). Its image reserves the XBE range
  before Windows puts process parameters there; it then reserves the contiguous window
  `0x80000000`..`0x88000000`, loads `xbrun-core.dll` (fixed at `0x60000000`) and never returns:
  the runtime maps the XBE over the loader image. This is Cxbx-Reloaded's `cxbxr-ldr` approach.
- **%fs / KPCR**: FS is the TEB, and 64-bit Windows has no LDT to give it another base, so the
  title's 513 absolute `fs:[disp]` instructions (SEH chain, StackBase, Prcb, Irql,
  CurrentThread, DPC state) are rewritten at load into jumps to per-site stubs that read the
  thread's KPCR pointer from a TEB TLS slot (`fs:[0xE10 + 4*slot]`, layout checked at start).
  The guest SEH chain stays in the KPCR; Windows never walks it.
- Guest threads point the TEB stack range at their guest stack (host code called from the game
  runs there); guest stacks are at least 1 MB.
- Faults: a vectored exception handler skips `WBINVD`/`INVD` and prints the usual `=== FAULT`
  report for exceptions in game code or the runtime (exit codes as on Linux: 139 access
  violation, 132 illegal instruction). `--watchdog` samples threads with `SuspendThread`.
- Timing: 1 ms timer resolution (`timeBeginPeriod`), and monotonic deadlines converted for
  winpthreads, whose waits only take `CLOCK_REALTIME`.

Testing without Windows: there is no Wine on the build host. With Wine (Debian/Ubuntu:
`sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install wine32:i386`),
`wine build/fspatch_test.exe` and `wine build/TheWarriors.exe ...` would be a smoke test; Wine is not
WOW64 on real Windows, so the user's PC remains the reference.

### ReShade look

`reshade/Warriors.fx` is a self-contained ReShade effect (it only includes `ReShade.fxh` and
`ReShadeUI.fxh`, which every ReShade install has) and `reshade/Warriors.ini` a preset that enables
it. One technique, `Warriors`, chains five stages, each with its own
toggle and sliders: AMD FidelityFX CAS sharpening for the upscaled image, a color grade after the
1979 film's night look (lift/gamma/gain, gentle S-curve, warm sodium-vapor highlights, cool
shadows, skin-safe vibrance), film grain, a soft vignette and an output dither. Defaults are
subtle so HUD and subtitle text stay readable; `Split view` shows the original on the left half.

1. Run the ReShade installer, select `TheWarriors.exe` and choose the **Vulkan** API (the port presents
   through a Vulkan swapchain).
2. Copy `Warriors.fx` into the game folder's `reshade-shaders\Shaders` (next to `TheWarriors.exe`, for
   example `C:\Warriors\xbrun\reshade-shaders\Shaders\`).
3. Copy `Warriors.ini` next to `TheWarriors.exe`.
4. Start the game, open the ReShade overlay (Home key) and pick `Warriors.ini` in the preset list.

Depth-based effects (SSAO, depth of field, depth-masked fog) may not find the depth buffer: ReShade
guesses which depth attachment belongs to the scene, and with this renderer it may pick a wrong or
empty one. `Warriors.fx` uses no depth.

### HUD bars

Two opt-in styles for the target's health bar (the target panel top right that shows the car or
enemy being hit). The rage meter under the character name (player panel,
top left: a thin white line that fills up to unlock Rage mode) is never changed: a bar is a target
bar only when its center lies right of the screen's center (the player panels are on the left).

`--hud-hd` (recommended) rewrites the bar's quads while the draw is recorded, no extra render
passes: the strip grows downward from its top edge to 4.3 % of its width (the proportion of the
user's reference; never thinner than before, `--hud-hd-height` scales it), the empty track turns
dark grey and slightly translucent, the fill keeps the game's color and ratio with a lighter top
and a darker bottom, and a dark outline quad one pixel (at 640x480) around the bar is drawn first,
sampling the same atlas part. `--hud-hd-opacity F` then scales the alpha of the bar's quads by F
and the outline's by sqrt(F) (0.6 -> about 0.77), so the bar turns translucent while its edge stays
defined. `--hud-hd-frost N` records a glass operation (the renderer step `--hud-glass` uses) with
exactly the bar's rect including the outline, square corners, no margin, tint or rim and blur
radius N before the bar's draw, so what shows through a translucent bar is frosted; without it no
operation is recorded.

`--hud-hd-opacity` fades the whole bar, the fill's color included, and its filled outline quad
lies under the translucent bar, so the color washes out. The glass style (`--hud-hd-glass`, or any
of `--hud-hd-track-opacity`, `--hud-hd-fill-opacity`, `--hud-hd-vivid`) separates the parts: the
track only covers the empty part (its left edge moves to the fill's right edge, so nothing lies
under the fill) with a light frosted tint at the track opacity; the end caps take the track
opacity too; the fill keeps the game's hue with a darker bottom and is drawn at the fill opacity
after its HSV saturation and value are raised by vivid x (1 / fill opacity - 1) (capped at 1:
over a neutral background a fill at alpha a keeps a times its saturation and brightness, so vivid
1 gives that back as far as the color has room); the outline is a frame of four thin quads (one
pixel at 640x480, soft dark 90101010) instead of a filled quad. Recommended:
`--hud-hd-glass`, optionally with `--hud-hd-frost` for more or less blur.

`--hud-glass` draws the target bar as glass instead. The bars are recognized
per quad in the draws the title records: the HUD draws its 2D parts in batches per atlas page, and
a bar is the atlas part of the bar track drawn as a thin horizontal strip, plus the quads on that
strip (fill, colored tick, end caps; `src/hud_glass.c`). For each bar the runtime records a glass
operation just before the batch draw and fades the bar's quads (fill at the opacity, the empty
track at half of it). The renderer executes the operation by ending the render pass (the target
is stored and, with `--msaa`, resolved), copying the panel rect plus the blur radius, blurring it
with a two-pass gaussian into scratch images, resuming the pass and drawing the blurred copy as a
pill-shaped panel with the tint and a 1-pixel light rim (`src/renderer/shaders/glass_*.frag`). Only the panel
rects are processed, so the cost is a few small passes per bar. Names and other HUD elements are
unchanged; without the option no operation is recorded and frames are identical to before.

## Design

- **Loader** (`src/xbe.c`): headers mapped at the base (XAPI reads the
  certificate through `0x10118`), every section copied to its virtual address,
  entry/thunk addresses decoded with whichever XOR key (retail/debug/SEGA) lands
  inside the image. Every section is loaded up front; `XeLoadSection` only
  tracks the reference count.
- **Kernel imports** (`src/kernel.c`, `src/ktable.c`): each thunk slot points to
  a 16-byte stub `push ord; call kentry_common; add esp,4; ret N`, where `N`
  comes from the calling convention and argument size in `ktable.c`. Data
  exports point to host data. `ktable.c` is generated by
  `tools/gen_ktable.py` from ghidra-xbe `XbeLoader.java` (names by ordinal) and
  nxdk `lib/xboxkrnl/xboxkrnl.h` (signatures); 64-bit by-value arguments count
  as two dwords.
- **FS / KPCR** (`src/kpcr.c`): i386 glibc keeps TLS in `%gs` (also in
  pthreads; `%fs` stays 0), so `%fs` is free. One GDT TLS
  slot (`set_thread_area`) is shared by all threads with a per-thread base that
  points at a zeroed KPCR: `fs:[0]` exception list (-1), `fs:[4]/[8]` stack
  base/limit, `fs:[0x20]` Prcb, `fs:[0x24]` IRQL, `fs:[0x28]` current
  (E)THREAD. XAPI TLS: `_tls_array = 4` with a negative `_tls_index`, so the TLS
  block sits at `StackBase - TlsDataSize`, `KTHREAD.TlsData` (+0x28) points to
  it, and XAPI's thread startup copies the template (verified by disassembly of
  `XapiThreadStartup` and `mainCRTStartup`).
- **Threads** (`src/kobj.c`): `PsCreateSystemThreadEx` creates a pthread, which
  binds its own KPCR and switches to a guest stack (`guest_call` in
  `src/thunk.S`). The XBE entry point runs on its own thread; the host main
  thread services `KeTickCount`, kernel timers and DPCs (at IRQL 2).
  `PsTerminateSystemThread` longjmps back to the pthread's host frame instead
  of calling `pthread_exit` on the guest stack (glibc's forced unwind would walk
  guest frames that have no CFI).
- **Synchronization**: events, semaphores, mutants, timers and threads are real
  `DISPATCHER_HEADER` objects in guest memory, guarded by one global mutex and
  condition variable; waits re-check after every broadcast.
- **Memory** (`src/mem.c`): guest allocations come from `0x10000000..0x70000000`;
  contiguous memory comes from a 128 MB window at `0x80000000`, with
  physical address = VA - `0x80000000`. Pool allocations use the host heap.
- **Files** (`src/files.c`, `src/kfile.c`): `\??\D:` and `\Device\CdRom0` map
  to the extracted disc (read-only, case-insensitive lookup);
  `\Device\Harddisk0\PartitionN\...` maps to `hdd/partitionN/`, and a raw volume
  open (no trailing path) maps to a sparse `hdd/partitionN.img`, which XAPI
  formats as FATX for the utility drive.
  A new, empty `hdd` folder is enough: the runtime creates the folder itself (one level; its
  parent must exist, the launcher creates the whole path), the `partitionN/` folders and the
  `partitionN.img` files on first use, and the game creates its save folders
  (`partition1/TDATA`, `UDATA`). Checked headless with an empty folder: 1500 frames, exit 0,
  the partitions and save files created; a folder whose parent is missing exits 3.
  A root directory handle of `-3` (`ObDosDevicesDirectory`, used by XAPI for
  `D:\...` paths) resolves to `\??\`. A `..` path component is refused (no escape
  from the disc or `hdd/` roots). `NtQueryDirectoryFile` enumerates a directory
  handle one `FILE_DIRECTORY_INFORMATION` entry per call (case-insensitive `*`/`?`
  mask, `STATUS_NO_SUCH_FILE` on an empty first scan, then `STATUS_NO_MORE_FILES`);
  XAPI `FindFirstFile`/`XFindFirstSaveGame` use it on `U:\`.
  `make -C runtime build/kfile_test` builds its game-independent test.
- **Crypto** (`src/kxc.c`): `XcSHAInit`/`XcSHAUpdate`/`XcSHAFinal` (SHA-1, context in
  title-owned storage, at most the 116 bytes XAPI reserves) and `XcHMAC` (HMAC-SHA1 over two
  buffers; keys over 64 bytes are truncated, as on the console). XAPI signs saves with them;
  `XboxSignatureKey`/`XboxHDKey` are zero, so signatures are only self-consistent.
  `make -C runtime build/kxc_test` checks the RFC 3174 / RFC 2202 vectors.
- **HLE audio** (`src/hle_audio.c`, `src/hle_thunk.S`, on by default): the title
  calls the statically linked XACTENG/DSOUND libraries only through flat stdcall
  wrappers (`IXACTEngine_X(pThis, ...)`), so 51 entry points (32 XACT, 19
  DirectSound incl. the ones BINK uses) are patched with a `jmp` to a host stub
  `push idx; call hle_entry_common; add esp,4; ret N`, with `N` taken from each
  wrapper's own `ret N`. An entry is patched only if its first 5 bytes match the
  table (and its symbol, if known, resolves to the same address). Names ending in
  `?` are inferred from arguments and internal callees. They return opaque host objects:
  - DirectSound buffers (BINK movie audio: PCM 16-bit stereo, 48/44.1 kHz) are mixer voices
    reading the buffer memory (own or `SetBufferData`) at a play cursor that the mixer advances,
    so `GetCurrentPosition` follows what was mixed; volume, frequency and front mix bins apply.
  - XACT: wave banks are parsed in memory or streamed from the title's file handle (`src/xact.c`:
    XACT 1 `.xwb` v3, PCM and Xbox ADPCM), the sound bank (`.xsb` v11) resolves a cue to its
    play-wave events (wave, bank, loop count, volume), and each wave plays as a voice.
    Prepare/PlayEx/Stop, sound source status (playing while a cue on it plays; a prepared cue
    belongs to the source `PlayEx` names, since the title prepares without one) and the
    title's notifications (type 3 "data available" after `XACTEngineDoWork` for prepared cues,
    type 1 when a cue stops) follow the XBE's own matcher; `GetNotification` returns E_FAIL
    when nothing matches. `GetSoundCueProperties` fills the block like the XBE's own code
    (duration in ms, loop count, wave, 3D record). 3D cues are attenuated by distance and
    panned from their sound source and the listener. Parameter controls: the sound bank maps
    named per-cue values (this title: "V" volume on every sound, "F" fade on 2D sounds, "P"
    pitch on 3D sounds) to volume or pitch through 4-point piecewise-linear curves (table at
    header +0x14); the title sets them in the `Prepare`/`PlayEx` description and with
    `SetParameterControl` (e.g. an ambient bed at V 0.18 = -15.9 dB, faded in by F 0 -> 1
    over 5 s). Volume curves add to the entry volume (clamped to 0 dB), for 2D and 3D cues,
    with XACT ramps advanced once per `XACTEngineDoWork`.
    Not applied: pitch curves (the title never sets "P"), global controls ("MasterVolume"),
    Doppler, cones, category volumes, effects (I3DL2 reverb).
  `build/hle_audio_test` checks the stubs and the XACT/DirectSound behavior with synthetic banks
  (including the parameter controls, mixed level by level); `build/audio_test` covers the curve
  evaluation and the control table lookup.
- **Audio output** (`src/audio.c`): a mixer thread clocked by the host monotonic clock mixes
  the voices every 5 ms (48 kHz stereo, linear resampling, float accumulation, s16 output)
  and hands each chunk to the backend (`SDL_QueueAudio`, queue capped at 150 ms) and the
  `--audio-dump` file, so the dump is the same with or without a device. SDL2 is opened with
  `dlopen` like the video backend (`LoadLibrary` of `SDL2.dll` in a Windows build).
  `build/audio_test` (and `tests/test_audio.py`, which compares the ADPCM decoder with a Python
  port of vgmstream's) covers ADPCM, synthetic banks, streaming, the mixer and the WAV dump.
  `tools/wav_stats.py FILE.wav` prints per-second RMS/peak levels of a dump.
- **HLE D3D** (`src/hle_d3d.c`, `src/hle_d3d_thunk.S`, on by default): the 80
  D3D8 entry points the title calls are patched the
  same way as the audio ones; the argument bytes come from each function's `ret N`
  after following a leading `jmp`/`call`. Device, surfaces, textures, index buffers
  and shaders are guest-memory objects with the Xbox `D3DResource` layout (Common,
  Data = physical address in the contiguous window, Lock, Format, Size, Parent);
  the static device at `0x37d980` and `D3D_g_pDevice` are filled like the original
  `Direct3D_CreateDevice` does, and its push-buffer put/end pointers point at a
  scratch area (Swap reports it if unreplaced library code ever writes there).
  `Clear` fills the render target, `Swap` copies the back buffer into a mailbox and
  paces the caller to 60 Hz. Title code keeps writing D3D globals inline
  (`D3D_g_RenderState`, deferred state, dirty flags); nothing reads them yet.
  State setters and draw calls are counted no-ops, except the Bink movie quad below and the
  vertex state and indexed draws the host renderer uses.
  `build/hle_d3d_test` checks the table and objects without game data.
- **Movie quad** (`src/yuv.c`, `draw_movie_quad` in `src/hle_d3d.c`): SetTexture,
  SetPixelShaderProgram/SetPixelShader and SetVertexShader record the bound state. A
  `DrawVerticesUP(QUADLIST, 4, ..., 0x24)` with FVF `0x244`, a pixel shader definition whose
  seven combiner stages are Bink's YUV program, and 8-bit planes on stages 0-2 (`LIN_L8`, or
  swizzled `L8`, which is unswizzled from Morton order first) is converted on the CPU into the
  render target over the quad's screen rectangle (nearest texel, scaled to the quad). The
  coefficients are read from the title's shader constants: offsets from stage 2 `c0`, R and B
  from the two `x4` dot products of stage 3, G from the expanded `x2` dot product of stage 4
  (stage 1 texture = Cr, stage 2 = Cb). `build/yuv_test` checks unswizzling, the coefficient
  decode and the quad without game data. A real renderer replaces this hook.
- **Renderer** (`src/renderer.h`, `src/renderer/vk_renderer.cpp`, C++ behind a C ABI):
  offscreen Vulkan (no surface): a B8G8R8A8 color image (the XRGB8888 byte order Swap
  presents) and a depth image at the back-buffer size. `libvulkan.so.1` is opened with `dlopen`
  and every function resolved through `vkGetInstanceProcAddr` (`VK_NO_PROTOTYPES`), so the
  runtime still starts without Vulkan. GLSL shaders (`src/renderer/shaders/`) are compiled to
  SPIR-V by `glslangValidator` at build time and embedded as C arrays.
  Threading: guest threads only record a per-frame operation list next to the back buffer
  (clears, and draws with their vertices already transformed); Swap posts it with the frame and
  the host main thread, which also presents, replays it and reads the color image back over the
  presented pixels. No driver call runs on a guest thread or guest stack. Frames without draws
  keep the CPU path (movies). The back buffer as it was at the first draw is the frame's initial
  color, so CPU clears and movie quads drawn before it stay visible. The first clear-only frame
  is replayed once as a parity check (`[renderer] clear parity: N of N pixels match`).
  Device choice: first discrete GPU with a graphics queue, else the first one. On this machine
  the i386 NVIDIA ICD fails to initialize (`vk_icdNegotiateLoaderICDInterfaceVersion` returns
  `VK_ERROR_INITIALIZATION_FAILED`, while the same code built for x86_64 runs on the RTX 2060), so
  the i386 runtime uses Mesa llvmpipe (software Vulkan). `build/renderer_test` checks clear,
  readback and draws without game data (exit 77, skipped, when no device is available).
- **3D draws, untextured** (`src/geom.c`, `record_draw` in `src/hle_d3d.c`): `DrawIndexedVertices`
  and `DrawIndexedVerticesUP` are recorded with the bound vertex format: an FVF code, or the
  declaration of a `CreateVertexShader` object (Xbox `D3DVSD` tokens, incl. `NORMPACKED3` and
  `SHORT` types). The vertices the indices reach are copied from guest memory per draw and
  transformed on the CPU: fixed function as `v * World * View * Projection` (`SetTransform`,
  Xbox numbering VIEW 0, PROJECTION 1, WORLD 6); vertex programs by interpreting the title's own
  NV2A microcode (see Vertex programs below). Primitives become triangle lists, depth test follows
  `SetRenderState_ZEnable`, depth is clamped instead of clipped at the far plane, and draws
  with neither a pixel shader nor a texture get a debug color from the normal (or a position
  hash). Only draws into the device back buffer are rendered; `DrawVerticesUP` other than the
  Bink quad is not yet. `build/geom_test` checks the layouts, decoding and math.
- **Vertex programs** (`src/vsh.c`): `CreateVertexShader` keeps the `XGAssembleShader`
  microcode (header `0x78 | version << 8 | count << 16`, four dwords per instruction) decoded with
  the field table of xemu's `hw/xbox/nv2a/pgraph/vsh.c`; every program vertex runs through a CPU
  interpreter (MAC: mov mul add mad dp3 dph dp4 dst min max slt sge arl; ILU: mov rcp rcc rsq exp
  log lit; swizzles, negation, masks, paired MAC + ILU writing r1, `c[a0.x + n]`, r12 = oPos)
  with the constants from `SetVertexShaderConstant*`, giving oPos, oD0/oD1 (saturated) and
  oT0-oT3. The D3D library appends a screen-space transform with its viewport constants c-38/c-37
  to every program; the HLE writes scale 1 / offset 0 there and multiplies oPos.xyz by w to get
  clip space back. Programs without decodable microcode fall back to dp4 with c69..c72.
  Draws run the interpreter 32 vertices at a time (`vsh_run_batch`: each instruction decoded once
  per batch, components stored as rows of 32 lanes, built at `-O2` so the lane loops use SSE),
  bit-identical to the per-vertex `vsh_run`. `build/vsh_test` checks the interpreter on
  hand-encoded instructions and the batch form against the per-vertex one on random programs.
- **Textures** (`src/xtex.c`, `stage_texture`/`resolve_texture` in `src/hle_d3d.c`): each draw
  records, for every stage its pixel shader samples (stage 0 without one), the texture
  (`SetTexture`) header (Format, Size, Data) and a key from the
  address, the header and a sparse checksum of the texels; the host main thread looks the key up
  in the renderer's texture cache and on a miss decodes the texture from guest memory: DXT1/3/5
  blocks are uploaded as BC1/2/3 when the device samples them (else decoded on the CPU),
  swizzled (Morton order) and linear 8/16/32-bit formats are converted to B8G8R8A8, with all mip
  levels. Cube maps, volumes, P8 and depth formats (the title's shadow map) are not sampled: the
  stage reads white. Texture coordinates: FVF `TEXi`, or the program's oTi; linear textures take
  texel units. Sampled with wrap and trilinear filtering.
  The alpha test comes from the NV2A methods the title passes to `SetRenderState_Simple`
  (0x300 enable, 0x33c function, 0x340 reference). The cache is dropped as a whole beyond 4096
  textures or 512 MiB. `build/xtex_test` checks parsing, conversion, unswizzling, DXT decoding
  and the key; `build/renderer_test` the sampling, mips, BC1 upload and the alpha test.
- **Pixel shaders** (`src/psh.c`, `RPixelShader` in `src/renderer.h`, `shaders/geom.frag`): each
  draw records the bound `D3DPIXELSHADERDEF` (`SetPixelShader` / `SetPixelShaderProgram`) with its
  stage constants resolved (UNIQUE/SAME C0/C1, PSC0/PSC1 mappings to `SetPixelShaderConstant`
  values, final combiner constants) and the fog color (fog factor 1: the title does not enable
  fog). One precompiled fragment shader interprets the NV2A register combiners per fragment: up
  to eight general stages (input registers, channel and mapping per operand; AB/CD products or
  dot products, sum or mux on r0.a, output shifts/bias, blue to alpha) and the final combiner
  (A*B + (1-A)*C + D, alpha G, V1+R0 sum and E*F), over texture modes none / project2d /
  project3d / pass-through / clip plane on stages 0-3 and the vertex colors v0/v1. The frame's
  distinct programs live in a storage buffer selected by a push constant; draws without a
  definition use "stage-0 texture x diffuse"; draws with neither a definition nor a texture keep
  the debug color. Semantics follow xemu's `hw/xbox/nv2a/pgraph/glsl/psh.c`. `build/psh_test`
  checks the definition conversion, `build/renderer_test` the combiner evaluation on the GPU.
- **Output state** (`RState` in `src/renderer.h`): blend enable/factors/equation/constant
  color, color mask, depth mask and depth function arrive as NV2A methods through
  `SetRenderState_Simple` (0x304, 0x344/0x348, 0x350, 0x34c, 0x358, 0x35c, 0x354; values are the
  GL enums of xemu's `nv2a_regs.h`), culling through `SetRenderState_CullMode` (Xbox `D3DCULL_*`
  0/0x900/0x901). Each draw records the state; the renderer creates one Vulkan pipeline per
  distinct state on first use and caches it. The target has no alpha channel (like the
  X8R8G8B8 back buffer): DST_ALPHA reads as 1 and alpha is never written.
  `build/renderer_test` checks factors, equations, mask, depth write/function and culling;
  `build/hle_d3d_test` the method capture.
- **Video** (`src/video.c`): `video.h` is the swap point (open/present/pump/close);
  the SDL2 backend is loaded with `dlopen` (prototypes declared locally) and a
  `null` backend serves tests and headless runs. The host main thread (the
  kernel service loop) presents frames and pumps window events (keyboard and controller input); closing the
  window or Escape (while the mouse is not captured) exits. Swap hands the frame (pixels and
  recorded draws) to a one-entry queue, returns once the host main thread has taken it (the
  service loop runs about every millisecond) and paces the title to 60 Hz, so every swap is
  presented while the host keeps up, and a slower host throttles the title without frames piling
  up between input and screen (a three-entry queue added up to three frames of input latency on
  a CPU-bound Vulkan driver). When the host does not take a frame within 100 ms, the next Swap
  drops it; the exit summary reports swaps, presented and dropped frames.
- **HLE input** (`src/hle_input.c`, on by default): `XInitDevices`, `XGetDevices`,
  `XGetDeviceChanges`, `XGetDeviceEnumerationStatus`, `XInputOpen/Close/Poll`,
  `XInputGetCapabilities`, `XInputGetState` and `XInputSetState` are patched like the audio
  entry points. `XDEVICE_TYPE_GAMEPAD`
  reports port 0 always and ports 1-3 while a host controller (or a scripted plug) is in them;
  `XGetDeviceChanges` reports insertions and removals since the last call (a quick unplug and
  replug gives both), a removed port's handle stops working (`ERROR_DEVICE_NOT_CONNECTED`);
  `XInputGetState` returns the handle's port state (port 0: keyboard and script masks, mouse,
  spin plus the first controller; ports 1-3: their controller and script steps) as `XINPUT_STATE`, and the packet number increments whenever it changes; rumble
  completes at once without output. The host main thread writes the state from the video backend
  pump, Swap applies the script, guest threads read it, all under one lock.
  `build/hle_input_test` (src/hle_input.c with `HLE_INPUT_TEST`) checks it without game data.
  Host keys, mouse buttons and pointer motion go through `src/input_map.c` (platform-neutral:
  bindings, held-key state, auto-repeat filtering, mouse-look filter, key map file); the window
  backends only translate their events. `build/input_map_test` checks it with synthetic event
  sequences.
- **XAPI timing** (`src/hle_xapi.c`, always on): the title's `QueryPerformanceCounter`
  (0x3400f8) is `rdtsc`, while its `QueryPerformanceFrequency` returns the constant 733333333
  (the Xbox CPU clock), and the title's millisecond timer divides the counter by 733333. Native
  `rdtsc` ticks at the host rate, so title time ran several times too fast (e.g. its frame-rate
  meter read 15 fps at 60 presented frames/s and the title's adaptive draw distance cut the
  camera far plane and fog to 50 units). The entry is patched to count at 733333333 Hz from the
  host monotonic clock; `build/hle_xapi_test` checks it without game data. Other `rdtsc` sites
  (D3D `GetDisplayFieldStatus`, `CMiniport_IsFlipPending`, `D3DPalette_Lock`, XACT
  `CSoundBank_Play`) are not patched.
- **Frame lock** (`src/pacing.c`, `src/hle_xapi.c`, on by default, `--no-frame-lock` turns it
  off): the title's step loop (`FUN_002b2430`, game clock `[0x4aca1c]`, time from XAPI
  `QueryPerformanceCounter`) runs fixed 1/60 s steps in pairs once two steps have elapsed, and
  updates the world on every other step (step counter 0x5825fc), so the world runs at 30 Hz.
  With the counter on real time, the time of the title's clock read drifts against the 60 Hz
  Swap pacing and the pair slips between frames: a world state stays on screen for 1, 2 or 3
  frames. With the lock, the counter advances exactly one 60 Hz period per presented frame
  (held at the frame time for the first half of the frame, then following the host clock, never
  reaching the next frame time early), so the pair runs on every other presented frame. When the
  frame time is more than half a period away from real time (a missed deadline, a slow host,
  30 fps movies) it moves by whole periods to the one nearest real time: game speed stays real
  time within about a period and the steps keep their phase. With the lock, a Swap that missed
  its deadline also restarts the 60 Hz schedule from now instead of presenting the next frames
  back to back to catch up. Only this counter is
  locked; KeTickCount, KeQuerySystemTime and the audio mixer stay on real time. `--stats` shows
  the cadence in the `[pacing]` line; `build/pacing_test` checks the clock with a fake host clock.
- **Rate fixes** (`src/hle_ratefix.c`, on by default, `--no-rate-fixes` turns them off): the
  title asked for 30 fps; the port presents at 60 Hz. The world simulation follows the real
  clock, but the HUD manager (`FUN_000da2a0`, object 0x4d6298) calls each widget's update
  (vtable slot 5) once per rendered frame, and some widgets step by a fixed amount per call, so
  they ran twice as fast. The graffiti minigame (the player's tag state) is world-side: its
  update `FUN_001939d0` is reached only from the world update (0x16d8d0 -> 0x17c980 -> 0x179b10
  -> 0x193ea0; the odd steps return at 0x16d8e0), so it runs at 30 Hz like the Xbox by default and
  at 60 Hz only with `--sim60`. It counts updates and clamps its cursor step to 30 ms, a whole
  update at 30 Hz, so its two fixes apply only with the 60 Hz world (`--sim60`); with the 30 Hz
  world the title's bytes are right and stay. Each fix replaces one or
  two operands (a pointer to an existing read-only constant, or an immediate) after checking the
  instruction bytes around them; a fix whose bytes differ is skipped as a whole. One line
  reports them, e.g. `[ratefix] applied: lockpick (000e0fd4), ...; skipped: none; sim60 only
  (30 Hz world, title bytes kept): graffiti cursor, graffiti off-path` (with `--sim60` the
  graffiti fixes are listed as applied and the last part is left out).
  `build/hle_ratefix_test` checks the mechanism and the table without game data.

  | Fix | Address | Original -> new | Per-frame / per-update evidence |
  | --- | --- | --- | --- |
  | lockpick | 0xe0fd4 (operand of `movss xmm0,[..]` at 0xe0fd0, update of vtable 0x4243e8) | 0x41b658 (0.1f) -> 0x41bfec (existing 0.05f) | angle += speed (1/2/3) x 0.1 rad per update call, no frame time; press window 1.15 rad |
  | instruction arrow (`HUDEnableInstArrow`, vtable 0x424260) | 0xe0372, 0xe0390 (`movss xmm0,[..]` in the update at 0xe0330) | 0x410170 (-0.5f) -> 0x424de0 (-0.25f); 0x410414 (2.0f) -> 0x40ffc8 (1.0f) | bounce offset += step per update call, no frame time |
  | graffiti cursor (`--sim60` only) | 0x193b5c (imm8), 0x193b60 (imm32) in the tag update 0x1939d0 | `cmp eax,30` / `mov eax,30` -> 15 | cursor += stick x speed x min(dt, 30 ms): every 30 Hz world update (33 ms) is clamped to 30 ms, so the Xbox moved 30 ms worth per update; 15 per 60 Hz update gives the same speed |
  | graffiti off-path budget (`--sim60` only) | 0x193cc0 (imm8 of `cmp dword [esi+0x4fc],4`) | 4 -> 9 | +0x4fc counts world updates with the stick moved off the outline; above the limit the cursor snaps back and input freezes 500-600 ms while the can (a real-time 9.5-13 s timer) keeps emptying |

  Checked and left alone: the graffiti spray can (milliseconds of the title clock), the cursor
  trail (spatial: a point per 2 units moved, 300 per can), the button minigames behind
  mugging, cuffs and freeing an arrested member (deadlines on the title clock), and the other
  HUD widget updates (no per-call accumulation, or cosmetic only: the progress bar's spin once
  full doubles its speed every frame and has no clean single-operand fix).
- **Spray time** (`--spray-time PCT`, default 110, 100..200; part of the rate-fix pass, so
  `--no-rate-fixes` turns it off too): a comfort setting, not a 60 Hz fix. The graffiti spray
  can empties over a fixed number of milliseconds of the title clock by difficulty, 13000 /
  11000 / 9500 ms, read from the rows `{can ms, input freeze ms, cursor speed}` at .data
  0x45d57c (file offset 0x4504fc; picked by `FUN_00192f30` as 0x45d570 + 12 x difficulty and
  used by the can timer `FUN_00193300`); when it runs out the next can in the inventory starts or
  the tag fails. The pass checks all three rows (36 bytes) and scales the three durations, e.g.
  110% = 14300 / 12100 / 10450 ms; `--spray-time 100` writes nothing. The `[ratefix]` line lists
  it as `spray time 110% (0045d57c)`. A mission could override the rows with the Lua call
  `HuTagDifficulty` (row 0x45d5a0, not scaled); no shipped script uses it.
- **Mission warp** (`src/hle_warp.c`, `--warp`, off by default: without it no title byte
  changes). For testing a mission without replaying the game, and for headless reproduction:
  `xbrun default.xbe disc --warp 8:3`, then load a profile (STORY, use an existing profile).
  Headless, with a copy of a save: `--hdd copy --video null --audio null --input-script
  3200:start:10,3400:start:10,3600:a,3720:a,3840:a` loads the first profile about frame 3900.
  Missions (the game's `levelNames` field 5; checkpoints = field 4, its chapter count):

  | Mission | Level | Checkpoints | Mission | Level | Checkpoints | Mission | Level | Checkpoints |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | 1 | level99 | 1-3 | 9 | level86 | 1-4 | 17 | level55 | 1-3 |
  | 2 | level80 | 1-4 | 10 | level93 | 1-6 | 18 | level84 | 1-3 |
  | 3 | level87 | 1-5 | 11 | level31 | 1-6 | 19 | level82 | 1-4 |
  | 4 | level34 | 1-5 | 12 | level14 | 1-3 | 20 | level92 | 1-3 |
  | 5 | level2 | 1-4 | 13 | level9 | 1-3 | 21 | level83 | 1-6 |
  | 6 | level3 | 1-5 | 14 | level51 | 1-7 | 22 | level20 | 1-6 |
  | 7 | level5 | 1-4 | 15 | level52 | 1-4 | 23 | level11 | 1-6 |
  | 8 | level81 | 1-5 | 16 | level54 | 1-5 | hub | level95 | 1-12 |

  The table is Lua 4.0 bytecode in Main.wad0 and the game builds its native copy
  (`CfgLevelName`) only after boot, too late to validate the option, so the runtime keeps the
  level identifiers and chapter counts. How it works: loading a profile runs Lua
  `runNextMission`, which calls `SetCheckPoint(hub checkpoint)` and `MenuLoadLevel("level95")`
  (twice in a row). The Lua C function MenuLoadLevel (0x27fd90) loads its name with
  `push eax; call 0x7bca0` at 0x27fd9e; those 6 bytes jump to a host stub. On the first request
  for a `level...` name, and on any request that replaces it while it is still pending
  (`[[0x5b9f78]+0x148]` = 3, set by the level load), the stub calls the title's SetCheckPoint
  (0x337480, what Lua `SetCheckPoint` calls) with the chosen checkpoint and loads the warp
  level instead; afterwards every request passes unchanged. Log: `[warp] applied: ...`,
  `[warp] armed: mission 8 = level81, checkpoint 3 ...`, `[warp] MenuLoadLevel("level95") ->
  level81, checkpoint 3 (mission 8)`. All four sites (MenuLoadLevel, the level load,
  SetCheckPoint, the profile write) are signature-checked first; on a mismatch nothing is
  written and the warp is off.
  After the warp the game runs as usual: failing gives the game-over menu, whose "Last
  checkpoint" reloads the same chapter (checked headless: mission 8 chapter 3, run over by the
  cart twice); completing the mission runs the game's own `UnlockAndLoad`: the level is marked
  complete for the session, `LastLevel` becomes the highest mission completed in the save, and
  `runNextMission` loads the Coney hub at that mission's checkpoint (not redirected).
  Saving: the game writes progress only through the profile write `FUN_0033f510` (the only
  code that creates the save container and writes `profile.dat`; it runs from the save sequence
  `SSMC_StartSaveSequence` 0x72f80, used before a level chosen in the hub, rumble mode and the
  native save-pending paths, and from 0x130391). A warped session could mark a later mission
  complete or carry a forced checkpoint into a save, so with `--warp` that function is replaced
  for the whole session by a stub that logs `[warp] save blocked: profile slot N not written`
  and reports success without writing. Progress made in a warped session is therefore lost when
  the game is closed; play without `--warp` to keep progress.
  Known limitation: finishing a mission in a warped session may crash on the return to the hub
  (fault at 0x3df59d, in the Bink code); the suspected cause is the save block. Not fixed by user decision.
- **60 Hz world** (`src/hle_sim60.c`, experimental, off by default, `--sim60` turns it on): the
  step loop `FUN_002b2430` runs fixed 1/60 s steps in pairs once two are due, and `FUN_0016d8d0`
  runs the world update (AI, characters, cameras) on every second step with a 1/30 s time step
  (the float at 0x45cc38). With `--sim60` the loop runs single steps (with the frame lock: one per
  presented frame), the world update runs on every step and every writer of the time step uses
  half the value, so the game keeps its speed with twice as many world updates, like a 60 fps
  patch. The patch set is applied as a whole: all original bytes are checked
  first and on any mismatch nothing is written and the line names the mismatching sites
  (`[sim60] applied: none; skipped: ...`); otherwise `[sim60] applied: step count (002b24a0), ...;
  skipped: none`. `build/hle_sim60_test` checks the mechanism and the table without game data.

  | Patch | Address | Original -> new | Why |
  | --- | --- | --- | --- |
  | step count | 0x2b24a4 (imm of `mov dword [esp+0x14],2`) | 2 -> 1 | steps per loop pass |
  | step due / step again | 0x2b2476, 0x2b2572 (`shld edx,ebp,1; shl ebp,1`) | 6-byte nop | loop runs once one step (not two) is due |
  | world gate | 0x16d8e0 (`jne` after `test al,1` on the counter 0x5825fc) | 6-byte nop | world update on every step |
  | dt initial, dt setters | 0x45cc38 (.data); operands at 0x15aa2a, 0x15aa44, 0x15fe0b, 0x15fe30, 0x1b9029, 0x1b906d, 0x1b908c, 0x1b90b4, 0x1b9390, 0x1b93b0, 0x335ed8, 0x335f15, 0x335f2d | 1/30 -> 1/60 (0x410440 -> 0x410448); slow motion 1/60 -> 1/120, 1/120 -> 1/240 | every writer of the world time step (normal, time scale, slow motion) |
  | 1/240 | 0x44c070 (unused bytes between .rdata and .data, checked zero) | 0 -> 1/240 | the title has no 1/240 constant |
  | camera dt floor | 0x16e0c3 | 0x410440 -> 0x410448 | cameras step by max(dt, 1/30) |
  | AI think | 0x1aface, 0x1afad3 in `FUN_001afab0` | `(counter >> 1) % 5` -> `counter % 10` | each character thinks once per 1/6 s, as before |
  | menu clock | 0x458860, 0x6c93b (fixed fps 30 -> 60); 0x458878, 0x6c9c9 (frame ticks 1/30 -> 1/60 s) | | the menus' fixed-step clock adds 1/30 s per presented frame: two steps (two pad polls, which lose button presses) per frame; also the reason the front end runs at twice real time without `--sim60` |
  | human fade | 0x16dc37 | `sub cl,10` -> 5 | fading character alpha drops per update |
  | litter | 0x8b929, 0x8ba3a, 0x8c394 | 0x410440 -> 0x410448 | wind-blown litter moves velocity x 1/30 per update |
  | tap run | 0x16a0a8 (cap), 0x16a0c7 (offset), 0x16a0d0, 0x16a160 in `FUN_00169d00` | cap 10 -> 21, offset 2 -> 5, range 8.0 -> 16.0 (0x4106e0 -> 0x41ac0c), slow-down 1/30 -> 1/60 (30.0 at 0x410408 -> 60.0 at 0x41bb84) | `HuSetButtonTapControl` runs (mission 8 cart chase): speed = max x (1 - clamp((n - 2) / 8, 0, 1)) with n = world updates between two A presses, so at 60 Hz the same tapping gave a quarter of the speed and the cart ran the player over |
  | scene task rate | 0x2b0301 (`push 2` before `FUN_002b1080` in `FUN_002b0270`) | 2 -> 1 | in-engine cinematics and fixed scenes reschedule every 2 steps (30 Hz); at 60 Hz the cinematic camera moved on every second frame under 60 Hz characters |
  | scene delay, skip lock | 0x2b032e (operand), 0x2abe6e (imm) | 1.0 -> 0.5 (0x40ffc8 -> 0x410174); 60 -> 120 | the scene task's per-run start-delay countdown and skip lock-out keep their duration at twice the run rate |
  | drain gate | 0x17a754 (`dec word [edi+0x144]` in `FUN_0017a6f0`) -> jmp to code at 0x10c00 | the decrement runs on even step counts only | the 0x4000 state (3 s after standing in a hazard zone, e.g. fire) takes 1 health per world update |
  | hazard delay | 0x45cae4 (.data) | 30 -> 60 | updates (+0x660, counted down in the human update `FUN_0016adb0`) before a hazard zone starts that state |
  | struggle gate | 0x19d9aa (`mov cx,[0x45cb28]` in `FUN_0019d890`) -> jmp to code at 0x10c20 | the meter decay (`CfgButtonMash(15, 250, 1000)`: 15 per update) is subtracted on even step counts, 0 on odd | grapple struggle meter: presses add, every update without one subtracts |
  | workout gate | 0x17b3f4 (in the workout state `FUN_0017b020`) -> jmp to code at 0x10c40 | the hold countdown and the gain/decay step run on even step counts; odd ones keep the progress | gym minigames (`CfgWorkoutParams`): a press sets a hold count, each update adds the gain while it lasts, else subtracts the decay |
  | blood gush | 0x17001c (imm of `mov dword [edi+0x31c],30`) | 30 -> 60 | blood effect attached for 30 updates |

  The three gates are code in the unused tail of the image-header page (SizeOfHeaders 0xbd4; the
  page is mapped and zero up to 0x11000, checked zero before writing): `test byte [0x5825fc],1`
  on the step counter, which with `--sim60` advances once per world update, so the gated code runs
  30 times a second as on the Xbox.

  Verified headless (mission-1 script): one world update per presented frame in gameplay, title
  clock at real time, the front end at real time, the mission intro cutscene ending at the same
  game time as without it. Known differences that remain: systems that count world updates
  instead of using the time step and were not patched run at twice their rate, among them the
  litter's damping and lifetimes, the held-button test (`CfgButtonHeldFrames(7)`: 7 pad polls in an
  8-entry history, so a hold is recognized after 117 ms instead of 233 ms) and an airborne
  fail-safe (+0x344, 60 updates).
  The pad is polled once per world update, so a frame that runs two steps to catch
  up can lose a press for the menus. Combat feel, minigames and physics are not verified. The
  graffiti rate fixes (Rate fixes above) are world-side (the tag update is reached only from the
  world update), so they match a 60 Hz world and the rate-fix pass applies them only when the
  60 Hz world is active (the sim60 patch set applied).
- **Rate diff** (`src/rateprobe.c`, `tools/ratediff/ratediff.py`, diagnostic): finds systems that
  count world updates instead of game time, which run at twice their rate with `--sim60`. Run the
  same input script twice, without and with `--sim60`, each with `--rate-probe FILE`; then

  ```sh
  /usr/bin/python3 tools/ratediff/ratediff.py base.rp sim.rp --from-frame 6900 --to-frame 13000 \
      --xbe ../extracted/default.xbe --closure closure.txt --out ~/.cache/rate-diff/report
  ```

  The probe file holds a header and, per snapshot, the Swap number, the game ms and per range a
  full copy or the dwords changed since the previous snapshot (format in `src/rateprobe.h`;
  `.data` is about 1.5 MB, a 13000-frame run a few tens of MB). The tool aligns both runs by game
  time inside the Swap window (runs are not frame-deterministic, so it compares rates, not values):
  per 4-byte field it sums the absolute change per game second as int32, float32 (when every
  value is a plausible float) and, for integers, each 16-bit half; pairs across a gap of game time
  (loading) or an unreadable sample do not count. A field is flagged when both runs changed it
  in at least 20 samples, the ratio sim60/default is 1.6-2.5 (or 0.4-0.625) and at least 75% of
  the chunks of game time (8 by default) agree. Flagged fields are grouped (the character pool
  0x4ff878, 60 x 0x668, by field offset; other addresses by adjacency) and listed with the
  instructions that reference them from an objdump of `.text` (`--xbe`, or `--disasm` for an
  existing listing): absolute operands, `disp(%reg)` operands with the field offset for pool
  fields, indexed arrays; `*` marks those inside the world-update closure (`--closure`, lines
  `start_hex size`). Output: `PREFIX.md` (ranked groups) and `PREFIX.csv` (every field). Limits:
  only ranges that were snapshotted (heap objects need `--rate-probe-range`), changes faster than
  the interval alias, and a field written through a computed pointer has no direct reference.
  `build/rateprobe_test` and `tests/test_ratediff.py` check both sides without game data.
- **Cutscene sweep** (`tools/cutscene_sweep/`, diagnostic): runs the opening scene of every story
  mission chapter (23 missions, 104 chapters from `levelNames` in `config_preload3.lua`) headlessly,
  once at 30 Hz and once with `--sim60`, each with `--rate-probe`, and compares them keyed by game
  time since the level start. A chapter is reached from a profile load on a copy of a save with
  three `--poke`s (the level name at 0x44c078, MenuLoadLevel's `call` at 0x27fd99, GetCheckPoint
  0x335f40 returning the chapter). `scenediff.py` reports per scene characters that move about 2x
  (or 0.5x) per game second, (dis)appear at other game times, change state at about half the game
  time, drift apart, the camera, the script timer list, a scene clock from matched scripted
  placements, and the rate-diff fields (character objects behind +0xb4/+0x170 included):

  ```sh
  /usr/bin/python3 tools/cutscene_sweep/sweep.py run -j 4        # nice 19; ~/.cache/cutscene-sweep/runs
  /usr/bin/python3 tools/cutscene_sweep/sweep.py compare -j 3    # report.md / report.csv there
  # a candidate fix as extra pokes on tagged --sim60 runs, compared with the same base runs:
  /usr/bin/python3 tools/cutscene_sweep/sweep.py run --sim-only --tag fix --extra-pokes pokes.txt
  /usr/bin/python3 tools/cutscene_sweep/sweep.py compare --tag fix
  ```

  Runs are not frame-identical (AI choices drift), so single diverge/fast/slow findings are noise;
  a mechanism shows as a bias across scenes or a field at 2.00. `tests/test_cutscene_sweep.py`
  checks the tool on synthetic probe files.
- **Text fixes** (`src/hle_textfix.c`, on by default, `--no-text-fixes` turns them off): known
  mistranslations in the shipped string tables, corrected in the data `NtReadFile` returns; the
  files on disk are not changed. Each fix names a file (base name, any case), the console
  language (`--language`), the exact old bytes and new bytes of the same length (string lengths
  and file offsets stay intact) and optionally the absolute offsets where the old bytes are
  expected; without offsets every read of the file is scanned. A read covering only part of a
  match gets its part of the new bytes (the rest of the match is read from the file to check
  it); bytes that differ are left as read and reported once. Startup prints e.g.
  `[textfix] armed: sit ups (Main.wad0, 2 offsets)` (or `none for XC_LANGUAGE 1`), and the
  first read of each place prints `[textfix] sit ups: Main.wad0+268301935 'Sentadillas' ->
  'Abdominales'`. `build/hle_textfix_test` checks the mechanism without game data; with
  `../extracted/disc/Main.wad0` as argument it also reads the real file in chunks of several
  sizes and checks that only the two matches change (Spanish) or nothing does (English).

  | Fix | File, language | Offsets | Original -> new |
  | --- | --- | --- | --- |
  | sit ups (gym label lists, next to "Flexiones") | Main.wad0, es (XC_LANGUAGE 5) | 268301935, 268996894 (the only two occurrences) | "Sentadillas" (squats) -> "Abdominales" |

  **Keyboard/mouse control names** (`--button-icons kbm`, or `auto` unless a controller is
  connected, or was the last device used, when the first such text is read; not affected by
  `--no-text-fixes`): the tutorial and HUD texts name the sticks in words, each phrase a separate
  Lua string constant of the scripts' Xbox branch. They become nouns that fit the article before
  them, built at start-up from the key map: the left stick the keyboard with the movement keys
  (forward, left, back, right), the right stick the mouse, and in the gang command tips that say
  to click the left stick, the stick-click key. The title's text renderer (0xe2580) skips any tag
  it does not know (no match in the tag name table at 0x45a610), so a shorter text is padded with
  `<~..~>` (one byte short: a space) and string lengths stay intact. The keys are the key cap
  labels (spaced, unspaced, short forms) or the arrows word; when they do not fit, or a direction
  is unbound, the noun alone. One constant serves every sentence of a script, so the click places
  are offsets in Main.wad0, chosen per script by the majority of its sentences (level31, level34,
  level80, the English command help); the others are scanned. Default key map:

  | Language | Left stick -> | Right stick -> | Click places -> |
  | --- | --- | --- | --- |
  | en | "left thumbstick" -> "keyboard (WASD)" | "right thumbstick" -> "mouse" | "C key" |
  | es | "stick analógico izquierdo" -> "teclado (W A S D)" | "... derecho" -> "mouse" | "C", and "hacer clic en el" -> "oprimir la tecla" |
  | fr | "stick analogique gauche" -> "clavier (W A S D)" | "... droit" -> "mouvement de la souris" | "C" |
  | de | "Ministick links" -> "WASD-Block" | "Ministick rechts" -> "Mauszeiger" | "Linken/linken Ministick klicken" -> "C-Taste drücken" |
  | it | "levetta sinistra" -> "tastiera (WASD)" | "levetta destra" -> "mano sul mouse" | "C" |

  Sentences that share a constant with the majority keep its text: in level31 the aiming tip and in
  level80 the car-radio tip show the click key (en/es/fr/it); the Spanish, French and Italian
  command help shows the keyboard. The first match logs `[textfix] control names: ...` (auto) and
  each fix's first place. `hle_textfix_test` checks the nouns, the padding, every language, the
  gating and, with Main.wad0, the click places and a movement sentence per language.
- **Privileged instructions**: title code runs `WBINVD` after loading GPU data;
  the SIGSEGV handler skips `WBINVD`/`INVD` (logged once).
- **Hooks**: success hooks are a `jmp` to a per-hook stub that reports the hook
  and its caller.

## Status

The design notes and known differences of each subsystem are in the sections above.
