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
| `--supersample N` | supersampling (2 or 4; default 1 = off): render at N x the size `--resolution` / `--render-scale` choose and box-filter the frame down to it (successive 2:1 linear blits, each an exact 2x2 average), before present or readback. Antialiases every edge, alpha-tested cutout and texture shimmer, which `--msaa` does not; the two combine (MSAA is resolved at the internal size). The window, `--dump-frame` and `--widescreen` framing keep the output size; HUD quads, glass ops and the character shadow maps are drawn at the internal size. Halved when the internal size exceeds the GPU's image limit; the startup line `[renderer] supersample Nx (internal WxH), ...` shows what is in use. Cost: N² x the pixels (4x / 16x the fill rate and target memory); measured GPU time per frame (`--stats` GPU wait, readback path, RTX 2060 shared with another GPU job, Quick Rumble fight, 1440x1080 output): 3.3 ms off, 5.5 ms at 2x, 6.6 ms at 2x with `--msaa 4`, 8.2 ms at 4x (4x MSAA alone: 4.8 ms). No launcher preset turns it on; for most GPUs `--msaa 4` is the cheaper antialiasing |
| `--sharpen N` | contrast-adaptive sharpening of the finished frame, 0-100 (default 0 = off: the frame is byte-identical to a run without it). A full-screen pass at the output size after the supersampling downsample, before the swapchain blit or the readback (so `--dump-frame` shows it): the filter of AMD FidelityFX CAS (no scaling), written for this renderer (`src/renderer/shaders/cas.frag`, no AMD code): a 3x3 neighbourhood, a cross filter with a negative lobe weighted by how much room the local min/max leave (strong on soft detail, gentle on edges that are already high-contrast), normalized so flat areas are unchanged; strength = CAS sharpness N/100 (peak -1/8 to -1/5). The startup line `[renderer] sharpening (contrast-adaptive) N` confirms it; a device that cannot create the pass logs that and renders without. Cost (offscreen, RTX 2060, submit-to-fence time with the readback copy): 1080p 1.00 -> 1.19 ms per frame, 4K 3.67 -> 4.13 ms |
| `--fsr MODE`, `--fsr-sharpness F` | AMD FidelityFX Super Resolution 1 (default `off`: no resource is created and every frame is as without it). MODE `ultra` (1.3x), `quality` (1.5x), `balanced` (1.7x) or `performance` (2x per axis): the 3D scene renders at the output size (`--resolution` / `--render-scale`) divided by that factor, rounded to the nearest pixel per axis (`src/fsr.c`; 1920x1080 balanced -> 1129x635), then two full-screen passes at the output size upscale it with EASU and sharpen it with RCAS (`src/renderer/shaders/fsr_easu.frag`, `fsr_rcas.frag`: GLSL ports of the 32-bit path of AMD's `ffx_fsr1.h` v1.20210629, MIT). `--fsr-sharpness F`: RCAS sharpness in stops, 0 (sharpest) to 2, default 0.2 (AMD's sample default). Every scene pass follows the internal size, as at a smaller `--resolution`: MSAA, glass, bloom, `--ssao`, lamp lights, light shafts, contact shadows, rain, `--filmic` and the title's render targets all run there, so nothing samples a target of another size. After the upscale come the gamma ramp, the frame counter, the swapchain blit and the readback (`--dump-frame` writes the output size). The HUD and text are drawn into the same target as the scene and are upscaled with it. FSR replaces `--supersample` (rendering above the output size; ignored, `[fsr] warning: --supersample N ignored ...`) and `--sharpen` (RCAS instead of CAS; ignored, `[fsr] warning: --sharpen N ignored ...`). The startup line `[fsr] mode=quality render=960x720 output=1440x1080 sharpness=0.20` confirms it; an output too small to scale down, or a device that cannot create the passes, logs `[fsr] mode=M unavailable ...` and renders and presents at the internal size without upscaling |
| `--fps-overlay`, `--fps-overlay-corner C`, `--fps-overlay-items LIST` | a small frame counter (default off: no resource is created, the swapchain keeps its usage and frames are byte-identical; `--no-fps-overlay` turns it off again; corner `tl`, `tr`, `bl` or `br`, default `tl`). `src/fps_overlay.c`: the host loop counts presented frames over windows of at least 0.5 s and formats `60 FPS  16.6 ms` (frames per second rounded, mean frame time); the renderer gets a new text only when it changes (`renderer_fps_overlay_text`, about twice a second). It draws white text with a one-font-pixel dark outline (no background box, so nothing behind the text is hidden) in a built-in 5x7 bitmap font (`src/fps_font.inc`, digits, `.`, `F P S m s`; written for this runtime, shared by the C code and `shaders/fps_overlay.frag`) as one scissored triangle with the text in push constants: no texture, upload, descriptor set or per-frame allocation. It is the last thing drawn: on the swapchain image after the blit (the swapchain then gets `COLOR_ATTACHMENT` usage), or on the read-back frame after the sharpening (so `--dump-frame` shows it), at that target's own pixels, so it is crisp at any `--render-scale` and never bloomed, graded or sharpened. One font pixel is round(height / 450) target pixels (2 at 1080p: digits 14 pixels tall, the text area about 190 x 22). Frames that skip the renderer (the CPU path) show no counter. The startup line `[renderer] fps overlay on (top left corner)` confirms it. `--fps-overlay-items` picks any of `fps`, `ms`, `cpu`, `gpu` (default `fps,ms`, the text above), joined in that order with two spaces, up to 40 characters (`999 FPS  999.9 ms  CPU 100%  GPU 100%`; the font also has `C U G % -`). CPU %: `plat_process_cpu_ms` (getrusage `RUSAGE_SELF` on Linux, `GetProcessTimes` on Windows: user plus kernel time of every thread) over the same window, divided by `plat_logical_cores` so 100% means every logical core is busy. GPU %: only with `gpu` the renderer creates a timestamp query pool (two per frame slot, `RendererConfig.fps_overlay_gpu`): a timestamp at the start of each frame's command buffer and one after the scene and its post-processing, before the readback copy or the swapchain blit (which may wait for the window's image); the pair is read when the slot is recorded again, after its fence was waited, so `vkGetQueryPoolResults` never waits. Ticks are masked to the queue's `timestampValidBits` and scaled by `timestampPeriod`; the mean busy time of the frames completed in the window over the mean frame period gives the percentage, clamped to 0-100. It is the game's own GPU work, not the whole GPU's utilization; a queue without timestamps shows `GPU --` (and the startup line says so). Cost: one triangle over about 190 x 22 pixels per frame; hideout at 1920x1080 (MSAA 2, bloom, filmic, sharpen 40, readback, RTX 2060), `--stats` host render over 30 windows back to back: 10.07 ms with it, 9.98 ms without (within run-to-run noise) |
| `--bloom`, `--bloom-strength F`, `--bloom-threshold F` | bloom of the 3D scene (default off: frames and pipelines exactly as without it; strength 0.1-3, default 0.6; threshold 0-1, default 0.75; `--no-bloom` turns it off again). The scene ends at the frame's last depth-tested back-buffer draw (`src/bloom.c`: the title draws its HUD, text and menu overlays after the scene and without depth test; frames without a 3D draw, menus and movies, get no bloom); there the renderer splits its pass (`renderer_bloom`, as the HUD glass does) and runs the scene through a bright pass with a soft knee on the brightest channel, 13-tap downsamples (Karis-weighted first level) from half the internal size down to 1/16 of it (4 levels: the glow stays close to the light), 3x3 tent upsamples added level by level (RGBA16F), and screens the result onto the scene (scene + glow x (1 - scene): bright areas brighten without clipping into flat white) with one bilinear tap per pixel before the HUD is drawn; CAS (`--sharpen`) and the supersampling downsample come after it. Light-aware: while bloom is on, the target's otherwise unused alpha channel is a light mask for the scene draws: additive draws (fire, neon, flares) add their alpha, opaque draws clear it (shader constant, no blending), alpha blending fades it (the lamps' halo sprites included: they already are the glow, and treating them as lights bloomed every lamp into a large disk); where it is set the threshold drops by up to a fifth and the glow is up to 1.3x stronger; the bright pass gives at most white per channel. Halo draws (`bloom_halo_draw`: the lamps' halo and light-cone particle quads, SRC_ALPHA/INV_SRC_ALPHA, depth test on, depth write off, FVF 0x142, triangle list; centred on the shade rim, the shade hides their upper half and blooming the rest hung a ball under each lamp) are replayed right after the split (`bloom_order`, in their order, after the last back-buffer clear before it), so the bright pass never sees them and they look as with bloom off; with bloom off the replay order is unchanged. Vulkan 1.0 only (RGBA16F is a required blendable, filterable color format; B8G8R8A8 otherwise). The startup line `[renderer] bloom on (strength S, threshold T)` confirms it. Cost (GPU timestamps around the split, RTX 2060, offscreen): about 0.33 ms per frame at 640x480 and 0.54 ms at 1920x1440 (level51, averaged over 1400 frames; the 3x3-tent composite this replaced cost 0.95 ms at 1920x1440) |
| `--filmic`, `--filmic-strength F`, `--filmic-grain F`, `--filmic-vignette F`, `--filmic-lut FILE.cube` | filmic look of the 3D scene, a 1979-film grade (default off: nothing is created, frames and pipelines exactly as without it, bloom keeps 4 levels; strength 0-2, default 1, 0 = the scene unchanged; grain 0-1, default 0 = off; vignette 0-1, default 0.25, corners about 6% darker; `--no-filmic` turns it off again). The replay calls `renderer_filmic` after the 3D scene's last draw (the last depth-tested back-buffer draw or rain layer, so after the bloom split, lamp halos, wet ground and rain), before the HUD, text and menus; frames without a 3D scene (menus, movies) are untouched. The renderer splits the pass there, copies the resolved scene and redraws every pixel (`shaders/filmic.frag`, alpha kept): in linear light (display ^ 2.2) the light-masked highlights (the bloom light mask in the scene's alpha) are expanded into a pseudo-HDR range up to 1 + strength, then a gentle tonemap (`src/filmic.c`: half Narkowicz's ACES fit, normalized so the headroom's top maps to white and exposure solved so mid-grey 0.18 maps to itself, half the scene's own values; plain white ends at about 0.96 display), a red-orange halation (the bloom glow of level 1, screened; needs `--bloom`), the grade through a 3D LUT (32^3, built at start-up from `filmic_grade`: a crushed-black toe, teal shadows, warm highlights, 0.9 saturation; or a `.cube` file, 2-64 per side, default domain, which replaces it; a bad file logs the reason and keeps the built-in grade), the vignette and the grain. With it bloom has 6 levels (down to 1/64 of the size) and its bright pass feeds the light-masked pixels from the same pseudo-HDR range. The startup line `[renderer] filmic look on (...)` confirms it. Cost (RTX 2060, 1920x1080, MSAA 4, bloom; a synthetic frame rendered 400 times alternating with and without the split, two passes): about +0.2 ms per frame (+0.19 and +0.23 ms); the two extra bloom levels (1/32 and 1/64 of the size) are too small to measure. |
| `--rain-hd`, `--rain-hd-density F` | better rain (default off: no pipeline, frames exactly as without it; density 0.25-4, default 1; `--no-rain-hd` turns it off again). The title's rain (Lua `StartRain`; `OE_Rain` queues it and the particle renderer draws it) is two full-screen quads with a scrolling streak texture; `src/rain.c` recognises them by render state and geometry (FVF 0x142 triangle list of 4 vertices, no depth test, SRC_ALPHA/INV_SRC_ALPHA, every vertex at clip z 0.5, w 1 and outside the screen, one colour, edges along the texture axes; no code addresses) and reads their colour and alpha, tiling, slant (the quad's v edge) and per-frame UV motion (fall and drift, summed while the layer keeps being drawn). With the option on, the replay draws one `renderer_rain` in place of the first layer and skips the layers: four depth slices from 1.5 to 24 view units, far first, each a full-target triangle at the slice's NDC depth (`A - B / d`, A and B fitted per frame from the depth-tested scene draws: clip z = A w - B), depth tested against the scene and not written, so nearer walls and characters hide the farther slices. The fragment shader draws a grid of procedural streaks in the tilted frame (one per cell at most, random place, a motion-blurred tail fading behind a brighter head, coverage-weighted width so sub-pixel streaks fade instead of shimmering); cell size, streak size and scroll speed scale with 1 / distance (near: long, wide, faint and fast, far: fine, dim and slow). Each streak also has its own distance between its slice and the next farther one (`gl_FragDepth`, `depth_greater` so the early depth test stays valid) and is sized by it, so the scene hides streaks one by one instead of in flat layers. Two passes per slice (the nearest: the lit pass only): a little flat colour and the veil (alpha blend), then a lit pass (DST_COLOR / ONE: the scene behind a streak brightened by up to twice), so streaks catch the scene's light, bright near lamps and bloom, faint in the dark. Opacity follows the title's layer alpha (its fade in and out), density follows the layers' tiling times `--rain-hd-density`. The title draws most of its world blended with depth writes and alpha test GREATER 1, so see-through fence, wire and crate texels write depth; in a rain frame each such draw before the rain is replayed with its alpha test raised to GREATER 0x20 (`rain_soft_depth`: texels under an eighth opaque neither write depth nor add their faint colour), so they no longer cut flat rain-free panels out of the slices. No title texture is used. Bloom: the layers have no depth test, so they come after the bloom split and the streaks are never bloomed. The startup line `[renderer] better rain on (density D)` and, at the first rain, `[rain] the title's rain (...) is drawn as procedural rain` confirm it. Splashes and ripples (`--rain-hd-splash`, on by default; `--no-rain-hd-splash`): in a rain frame the replay calls `renderer_rain_splash` right after the end of the opaque world (the SSAO split point; one shared pass split when `--ssao` runs too) with the frame's depth projection, the camera (the view and projection matrices of the last depth-tested fixed-function draw with a perspective projection: particles, sky; the title's world is z-up and its projection right-handed) and the title's clock (`rain_splash_build`). The renderer reads the depth (as SSAO: sampled in place, sample 0 with MSAA) and, per pixel, rebuilds the view position, the normal (depths 2 pixels away) and a flatness test (4-12 pixels, about 5 cm away); only surfaces facing up within about 40 degrees and flat (streets, roofs, crate tops; not walls or the curved tops of characters) go on. This is worked out at the output size (the internal size divided by the supersampling factor) into an R16G16 mask, then laid over the target bilinearly; with `--ssao` it runs inside the SSAO composite's pass. Their world position falls in a grid of ground cells (0.5 units, smaller above one impact per cell and period: about 0.24 at the default rate) each with one chance of an impact per 0.6 s at a random phase, place and size (30 impacts per square unit and second times the streaks' density), so the drops stay fixed in the world and only the title's clock moves them (same speed at 30 and 60 fps). The surface tests (normal, flatness: 8 more depth reads) run only where a drop shows. An impact throws 2-3 droplets up and out for 0.16 s (tiny spheres tested against the pixel's view ray, so they are seen in front of the ground behind them) and, on flat ground, a ring (up to about 0.09 units) widens over 0.5 s with a fainter inner ring; sub-pixel features are widened to a pixel and dimmed; everything fades out between 9 and 22 view units. Two composites: one brightens what lies under them (DST_COLOR / ONE, like the lit streaks: clear near lamps), one adds a little flat colour (ONE / ONE) so they do not vanish on the dark night ground; they sit under the particles, rain streaks and HUD drawn after them. Cost (RTX 2060, Quality preset: 1920x1080 widescreen, MSAA 4, supersample 2, bloom, SSAO, level51 ch3): about +0.4 ms. Covered ground: see the cover map below. The title's own `RainDrops` (Lua `CfgRaindrops`: 1-3 `sub_splash`/`sub_ripple` particle effects per update on its own list of ground triangles near the camera) are left as they are. The shader is the splash mode of `shaders/ssao_comp.frag` (specialization constant 0 = 1). The line `[rain] splashes and ripples after the opaque world (...)` at the first rain confirms it. Wet ground (`--rain-hd-wet`, on by default; `--no-rain-hd-wet`; `--rain-hd-wet-strength F`, 0.1-4, default 1.8, scales reflections and darkening): in a rain frame the replay calls `renderer_rain_wet` right after the frame's last depth-tested back-buffer draw, where the 3D scene ends (after the bloom split and the halos moved after it; before any HUD, text or overlay drawn without depth test, so none of them is reflected, even when the title draws them before its rain layers) (`rain_wet_build`: the depth projection, x/y scale and world up in view space from the same camera as the splashes; reflection gain 2.5 and darkening 0.3 at the title's full rain, at least half that in light rain, times the strength, darkening capped at 0.6). There the colour holds the opaque world plus the effects, lamp halos and bloom (so halos are reflected) and the depth is still the opaque world's (those draws write no depth). The renderer ends the pass, blits the resolved colour to half the output size and, at that size (`shaders/wet.frag`): mode 0 rebuilds each pixel's view position and normal from the depth (as the splashes), keeps surfaces facing up (smoothstep 0.8-0.92 of the normal against up) and flat (the splashes' flatness test at about 10 cm, 4-64 pixels, each axis taking the nearer of its two sides: the curved tops of characters such as shoulders stay dry, the floor at a wall's foot stays wet; this mask drives both the darkening and the reflection), mirrors the view ray about world up and marches it 20 steps (denser near the surface, per-pixel dithered start) through the depth in screen space out to 40 view units, refining a hit in 4 bisection steps; the hit colour is kept by its brightness (smoothstep of luma 0.12-0.7: lamps, windows, signs, lights), weighted by Schlick fresnel (F0 0.04, water) and fades at the screen edges, towards the end of the ray and for rays turning towards the camera; mode 1 blurs it over 3 x 13 texels, mostly vertically (the streaks of wet asphalt), wet texels only; mode 2 composites it bilinearly at the internal size (blend ONE / ONE_MINUS_SRC_ALPHA: dst x (1 - darken x wet) + reflection, rgb only), then the frame's pass resumes and the rain and HUD are drawn over it. A frame without rain (or with `--no-rain-hd-wet`) has no extra pass and is byte-identical. Cost (Quality preset): too small to measure in the frame time. The line `[rain] wet ground where the 3D scene ends (...)` at the first rain confirms it. Fade (`rain_fade_step`, `rain_fade_apply`): the replay keeps a level 0-1 that moves towards 1 while a frame has the title's layers and towards 0 without, by the title's clock (`[0x4aca1c] + 0x40`, read at each frame's first draw; 500 ms end to end, so the same at 30 and 60 fps; a fade-out also ends after 60 frames without layers, in case the clock stops); the last layers and scroll are kept and keep falling at their last speed, and the level scales the streaks (flat colour, lit, veil), the splashes and the wet ground. Without layer ops the fading rain is drawn after the frame's last depth-tested back-buffer draw (after the bloom split when bloom runs; frames without a 3D scene get none). The line `[rain] the title's rain stopped: rain HD fades out (...)` confirms it once. Cover map (`--rain-hd-cover`, on by default; `--no-rain-hd-cover`): in a rain frame the replay builds `RRainCover` (`rain_cover_build`: from the same camera as the splashes, a top-down orthographic map of 48 x 48 world units centred on the camera and snapped to its texels, 40 units below the camera to 60 above, world z-up) and calls `renderer_rain_cover`; the opaque world draws (the SSAO set: depth-tested, depth-writing back-buffer draws after the frame's last depth clear, so not the sky with its own projection) are flagged `RENDERER_RAIN_COVER`. Before the main pass the renderer draws those draws once more, depth only, into a 512 x 512 D16 map (`shaders/rain_cover.vert`: each vertex's clip x, y, w to the map by three affine rows, no fragment shader, no culling, depth clamp; the clip z is not used, so the depth row of any projection works). The RAIN_COVER variants of `rain.frag`, the splash mask (`ssao_comp.frag` mode 1) and the wet trace (`wet.frag` mode 0) rebuild each point's view position (a streak at its own distance, the veil at its slice's, the ground from the depth), take it to the map (`shaders/rain_cover.glsl`, one `textureGather`) and leave it out where the map's top surface lies more than 0.3 units above it (smoothstep to 0.7, bilinear over the four texels: soft edges about a texel, 0.09 units, wide). Characters are not part of the map (below); props are, so the ground right under a parked car stays dry. Without a map in the frame (no rain, or the flag off) the shaders read nothing (`--no-rain-hd-cover` keeps the original pipelines). The line `[rain] cover from above: N opaque world draws (...)` confirms it once. Character mask (with splashes or wet ground): a draw made by a character's vertex program (`VShader.character`: the skinned declaration, bone indices and weights in stream 1, or a program that reads a bone palette, `vsh_bone_palette`: an `arl` and constants read at a0.x, the title's vsCharacter) in the frame's 3D scene (after its last depth clear, up to the end of the opaque world) is flagged `RENDERER_CHARACTER` and never `RENDERER_RAIN_COVER`. Before the main pass the renderer draws those draws once more, depth only, into a depth image of the internal size (`shaders/char_mask.vert`, the main pass's transform and viewports; cleared to 1 also in a frame without characters; no work in frames without splashes or wet ground, nor without `--rain-hd`). The splash mask (CHAR_MASK variant of `ssao_comp.frag`) and `wet.frag` read it (`is_char` in `ssao_common.glsl`: the pixel's visible depth is the character's within 0.02 + 1% view units): characters get no splashes, no darkening and no reflection (the composite also leaves character texels out of its bilinear filter, so the ground stays wet up to the silhouette), and the normal and flatness tests ignore samples on characters (the opposite side counts instead), so the floor at someone's feet is no longer taken for a step and left dry. Characters still appear in the reflections. The line `[rain] character mask: N character draws kept dry (...)` confirms it once. Cost (Quality preset): about +0.7-0.8 ms. |
| `--sky-hd` | enhanced sky (default off: no pipeline, operation or vertex change, frames exactly as without it; `--no-sky-hd` turns it off again). `src/sky.c` finds the frame's sky stage, the back-buffer draws between its colour clear and the depth-only clear before the world: the dome (first draw: fixed-function FVF 0x152, no depth write, camera-centred, near plane under 0.2, a DXT1 texture twice as high as wide and at least 64 wide), the cloud cap (the next draw if blended on the same camera), the skyline backdrop (depth writes, near plane 10-100, an atlas of at most 32 texels) and the far lamp glows (blended emissive draws after the depth clear). The dome and cap are replaced by one full-target pass (`renderer_sky`, `shaders/sky.frag`) that rebuilds each pixel's world view direction from the dome's camera: the level's colour ramp (trimmed row means of its own dome texture at load, scaled by the dome's lit vertex colour), a distant city band hashed per azimuth cell (no 40-degree repeat), stars one pixel wide at any resolution (as many as the dome's painted stars; a dark zenith gets some), value-noise clouds drifting with the title's clock (more with a cap, overcast while the title's rain layers are drawn), and a moon opposite the shadow direction `c_vSunDir` (c90). The skyline draw keeps world positions in its unused stage-2 coordinates and is drawn again over itself (`renderer_skyline`, `shaders/skyline.frag`): windows per building on its facades (lit share, warm or cool light, a rare flicker), its painted windows lit or dark per window, the light marked in the bloom mask, and haze toward the horizon colour by distance. |
| `--sharp-fonts`, `--no-sharp-fonts` | sharp fonts (default on; `--no-sharp-fonts`: the atlases as shipped). `src/sharp_font.c` upscales the two font atlases, the main font/HUD sheet `part_page0` (content hash 886a4c14094e9597, 512x512) and the big font (912d0ecc2ada0599, 1024x512), 4x from the guest's own decoded level 0 the first time each is drawn, on a worker thread (the original is drawn meanwhile, cached under the texture-pack fallback key); the result and its mip chain take the guest texture's key, so the title's normalized UV rects sample the same cells. Letter ink (near-white texels whose alpha changes by at least ~0.5-0.65 over two texels) gets a Catmull-Rom alpha cut at half the local ink level with a ramp one output texel wide; every other texel (button icons, portraits, markers, soft glows) gets exactly the bilinear value the GPU samples from the original. A texture pack that replaces an atlas wins. With `--button-icons kbm|auto` the key caps are painted on the sharp main atlas at 4x. The main atlas's chain is kept in memory (re-upload, key caps); the big font's is freed after the upload and rebuilt if the renderer drops its cache. Nothing is written to disk. |
| `--neon-glow`, `--neon-glow-strength F`, `--neon-glow-bloom F`, `--neon-glow-flicker F` | emissive signs (default off: no hashing beyond the packs', no pipeline or operation change, frames exactly as without it; `--no-neon-glow` turns it off again). `src/neon_glow.c` indexes the emission masks of the `--texture-pack` folders (`<hash>.emit.png` grey, red channel; `<hash>.emit.dds` BC4 UNORM, level 0; first pack wins, the `.dds` within a pack; a mask-only pack works), reads each into an L8 mip chain on a worker thread the first time its texture is drawn (uploaded at most 2 per frame) and gives each sign a deterministic flicker (frame count; 0-0.3, default 0.04). `src/hle_d3d.c` binds the mask of the first stage whose content hash has one (frame-target world draws, also those whose vertex program runs on the device with `--gpu-vertex-programs`; not characters or movies) with `renderer_set_emission`; the renderer draws those with the emissive variant of `geom.frag` (`-DEMIT`, shape bit 31, set 1 = the mask, the stage in the emission push words; a world-lit draw uses the lit emissive variant, `-DCHAR_LIT -DEMIT`: set 1 the lights, set 2 the mask, push word 3 still the light mask, so a sign is lit first and then glows): after the final combiner (the fog mix) and any lighting with e = stage colour x mask x strength (at most 1) the colour becomes b + e/2 x (1 - b), b = max(colour, e) (at least its own colour through fog, hotter than paint), and an opaque draw before the bloom split writes mask x bloom (0-1, default 1) into the bloom light mask instead of 0. Strength 0.1-3, default 1.25. Masks are made by each user from their own copy (`docs/TEXTURE_PACKS.md`, section 7); none ship. |
| `--normal-maps`, `--normal-strength F`, `--specular-strength F`, `--normal-baked F` | per-pixel relief and specular from companion maps (default off: no index, no lookup beyond one test per draw, no pipeline or operation change; the off frames' shaders are byte-identical SPIR-V to before; `--no-normal-maps` turns it off again; strengths 0-4, default 1; baked 0-1, default 1). `src/normal_maps.c` indexes `<hash>_n.png/.dds` (tangent-space normal map, OpenGL convention: +Y = green up the image) and `<hash>_s.png/.dds` (R specular intensity, G gloss) in the `--texture-pack` folders (first pack wins; the `.dds` within a pack while the device samples BC blocks, a later pack's `.png` backing a `.dds`-only hash; companion-only packs work over the original textures) and logs `[normal-maps] N textures with relief, M with specular`. The texture pack loader threads read them like replacements (`texpack_load_image`, extra load-queue kinds), requested and prefetched with their texture; until they are uploaded (and while the texture's replacement is not), the draw is drawn without them. `src/hle_d3d.c` binds the maps of the first stage whose content hash has one, for draws lit per pixel only (`--char-lighting` characters, skinned or not; `--world-lighting` world draws), with `renderer_set_relief`; the renderer draws those with the lit relief variant of `geom.frag` (`-DCHAR_LIT -DRELIEF`, shape bit 30, the lit emissive pipeline layout: set 2 = the two maps, the push words after the light mask = normal strength, specular strength, stage + 4 x flags, baked weight; an emissive draw keeps the emissive variant and no relief). The vertices have no tangents: the tangent frame is the cotangent frame from the screen-space derivatives of the interpolated view position and of the stage's (already transformed) coordinates, computed at the top of the shader in uniform control flow (before any discard: alpha-tested draws and MSAA are fine), its orientation fixed by the sign of the screen-space area against the normal (Vulkan's y-down window, mirrored mappings), +Y mapped to -dP/dv (the title's v grows down the image). The tilted normal replaces the smooth mesh normal in `char_light()` and `world_light()`; a Blinn-Phong term of the same lights (exponent 8-512 from the gloss, energy-normalised, through the lamp pass's soft knee, x the character or world gain) x intensity x specular strength is added after the final combiner and faded by the fog; the vertex (baked) colour, which has no direction, is scaled by mix(1, (1 + cos) / 2, baked) with cos between the tilted and the mesh normal (light from the normal's hemisphere). The screen-space `--lamp-lights` pass (normals from depth) and unlit draws do not use the maps. Tested by `tests/normal_maps_test.c` (options, names, index) and `tools/renderer_test.c` `test_relief` (flat map unchanged, +X / +Y tilt directions, highlights, baked term, plain draws untouched). |
| `--sharp-faces` | character skins at 2x (default off; `--no-sharp-faces` turns it off again). `src/sharp_faces.c` picks stage 0 of draws by a character's vertex program whose texture is DXT1 with power-of-two sides from 64 to 512 and comes from the guest (a texture pack or built-in replacement for that hash wins). The skin's level 0 is decoded and filtered on one worker thread: DXT1 seams between flat sides are smoothed, a 2x Catmull-Rom result is clamped to the 2x2 source texels around each output (no ringing), then a mild unsharp mask clamped to the local 3x3 range; binary alpha stays binary. Until the result (with its mip chain, B8G8R8A8) is uploaded the draw samples the guest texture while the upscale has its own cache key (so a skin already cached for another draw is still upgraded); at most one upload per frame. Nothing game-derived ships: each copy of the game is filtered as it plays. |
| `--soft-particles`, `--soft-particles-distance F` | soft particles (opt-in: the launcher offers them on its Display page and no preset turns them on) (default off: no new operation, pipeline or render pass, frames exactly as without it; distance 0.05-4 world units, default 0.5; `--no-soft-particles` turns it off again). The title's world particles (`src/soft_particles.c`, `soft_particle_draw`: fixed-function FVF 0x142 with 24-byte vertices, an indexed triangle list, back buffer, depth test on, depth write off, SRC_ALPHA or ONE onto ONE_MINUS_SRC_ALPHA or ONE: smoke, dust, fire, sparks, lamp halos and cones, haze; the glow/overlay pass has no depth test and never matches) are CPU-built quads. After the end of the opaque world (the SSAO split point) the replay calls `renderer_soft` with the frame's depth projection (as for `--ssao`) and replays each particle draw with its quads classified (`soft_classify_sprites`): a quad in the view plane (normal within about 1.8 degrees of the view axis: the title's billboards are built there; world-fixed quads that merely face the camera, such as the graffiti tags painted on walls, would fade into the wall they lie on) and not lying within 30 degrees of horizontal in the world (ground decals, the rain's ripples), whose sprite is not world-fixed, is flagged `RENDERER_SOFT`. A sprite is the draw's stage-0 texture and the quad's uv rectangle (to 1/4096): the title draws its world particles from one atlas, but each sprite is either always a view-plane billboard or always fixed in the world (level87 with the tag pokes: 56 billboard sprites, 345k quads, all within 0.8 degrees; 6 world-fixed sprites, 393k quads, none of them ever in the view plane; the three tag layers seen at 10-13.5 degrees under the tag camera). Once a quad of a sprite is seen off the view plane and not lying level, the sprite is world-fixed for the session (`SoftSprites`, up to 3072), so a graffiti tag seen within 1.8 degrees of head-on keeps its paint too; runs of soft and other quads go out as separate draws in their recorded order, so the back-to-front blending is unchanged. The renderer ends the pass there (sharing the split with SSAO and the rain splashes when they run) and resumes it with a render pass variant whose subpass holds the depth in `DEPTH_STENCIL_READ_ONLY_OPTIMAL`, so the soft pipelines (`shaders/geom.frag` built with `SOFT`, `MSAA`: sample 0 of the multisampled depth) test against the depth and read it in place with no copy; every later split (bloom, glass, wet ground) resumes with the same variant. Each soft fragment is faded by saturate((scene view depth - fragment view depth) / distance): alpha only for SRC_ALPHA blends, colour and alpha for ONE (additive) blends; where nothing was drawn (depth 1) there is no fade. A depth clear or depth-writing draw after the split cancels it for the frame. Vulkan 1.0: needs the depth format sampled at the MSAA sample count; otherwise `[renderer] soft particles unavailable on this device`. The startup line `[renderer] soft particles on (distance D)`, `[soft] soft particles after the opaque world (...)` and `[soft] N of M particle quads soft (view-plane billboards), K world-fixed sprites` confirm it. Cost (RTX 2060, Quality preset, level51, SSAO sharing the split): about +0.08 ms per frame (at most +0.2 ms). |
| `--ssao`, `--ssao-strength F`, `--ssao-radius F` | screen-space ambient occlusion of the opaque world (default off: the depth target, frames and pipelines exactly as without it; strength 0.1-2, default 0.8; radius 0.1-8 world units, default 0.8; `--no-ssao` turns it off again). The opaque world ends at the frame's last depth-tested, depth-writing back-buffer draw after its last depth clear (`src/ssao.c`, `ssao_split`; measured in the hideout, level51 and its in-engine cutscene: the title draws its whole opaque world in one run, blending enabled but depth written, then the depth-write-off particles, smoke, fire, lamp halos and cones, then the HUD, text and rain layers without depth test); there the replay calls `renderer_ssao` with the frame's depth projection (clip z = A w - B, fitted per frame from the depth-tested draws, as for `--rain-hd`) and the camera's x/y scale (read from the world-view-projection constants of the last opaque draw's vertex program, or the fixed-function projection). The renderer splits its pass (as bloom does), samples the depth target (single-sample or sample 0 of the multisampled one) at half the output size (`ssao_work_size`: 2 x the supersampling factor internal pixels per texel, so `--supersample 2` does not quadruple its cost), rebuilds view positions and normals from it, takes 12 taps on a disc of the radius around each point (golden-angle spiral rotated per pixel), counts nearer surfaces within the radius by the cosine to the normal less a bias, fading with distance (a character far in front of a wall casts no halo on it), blurs the result twice (9 taps, depth-aware), upsamples it with the full-size depth (the four nearest texels, those from another surface left out; without one on this surface, the closest depth among the 4 x 4 around it, so silhouettes get no halo) and multiplies 1 - strength x occlusion onto the color (rgb only: the bloom light mask in alpha is kept; the far plane and unoccluded pixels keep their exact color). Lights are spared: the light mask in the scene's alpha (set by the opaque world's additive draws, neon, fire, flares; kept with and without `--bloom`) fades the occlusion out (none from a mask of 0.5 up), per pixel in the composite with `--msaa` (read from the resolved target), per texel in the last blur pass without it. Bloom, rain, the HUD and everything else after the split are drawn over it unchanged. Vulkan 1.0: needs the depth format sampled at the MSAA sample count (`sampledImageDepthSampleCounts`) and R16G16_SFLOAT (required); otherwise `[renderer] ambient occlusion unavailable on this device` and frames are drawn without it. The startup line `[renderer] ambient occlusion on (strength S, radius R)` and, at the first 3D frame, `[ssao] occlusion after the opaque world (...)` confirm it. Cost (RTX 2060, offscreen, hideout): about 0.32 ms per frame at 640x480, 1.0 ms at 1920x1080 with MSAA 4, about 2 ms with supersample 2 on top (3840x2160 internal) before the half-output cap; Quality preset, 13000-frame headless run (`--stats` GPU wait against `--no-ssao`, mean of 113 windows of 3D frames): +2.5 ms before the cap, +2.1 ms after it (the rest is the pass split with MSAA and the composite at the internal size) |
| `--lamp-lights`, `--lamp-lights-strength F` | lamps, fire and neon light the opaque world around them (default off: no new operation, resource or pipeline, frames exactly as without it; strength 0.1-3, default 1; `--no-lamp-lights` turns it off again; on in the launcher's Quality preset). The title has no dynamic light in its world (flat ambient plus baked vertex colour); its lamps are halo particle quads (the `bloom_halo_draw` signature: FVF 0x142, SRC_ALPHA / INV_SRC_ALPHA, depth test on, depth write off). While recording, every quad of such a draw (`src/lamp_lights.c`, `lamp_sprite_quad`) becomes a light sprite when it is bright (vertex alpha x brightest channel at least 0.45, brightest channel at least 0.6) and at least 0.3 world units wide: lamp halos, light cones and white glows; sparks, blood, dust, smoke and haze plumes in the same draws are not (measured in level51 and the hideout). At replay the sprites of one lamp are merged (`lamp_cluster`) and followed across frames by world position (`lamp_track_update`): each light keeps its id, fades in and out over 8 frames, stays lit for up to 300 frames while its lamp is off screen (the title draws a halo only for lamps on screen), and the 32 of highest priority (weight x radius^2 / (distance^2 + radius^2), x1.5 for a light already shown) are passed to `renderer_lamp_lights` right before the SSAO split (they share it), in the camera's view space, with radius 4 x the sprite size (1-10 units) and intensity at most 1. The renderer reads the depth at the SSAO working size (half the output), rebuilds each pixel's view position and normal (`shaders/lamp.frag`), adds colour x intensity x strength x falloff ((1 - x^2)^2 / (1 + 4 x^2), x = distance / radius) x a wrapped Lambert term (wrap 0.35), tests the 4 strongest lights with a 6-step screen-space shadow ray, puts the sum through a soft knee (L / (1 + L / 2), so overlapping lamps do not wash out), gives characters (the rain's character mask) 1.35 x as much and pixels of the bloom light mask less (none from 0.5 up), then upsamples it with the full-size depth and applies dst x (1 + light) (rgb only). Effects, halos, rain and the HUD are drawn after it unlit. Vulkan 1.0: needs the depth format sampled at the MSAA sample count and R16G16B16A16_SFLOAT (required); otherwise `[renderer] lamp lights unavailable on this device`. The startup line `[renderer] lamp lights on (strength S)` and, at the first lit frame, `[lamp] N lights from M halo quads (...)` confirm it. Cost (RTX 2060, Quality preset, GPU timestamps around the light pass and composite; SSAO on, so no extra pass split): 0.23-0.49 ms per frame (level51, hideout with up to 14 lights, the mission-1 script). |
| `--world-lighting`, `--world-lighting-strength F` | the lamp lights light the world (walls, floors, buildings) and props per pixel (default off: no new resource, pipeline or per-draw work, frames exactly as without it; strength 0.1-3, default 1; `--no-world-lighting` turns it off again; on in the launcher's Quality preset). The world's light is baked: the title's world programs (vsWorldTransform + vsWorldLight0/3/7 + vsWorldFinal / vsWorldFinal2Pass) write oD0 = r7 x c93 + c94 + v2, the baked vertex colour v2 plus material-scaled vertex lights and the ambient. `world_light_program` (`src/world_light.c`) tags a program at CreateVertexShader as world when oPos.x-w are dp4 of the raw v0 with c69-c72, an add or mad that the input v2 reaches writes oD0.rgb and nothing is read relative to a0, and as object when it also reads c62-c64 (vsObjectShadow*); the startup lines `[world-light] vertex program N (...): world / object` list them. While recording, each draw of such a program into the 3D scene (depth tested; opaque or a SRC_ALPHA / INV_SRC_ALPHA decal: additive and multiplying layers are not lit) gets view normals from its raw v1 through its own rows (c69 / px, c70 / py, -c72, renormalised, not welded: building corners stay sharp) and its view-space box (`world_light_record`). At replay the lamp lights (the same list as `--lamp-lights`, built for this option alone too) that reach the box (`world_light_mask`, at most 32) go into push constant word 3 of the character-lighting pipeline (`shaders/geom.frag` with CHAR_LIT, `shaders/world_lit.glsl`): per fragment colour x intensity x strength x falloff x a wrapped Lambert term (wrap 0.15), the soft knee, then the vertex colour v0 becomes v0 + L (1 - v0) before the combiners, so the texture, the combiners and the final combiner's fog mix apply to the lit colour (dark streets gain the most, baked pools do not wash out). A draw no light reaches is drawn plain. With `--lamp-lights` the lamp pass then lights only the characters (it needs the character mask, as `--char-lighting`; without it world lighting stays off that frame) and is skipped when `--char-lighting` lights them. Lights are not occluded: a lamp lights a wall's far side too. The startup line `[renderer] world lighting on (...)` and `[world-light] world draws lit per pixel by the lamp lights (...)` at the first lit frame confirm it. Cost (RTX 2060, Quality-like options at 1920x1080 MSAA 4, the hideout with 6-9 lights and ~400 lit draws, the same frame re-rendered 25 times each way with and without it; the GPU was shared with other runs): +0.56 and -0.36 ms at two frames, within the noise (frames without any lit draw spread as much): the per-pixel light costs about what the lamp pass it replaces did. CPU: `world_light_record` takes about 0.17 ms per 30000 world vertices (i386 build, -O2; the hideout records 28000-38000 per frame). |
| `--fire-light`, `--fire-light-strength F` | the burning fire barrels light the walls, floor and people near them with a warm, gently flickering light (default off: no fire code runs, frames exactly as without it; strength 0.1-3, default 1; `--no-fire-light` turns it off again; on in the launcher's Quality preset). The title's fires are flame sprites drawn with the lamp halos' render state (`bloom_halo_draw`); a halo draw whose stage-0 texture is one of the two flame sheets (`fire_light_texture`, `src/fire_light.c`: content hashes `FIRE_TEX_FLAMES`, the barrels' 6 x 6 flame grid, and `FIRE_TEX_FLAME_STRIP`, the taller fires' strip) gives fire sprites (`fire_sprite_quad`: centre, size, mean alpha) instead of lamp sprites, so a fire is no longer also a steady white lamp. At replay the sprites within 1.2 m are merged per fire (`fire_cluster`), a second lamp tracker (`lamp_track_update`: stable ids, fades, held out of view) keeps the 8 nearest the camera, and `fire_light_shade` gives each about 2000 K (1, 0.47, 0.14), a 4.5 m radius and intensity 0.9 x fade, all three flickering with smooth value noise (1.3, 3.1 and 7.3 Hz octaves, at most 35% dimmer; position sway of 0.12 m) from the guest clock (frame-rate independent: the same at 30 and 60 Hz; still while paused). They are appended to the frame's lamp lights (`frame_lamp`; the lamps give way when both exceed 32), so `--world-lighting` and `--char-lighting` light with them through the per-draw light mask and the soft knee (no renderer, shader or push-constant change), and `--lamp-lights` through its pass. Without one of those three it does nothing (the startup line says so). `[renderer] fire light on (...)`, `[fire-light] N fire lights from M flame quads` at the first lit frame and, with `--stats`, a `[fire-light]` line every 600 frames confirm it. Cost: no new pass, pipeline or resource; the GPU work is the fire lights' share of the per-fragment light loop of the lit draws whose box a fire reaches (at most 8 more lights). Measured on the RTX 2060 (shared with other runs; level51, 1280x720 MSAA 2, the same recorded frame rendered 10-20 times each way with and without the fire lights, 19 frames with lit fires): +0.03 to +0.08 ms on average (standard error 0.09-0.18 ms: the shared GPU spreads single frames by about 0.6 ms). Host: 2-4 us per frame for the fire part of the light list (`--stats`), plus one memoized texture-hash lookup per halo draw. Known limits: no occlusion (as the lamp lights); the title's cutscenes draw their world with other vertex programs, so there the fire lights the characters (and, with `--lamp-lights`, the ground through the lamp pass) but not the walls; each barrel's flicker is its own but not tied to the flame sprite's animation. |
| `--lamp-shadows`, `--lamp-shadows-strength F` | characters near a lit street lamp or burning barrel cast a ground shadow pointing away from that light (default off: no lamp-shadow buffer, the shadow word and every pipeline as without it; strength 0.1-1, default 0.6; `--no-lamp-shadows` turns it off again; on in the launcher's Quality preset). The title draws one character shadow, from one light above: a depth map of the characters seen from it, sampled by every world draw. The lights are those of `--lamp-lights` / `--fire-light` (`frame_lamp`: the halo and flame quads clustered and tracked, also built for this option alone). Each frame the HLE takes the depth stage's projection (`c_mTexPrj`, c65-c68, with the stage's linear-texture scale) from the first world-program receiver, and `lamp_shadow_setup` (`src/lamp_shadow.c`) inverts it, derives the light projection r = A q - B and keeps the 8 lights nearest the camera, each also in the stage's coordinates; `renderer_lamp_shadows` puts them in a storage buffer at set 0, binding 5, read by receivers through shadow word bit 14. Per receiver fragment (`geom.frag` `lamp_shadow`) the stage coordinates give the world position; a light less than 0.5-1 unit above the fragment (a light pool sprite on the ground) casts none. For each light in reach a ray from the fragment towards the light (from 0.25 to at most 4 world units away horizontally, ending 2.2 units above the fragment) is marched in 12-32 steps at most 0.12 units apart, each pixel offsetting its points within the steps differently (interleaved gradient noise: the edges dither, smoothed by supersampling, instead of showing a chain of one caster outline per step), stopping once fully blocked, through the same shadow map as a height field: a ray point less than 2.2 view units behind a stored character depth is inside that character (bilinear over the four texels). One lamp per character, chosen at the character: where the ray meets a caster, every light is rated by its brightness (weight x the `--lamp-lights` falloff) at the caster's middle (its stored top, 0.9 units lower); lights below 0.95 of the brightest there get no share and the shares of the others add up to 1, and the ray's occlusion is scaled by its light's share (rated once per ray, at the first caster it meets; lights below 0.15-0.3 of the brightest at the fragment are not marched, a cost gate: a character's lamp within a shadow's length lights the fragment too). So a character's whole shadow points away from the lamp nearest it, one shadow per character however many lamps are in reach, crossfading (each lamp at half strength exactly between two equal ones) as it walks from one lamp to the next. Choosing the light per ground point instead (the first versions) gave each part of the ground around a character its own lamp, so a character among several lamps got several shadows. The vertex diffuse colour (after `--world-lighting`, before the combiners and the fog) darkens by strength x the largest weighted blocked share (overlapping shadows do not stack); the occlusion fades from 2.4 to 4 units away from the character, each light's weight falls off from half its radius to its radius (so shadows crossfade from one lamp to the next), and where the title's own compare already shadows the pixel only half the darkening applies. `[renderer] lamp shadows on (...)`, `[lamp-shadows] light projection from c65-c68 ...` (with the check against the program's own coordinates) and `[lamp-shadows] N of M lights cast character shadows` confirm it. Cost of the per-caster lamp and the per-pixel offsets (hideout, the launcher's Quality flags at 1920x1080, MSAA 4, supersample 2, shadow-scale 4, RTX 2060 shared, `[level-summary]` GPU time of the level): 22.1 ms/frame before (two runs), 30.1 ms after (26.0 without the offsets, 28.4 with a 0.4-0.6 gate that cut a character's own shadow again: rejected); roughly a third of that on an RTX 4070-class GPU. Cost: no pass or pipeline beyond the shadow word bit; per receiver fragment one 4x4 transform, and per light in reach up to 12 four-texel gathers. Measured on an RTX 2060 shared with other runs (hideout, 1280x720 MSAA 2, up to 7 lights, two 5600-frame runs each way, `[level-summary]` GPU time): 3.7 / 4.1 ms off, 4.3 / 3.7 ms on, so below the run-to-run spread (about 0.5 ms); estimated 0.2-0.6 ms at 1080p on a mid-range GPU, more with many lamps in view. Known limits: only characters (and the title's other casters) cast; the height field is the title's light's view, which is tilted about 15 degrees from vertical, so the shadow's base can be offset by up to ~0.5 units from the feet; the title's cutscenes draw their world with other programs (no lamp shadows there unless a world program draws first). |
| `--light-shafts`, `--light-shafts-strength F`, `--light-shafts-cheap` | street lamps hang a visible cone of scattered light in the air below them, burning barrels (with `--fire-light`) a glow above them, thicker while the title's rain falls; depth-correct (the visible surface ends each ray), dithered; half-size pass, quarter-size with `--light-shafts-cheap` (default off: no new operation, resource or pipeline) |
| `--contact-shadows`, `--contact-shadows-strength F` | contact shadows under people and props (default off: no new operation, resource or pipeline, frames exactly as without it; strength 0.1-2, default 0.6; `--no-contact-shadows` turns it off again; on in the launcher's Quality preset). At the end of the opaque world (the SSAO split point) the replay calls `renderer_contact` with the frame's depth projection and world up in view space (`contact_build` in `src/ssao.c`, from the same camera as the rain splashes), right before the lamp lights and SSAO, so they share one pass split (without them it splits on its own). At half the output size `shaders/contact.frag` (mode 0) keeps pixels facing up (smoothstep 0.6-0.8 of the depth normal against up) that are not characters (the rain's character mask, which the replay now also builds for contact shadows), sends seven rays (one straight up, six tilted about 40 degrees) 0.6 view units up from the surface in six steps and counts a ray occluded when a surface lies in front of a ray point by 2 cm + 0.4% of the distance up to 0.6 units (nearer hits count more); it fades out between 20 and 35 view units. Mode 1 multiplies 1 - strength x occlusion (at most 0.9) bilinearly onto the colour at the internal size, discarding character pixels and unoccluded ones. Effects, halos, rain and the HUD are drawn after it. Cost (RTX 2060, Quality preset at 1920x1080 MSAA 4, the same frame re-rendered 7-9 times with and without it, medians): +0.33-0.43 ms in level51 ch3, +0.38-0.73 ms in level81. The startup line `[renderer] contact shadows on (strength S)` and `[contact] contact shadows after the opaque world (...)` confirm it. |
| `--rain-hd-puddles` | puddles on the wet ground (default off; with `--rain-hd` and its wet ground; `--no-rain-hd-puddles`; on in the launcher's Balanced and Quality presets; about +0.05 ms at 1080p). `rain_puddle_build` adds the title's clock and the camera in the world to `RRainWet` (`puddles` > 0); with 0 the wet pass's push constants and shader paths are exactly as before. In `shaders/wet.frag` mode 0 the wet mask (which already holds the cover map's `rain_open`, so never under a roof) is multiplied by a world-anchored puddle mask: two octaves of value noise on world x, y plus, with the cover map, low spots (how far the map's surface lies above the point 0.6 units around it, up to 0.25 units each: kerbs, gutters, wall feet). In a puddle the mirrored ray uses a normal tilted by ripple rings (a 0.35-unit grid, one drop per cell per 0.9 s, rings to 0.14 units, within 16 view units), reflects 70% more of the non-bright scene, and its share is passed in alpha above 1; mode 1 blurs puddle texels up to 85% less (sharper reflections) and mode 2 darkens them up to 1.6x the wet darkening (at most 0.95). The rain fade scales them. `[rain] puddles on the wet ground (...)` confirms it. |
| `--msaa N` | N samples per pixel (2, 4, 8; the largest the GPU supports up to N), resolved before present; alpha-tested cutouts are not antialiased (no alpha-to-coverage) |
| `--aniso N` | anisotropic texture filtering up to N (clamped to the GPU limit; default off) |
| `--no-render-targets` | skip the title's own render-target passes (the character shadows) |
| `--shadow-scale N` | character shadow maps (the title's 1024x512 shadow render target, created at that size times the render height / 480) at N x that size (1-4, default 1): sharper shadow edges. Memory: a D16 map, 2 bytes per texel (1440x1080 output: 2304x1152 = 5 MiB at 1, 21 MiB at 2, 85 MiB at 4; clamped to the GPU's image limit) |
| `--shadow-soft` | soft character shadow edges: the shadow compare is filtered over 3x3 bilinear taps one texel apart (a 4x4-texel tent) instead of one 2x2 tap; the blur width is in shadow-map texels, so it narrows as `--shadow-scale` grows. Measured with `--shadow-scale 2 --shadow-soft` (1440x1080, RTX 2060, see `--supersample`): GPU time 3.3 -> about 4.5-5 ms per frame; `--shadow-scale 4 --shadow-soft` 5.5 ms |
| `--shadow-range`, `--shadow-range-dist F` | character shadows stop short of floors far below the caster (default off: the title's shadows, frames and pipelines exactly as without it; distance 0.5-50 world units, default 3; `--no-shadow-range` turns it off again). The title's shadow map holds only the characters, seen from a light above, and every world draw is a receiver, so a character on the hideout walkway also shadows the floor far below it. With this option the depth-stage compare (`geom.frag` `shadow_tap`, also under `--shadow-soft`) converts the receiver depth and each stored caster depth to the light's view depth and fades a failed compare to lit between F and 1.5 x F behind the caster (smoothstep). The light projection comes from the draws themselves: the depth stage's coordinates satisfy r = A q - B for a perspective light, fitted once per frame in the HLE (`src/shadow_range.c`) and passed per draw (`RPixelShader.shadow_depth`); a frame without a fit keeps the last one, and before any fit (or with an orthographic light) the compare is the original. F is measured from the top of the caster (the light looks down), so it must exceed a character's height plus a jump: the default 3 (a character is about 1.8 tall) keeps the shadow of standing, jumping and knocked-down characters on the ground under them. The startup line `[renderer] shadow range F (...)` confirms it. Cost: one vector divide per shadow tap, no pass or pipeline beyond a specialization constant |
| `--widescreen` | report a 16:9 TV to the title (the `XC_VIDEO_FLAGS_WIDESCREEN` setting `XGetVideoFlags` reads): it widens its camera field of view and HUD layout for anamorphic frames, which are presented 16:9 |
| `--movie-hd`, `--no-movie-hd` | play the disc's 1280x720 movies (default on): opening `bik\<name>.bik` (or `_w`, `_pal`) opens `<name>_hd.bik` from the same folder when it exists and its Bink header is 16:9 HD (every movie except `trailer`, whose `_hd` file is 640x480 and keeps the requested file). The frame is drawn by the renderer at the output resolution (the title's own YUV program on the GPU, bilinear luma and chroma), fitted to 16:9: letterboxed at 4:3, the whole frame with `--widescreen`. Without a renderer (`--renderer none`) the CPU quad draws it, bilinear. Bink audio is unchanged. `--no-movie-hd`: the 640x480 files drawn as before |
| `--gamma-ramp`, `--no-gamma-ramp` | apply the title's own gamma ramp (default on): the HLE'd `D3DDevice_SetGammaRamp` (0x361110, Flags + 768-byte D3DGAMMARAMP: red, green, blue, one byte per entry) hands the latest ramp to the renderer, which runs it as a per-channel lookup on the finished frame, after every post effect and before the window scaling and the frame counter (as the console's scanout); an identity ramp skips the pass (bit-identical frames). `--no-gamma-ramp`: the ramps are ignored, frames as rendered |
| `--language en\|es\|fr\|de\|it` | the console language reported to the title (`XC_LANGUAGE`, read by `XGetLanguage`; default `en`): it loads that language's string tables (`config_strings_<code>`) for menu and HUD text (cutscene subtitles not yet checked); voices are not translated. Any other value exits with the list of valid codes |
| `--present auto\|mailbox\|fifo\|immediate\|readback` | `auto` (default): a Vulkan swapchain in the SDL window (mailbox when the driver has it, else fifo), the frame is blitted (scaled, pillarboxed) into the swapchain image with no CPU readback; the others force that present mode, or `readback`, the read-back frame shown through SDL's renderer. Readback is also used with `--video x11\|null`, `--dump-frame`, a remote `DISPLAY`, and whenever the surface or swapchain cannot be made (logged as `[video] present: ...`). Known issue: the NVIDIA Linux driver crashes inside `vkGetPhysicalDeviceSurfaceSupportKHR` on an X server that is not NVIDIA's (Xvfb); use `readback` there |
| `--frames-in-flight N` | swapchain path (`--present auto\|mailbox\|fifo\|immediate`): how many frames the GPU may still be executing while the host records the next one (1-3, default 2). The host waits for a frame's fence only when its resources (command buffer, descriptor pool, geometry, program and base-image buffers) come round again N frames later, so command recording overlaps GPU execution; `1` is the old behavior (submit, present, wait). Texture uploads (once per frame, batched) and new render targets still wait for the GPU. The readback path (`--present readback`, `--video x11\|null`, `--dump-frame`) always waits for its frame, so its output does not depend on N. The startup line `[video] present: Vulkan swapchain ... <mode>, ... images (no readback), N frames in flight` shows the present mode and N in use; `--stats` reports the fence wait as GPU wait |
| `--gpu-vertex-programs` / `--no-gpu-vertex-programs` | on by default (with `--gpu-skinning`): the world and object vertex programs on `gsk_world_program_allowed` (`src/gpu_skin.c`, the 8 programs of the hideout and mission 8, each checked with `XB_GSK_CHECK`) run in the GPU-skinning compute pre-pass instead of the CPU interpreter. The CPU keeps what its own passes read of those draws: `frame_depth_fit` gets the first 256 clip positions of each depth-tested back-buffer draw from the program's oPos slice (`vsh_slice`; bit-identical to the whole program, checked by `XB_GSK_CHECK`), and a draw sampling a depth stage stays on the CPU until the frame's `--shadow-range` fit is done. Same-frame A/B (`XB_GSK_AB`): isolated edge pixels differ (about 0.05% of 1920x1080). With `--world-lighting` the world-lit draws among them keep their host view normals (`world_light_record` reads only the raw v0/v1 inputs, not the program's outputs) and go through `renderer_draw_skinned_world_lit`: the device computes the vertices, the host normals fill the lit draws' normals region and the light mask goes into push word 3, as `renderer_draw_world_lit`. `--no-gpu-vertex-programs`: those programs on the CPU as before. |
| `--direct-geometry` / `--no-direct-geometry` | on by default: `renderer_draw` writes the frame's vertices straight into the slot's mapped geometry buffer (`src/renderer/vertex_sink.h`, `RendererConfig.direct_geometry`) after `renderer_begin` waited for the slot, so `upload_geometry` copies only the indices and normals: the same bytes at the same offsets, one copy of every vertex less (about 1.6 ms/frame of host render time in the hideout). A frame that outgrows the buffer moves its vertices to the heap copy once and is uploaded as before; the buffer then grows with headroom. `--no-direct-geometry`: the heap copy, as before. |
| `--no-pipeline-cache` | the persistent pipeline cache is on by default (`--pipeline-cache`): every pipeline the renderer creates (graphics and compute) goes through one `VkPipelineCache`, loaded at `renderer_init` from `<hdd>/pipeline-cache.bin` and written at exit (`renderer_shutdown`, or `renderer_pipeline_cache_save` on exits that skip it) and every 32 new geometry pipelines, so a crash keeps the progress. The file (`src/renderer/pipe_cache.h`) is a checksummed wrapper around the driver's `vkGetPipelineCacheData` blob; it is used only when its `VkPipelineCacheHeaderVersionOne` names this vendorID, deviceID and pipelineCacheUUID (same GPU, same driver build), otherwise it is dropped (`[pipeline] cache ...: made by another GPU or driver` / `corrupt (checksum)`) and rewritten. Writes are atomic (temporary file, flush, rename). `<hdd>/pipeline-keys.bin` lists the geometry pipelines a run used (key, emissive flag and the shape's 37 words, not the per-run shape index; at most 4096, newest kept; dropped when the geometry shaders change); the next `renderer_init` builds them on a low-priority thread (nice 19 / THREAD_PRIORITY_LOWEST) sharing the cache (created without EXTERNALLY_SYNCHRONIZED: the driver synchronizes it; the layouts and shader modules are made on the render thread first). `get_pipeline` takes a pre-warmed pipeline when it is ready and otherwise compiles it as before, never waiting for the thread. The pipeline key layout is unchanged. `--no-pipeline-cache`: no cache object, no files, no thread (as before). `--stats` adds `[pipeline] 0-60 s: N compiled on demand (T ms, longest L ms), P pre-warmed used, worst renderer frame F ms` per 60 s and the same for the whole run at exit (`[pipeline] run (N s): ...`; `renderer_pipeline_report`). Frames are identical either way (hle_d3d_test frame sha256 unchanged) |
| `--vp-threads N` | worker threads that run the title's vertex programs (the CPU interpreter; default auto = physical cores - 2, at least 1, at most 7; `0` = none, every draw on the recording thread). A draw of two or more 32-vertex batches is copied (inputs and constants; each stream up to the end of its last vertex's elements, so a declaration whose elements reach past the stride reads the same bytes after the draw's vertices as on the recording thread) and queued, and the workers fill its vertices before Swap hands the frame over; a draw of one batch runs on the recording thread. Idle workers poll briefly (about 5-30 us), then block until a draw has more pending batches than workers awake, and they wake each other in a chain, so the recording thread makes at most one wake-up call per draw. The output is byte-identical for every N (`runtime/tests/vp_pool_test.c`). The startup line `[d3d] vertex programs: N worker threads` shows the size; `--stats` reports the pool's busy and spinning time |
| `--hud-hd` | restyle the target's health bar (top right, e.g. a car or enemy being hit) as a cleaned-up bar: thicker (grows down from its top edge to 4.3 % of its width), dark outline, the game's fill color with a vertical gradient, dark translucent empty part (off by default; see [HUD bars](#hud-bars)). `--hud-hd-height F` scales the height (0.5-4, default 1), `--hud-hd-opacity F` makes the bar translucent (0-1, default 1; the outline at sqrt(F) so the edge stays readable), `--hud-hd-frost N` blurs the background seen through the bar inside exactly its own rect, outline included (radius 0-32 in 640x480 pixels, default 0 = off; square corners, no margin, tint or rim); each turns it on. The rage meter under the character name is never changed |
| `--hud-hd-glass` | glass target bar preset (recommended look): the empty part very transparent with a light frosted tint over a blurred background, the fill translucent but vivid (it keeps the game's hue), a thin soft frame outline; sets track opacity 0.25, fill opacity 0.6, vivid 1, frost 4 (options after it override them; see [HUD bars](#hud-bars)) |
| `--hud-hd-track-opacity F`, `--hud-hd-fill-opacity F`, `--hud-hd-vivid F` | the glass style's controls (each turns on `--hud-hd` in the glass style): opacity of the empty part and of the fill (0-1, default `--hud-hd-opacity`), and color compensation of the translucent fill (0-1, default 0 = off; 1 gives back the saturation and brightness the fill's translucency takes away) |
| `--hud-glass` | draw the target's health bar as a low-opacity bar on a frosted-glass panel: blurred background, light tint and rim (off by default; see [HUD bars](#hud-bars)). Tunables, each also turning it on: `--hud-glass-blur N` (blur radius in 640x480 pixels, 1-32, default 4), `--hud-glass-opacity F` (bar opacity 0-1, default 0.45; the empty part of the bar at half of it), `--hud-glass-tint AARRGGBB` (tint color and strength, default `30ffffff`) |
| `--fullscreen` | start in desktop fullscreen; F11 or Alt+Enter toggle fullscreen in the SDL window (the swapchain follows the new size) |
| `--refresh-match`, `--no-refresh-match` | match the display's refresh rate to the 60 Hz frames (default on). When the game starts in `--fullscreen` on a display whose rate is not a multiple of 60 (144, 165, 75 Hz; 59.94 and 119.88 Hz count as multiples, within 1 Hz as SDL reports them), the window goes exclusive fullscreen with the display switched to the desktop's size at the highest multiple of 60 that size offers (144 Hz -> 120 Hz), so every frame stays on screen for the same number of refreshes instead of alternating between 2 and 3 (a judder at 60 fps on 144 Hz). The frame keeps its `--resolution` and the swapchain the desktop size; the present mode is unchanged. The window still opens in desktop fullscreen; the switch happens once it has shown its first frame (and its events were handled once), so the renderer starts on a known window and only rebuilds its swapchain for the new mode. While exclusive, the window no longer minimizes when it loses focus (SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS 0: the launcher or another program taking the foreground left a minimized window without a swapchain, which looked like a hang at start). For 3 s after the switch a watchdog goes back to desktop fullscreen for the rest of the session when no frame was shown or the window was minimized without losing focus (`[video] refresh-match: reverted to desktop fullscreen (<reason>)`). SDL restores the desktop mode at exit, while the window is minimized and when F11 / Alt+Enter leaves fullscreen; F11 back sets the chosen mode again. Windowed, already a multiple of 60, no such mode or `--no-refresh-match`: nothing changes (desktop fullscreen as before). One startup line tells what happened, e.g. `[video] display 144 Hz is not a multiple of 60: switched to 1920x1080 @ 120 Hz (--no-refresh-match keeps 144)` or `[video] display refresh left at 144 Hz (--no-refresh-match)`; a failed switch keeps desktop fullscreen and says so; the `[video] refresh-match:` lines log each step of the switch |
| `--skip SYM:POP[:RET]` | patch a guest function to `mov eax,RET; ret POP` |
| `--audio sdl\|null` | audio output (default `sdl`: SDL2 loaded at run time, any driver SDL picks, e.g. PulseAudio/PipeWire/ALSA; falls back to `null` with a one-line notice when SDL2 or a device is unavailable). The mixer runs in both cases |
| `--resampler linear\|sinc` | how each voice (22-32 kHz XACT waves, DirectSound buffers) is converted to the 48 kHz mix. Default `sinc`: 64-tap Kaiser-windowed sinc (polyphase, 1024 phases), images above the source Nyquist about -90 dB, flat to about 0.45 of the source rate, any rate change per 5 ms chunk; delays every voice by 31 source frames (1.4 ms at 22 kHz); 48 kHz sources (Bink movies) pass through unchanged. `linear`: the earlier linear interpolation (images up to -25 dB on voices, top octave 4.5-5 dB dull), for A/B listening. With `--stats` the mixer prints its CPU cost every 5 s of audio |
| `--audio-lead-ms N` | output lead on the SDL device, 0 to 500 ms (default 50). The mixer keeps that much audio queued: it prefills the queue when the device opens, mixes ahead whenever the device pulls a period and refills it at once after an underrun, so the device clock paces the mix and nothing is dropped. The device period requested is the largest power of two up to half the lead (256..1024 frames; 1024 at 50 ms) and the lead is raised to at least two of the periods SDL grants. A device that consumes nothing for 250 ms (or four periods) counts as stalled: the mixer then follows the host clock without queueing, so DirectSound cursors and cue ends keep moving. `GetCurrentPosition` on DirectSound buffers reports the play cursor held back by the frames still queued (at the buffer's rate, never before the position at `Play`), so Bink's audio clock and lip-sync follow what is audible; the write cursor stays 1024 bytes ahead of the mixer's read position. `0`: the earlier host-clocked queue (chunks mixed when due by the host clock, 1024-frame periods, a 5 ms chunk dropped whenever more than 150 ms is queued), which underran about 13 times a second in a headless `SDL_AUDIODRIVER=disk` yard run. Logged as `[audio] output lead 50.0 ms (2400 frames, device period 1024 frames)` or `[audio] output lead off (host-clocked queue)`; an invalid value exits with status 2 |
| `--audio-dump FILE.wav` | write the mix (16-bit stereo 48 kHz) to a WAV file; the header is updated every second and at exit |
| `--audio-log N` | log the first N calls per XACT/DirectSound entry point plus `[xact]` lines for banks, cues and notifications (default: 32 with `--trace`, else 0) |
| `--sound-pack DIR` | play `DIR/<cue name>.wav` instead of that cue's wave (see [Sound packs](#sound-packs)); logged as `[audio] sound pack DIR: N sounds replaced`, `[audio] sound pack: K waves of this sound bank replaced` and one `[audio] sound pack: cue index X (bank/wave) plays FILE` line the first time each override plays |
| `--no-xact-controls` | A/B test: ignore the cue volume and fade controls ("V", "F") the title sets in `Prepare`/`PlayEx` and `SetParameterControl`, so every XACT cue plays at its sound entry volume (the behavior before they were applied). Logged as `[audio] xact controls: on` or `off` |
| `--no-xbox-headroom` | A/B test: drop the console's voice headroom. The Xbox DirectSound gives every new voice 600 mB (-6 dB) of headroom unless its creation flags include any of 0x182000 or 0x200010 (XBE 0x3ac844), and a voice plays at its volume minus its headroom (0x3a8457). XACT creates its wave voices with flags 0x400000, so every cue (effects, music, dialogue, and their reverb send) plays 6 dB below its sound entry and control level; no sound entry of this title has the flag (+0xb & 2) that sets 0 (0x3d9ce0). DirectSound buffers get the default of their creation flags and `IDirectSoundBuffer_SetHeadroom` changes it (BINK sets 0, so movies stay at full level). Default: applied, which leaves the limiter about 6 dB more room on busy scenes. Logged as `[audio] Xbox headroom: on` or `off` |
| `--reverb`, `--no-reverb` | the room reverb the title describes through I3DL2 (default on). The title sets one I3DL2 listener (`IXACTEngine_SetI3DL2Listener`; in level 1: room -1000 mB, room HF -800 mB, decay 1.49 s, HF ratio 0.67, reflections -2273 mB at 7 ms, reverb -2217 mB at 11 ms, diffusion 50, density 100, HF reference 5 kHz) and sends every positioned (3D) sound to it through mix bin 10 at 0 mB; the console ran it on its DSP (`D:\audio\Warriors.bin`). The host mixer rebuilds it (`src/reverb.c`): 8 early-reflection taps per ear and an 8-line feedback delay network whose decay is the I3DL2 decay time at low frequencies and decay x HF ratio at the HF reference (ratios above 1 are clamped to 1). The send follows the listener's room rolloff (0 here: no distance attenuation, so far sounds are more reverberant). 2D cues (music, cutscene dialogue), Bink movies and streams stay dry unless their DirectSound mix bins include bin 10. Before the title sets a room (lRoom -10000, the XDK default) nothing is added. Cost: about 0.5% of a core (`build/reverb_test`; `--stats` prints it per 5 s). `--no-reverb`: dry, as before. Logged as `[audio] I3DL2 reverb: on` or `off`; the title's values as `[i3dl2]` lines |
| `--doppler`, `--no-doppler` | Doppler pitch on positioned (3D) sounds (default on): ratio (c - v_listener.u) / (c - v_source.u), u from the source to the listener, c = 343 units/s (DirectSound 3D: 1 unit = 1 m), each projected speed clamped to c/2 and the ratio to 1/2..2, applied through the voice's resampling ratio and eased over about 20 ms. Velocities are the title's (`SetListenerVelocity`, `IXACTSoundSource_SetVelocity`) when it sets nonzero ones, else estimated from the position updates over the title's clock (the frame-locked `QueryPerformanceCounter`), smoothed with a 0.1 s time constant; a step faster than 100 units/s (a teleport or a reused source) or a gap over 0.5 s restarts the estimate at rest. 2D cues, Bink movies and streams are unchanged. `--no-doppler`: no pitch change, as before |
| `--rear-cue`, `--no-rear-cue` | front/back cue on positioned (3D) sounds (default on): the stereo pan cannot tell front from back, so a source behind the listener gets a one-pole low-pass, from none at the sides to a 5 kHz cutoff straight behind (about -6 dB at 10 kHz), fading out inside the sound's minimum distance like the pan. `--no-rear-cue`: off, as before |
| `--dialogue-volume DB`, `--music-volume DB`, `--sfx-volume DB` | player volumes: extra level in dB (-40 to +12, decimals allowed, default 0) on every XACT cue of a class, 2D and 3D, on top of the title's own levels (entry volume, controls, category volumes). Music = category 1 (sound entry byte +0xa). Dialogue = the spoken-line cues: the title has no dialogue category (speech and effects are both category 0), but its sound bank sorts the cues by name, so the lines are two blocks of cue indices, `vags/character/voices/...` (crew, enemies, passers-by: 0x537-0x4d62) and `vags/speeches/...` (story lines and in-engine cutscenes: 0x519c-0x5952), from the name table in `warriors.glr`; only for the USA bank (23523 cues, header word +6 = 0x62d8), any other bank has no dialogue class. Effects = every other cue. Bink movies (DirectSound buffers) are unchanged, also their dialogue. Logged as `[audio] volumes: dialogue ... dB, music ... dB, effects ... dB`; an invalid value exits with status 2 |
| `--sfx-3d-volume DB` | experimental: extra level in dB (-40 to +12, decimals allowed, default 0) on every positioned (3D) XACT sound effect; dialogue, 2D cues, music, Bink movies and streams are unchanged. Logged as `[audio] 3D sound-effect volume: ... dB`; an invalid value exits with status 2 |
| `--no-rolloff-curves` | A/B test: attenuate 3D cues by the inverse distance law (min / (min + (d - min)), constant beyond max) instead of their 3D record's rolloff curve, which is what the Xbox uses (see HLE audio below). With it, voices 5-20 m away are 10-15 dB quieter than on the console |
| `--audio-levels` | one `[levels] t=S all A rev R TAG:L ...` line per second of mixed audio: RMS dBFS (both channels, before the peak limiter) of the whole mix, the reverb return and every voice tag that sounded (XACT cue index in hex, `100000` = DirectSound/Bink). Join the tags with a cue name list to compare dialogue, music and effects |
| `--no-hle-audio` | do not replace the XACT/DirectSound entry points (reproduces the MCPX APU MMIO fault) |
| `--no-hle-input` | do not replace the XAPI input entry points; `XInitDevices` (USB/OHCI init) is then skipped with `ret 8` and the title sees no controller |
| `--input-script SPEC` | scripted pad input for headless runs (see Input below) |
| `--input-config FILE` | key map file (see Input below); default `warriors-input.cfg` in the current directory when present; an unreadable FILE exits with status 2 |
| `--pad-mappings FILE` | SDL controller mappings read instead of `gamecontrollerdb.txt` in the program's folder and the current directory (see Input below); an unreadable FILE is logged, not fatal |
| `--stats` | every 60 presented frames, print the presented rate and where a frame's time goes: guest frame time without the 60 Hz pacing sleep (and its draw recording: vertex fetch, vertex programs, lighting; vertices per frame), time Swap waited for the host, host render time split into command recording, GPU wait, readback and swapchain (image acquire + queue present), and present time (the SDL/x11 backend); and a second line with the input latency of frames that read the pad: age of the pad state at `XInputGetState` (window events are pumped every 4 ms and after each present), read to Swap, Swap to taken by the host, and render plus present; a third line splits the recording per recorded draw (µs/draw): setup (declaration, primitive expansion, index range, stream checks), state (base image, fog, lighting, matrices), pixel shader, textures (stage headers and texel keys), vertices (fetch and conversion), vertex programs (the interpreter), tail (indices, operation), plus the guest-memory checks made while recording (count per draw and their time); a fourth line gives the real CPU cost: process CPU time per presented frame (every thread, also as cores in use) and the vertex-program worker pool's busy and spinning time per frame (ms, and busy as a share of both), with the program draws per frame queued to the pool and run on the recording thread (compare CPU ms per frame, not fps, when other programs share the CPU); every 300 presented frames a `[pacing]` line: world updates per presented frame (0/1/2+, from the step counter at 0x5825fc, whose even values run the world update) and how many frames each world state stays on screen (1/2/3/4+, share held exactly 2), plus the title clock (`[[0x4aca1c]+0x40]`, ms) against host time as a game-speed check; a total line at `--frames`. With `--sim60` every step is a world update and the line counts it so: 1 update per frame and holds of 1 frame are the 60 Hz cadence |
| `--tri-check` | diagnostic, off by default (also `XB_TRI_CHECK=1`): every replayed draw's clip positions are checked for NaN/infinite values and for spike triangles, from inside the draw's robust box (10th-90th percentile of x, y, w over a sample of its vertices) out to a vertex more than 32 times its extent outside it (`src/tri_check.c`); a triangle whose vertices are all far out, a distant quad of a particle batch, is not a spike. Each hit (at most 300) logs `[tri-check] frame F op I: PATH ...` with the path (`gpu-skin`/`gpu-world`: the device's vertices, read back with the frame on the readback path, else `(cpu-replica)`: the same program on the CPU from the recorded inputs; `cpu-vp`: the vertex-program interpreter; `fixed`: fixed function), character / mesh-pack / offscreen tags, the program and mesh hashes, the worst vertex and the box; every 600 frames a `[tri-check] N frames (S sampled, 1 in 8): ...` summary. Only one frame in 8 is checked (the device's vertices sit in mapped, uncached memory that is slow to read). It also logs once per program and stream a declaration whose elements reach past the stride, and a `--mesh-pack` replacement whose bone indices use palette slots the original mesh does not (those constants hold whatever the title left there). Costs CPU time per draw; for logs, not for play |
| `--no-tex-race-guard` | turns off the texture replay race guard (on by default, `src/tex_race.c`). A draw records only its textures' headers and a sparse key of their bytes; the host thread reads the texels when it replays the frame, while the title already records the next one, so memory the title reuses in between (streaming) could be decoded and cached under the old key for good. At replay, before anything read from guest memory is cached by a texture's key (the decoded upload, the texture-pack hash memo, the `--sharp-faces`, sharp-font and KBM-atlas sources, the `--sky-hd` dome palette), the key is computed again from the bytes there now. A mismatch is counted and logged as `[tex-race] frame F: texture ADDR (format FF, WxH, L levels, N bytes) changed between record and replay: ...` (at most 8 lines per 600 replayed frames); with the guard nothing is cached under the recorded key: the first mismatch of a texture address skips the draw for that frame (the next frame records the new bytes), and when the same address mismatches again on a later frame (within 8 frames; a texture rewritten in place every frame) the draw uses the bytes there now, under their own key (`... changed again, drawn uncached under the recorded key ...`); with this option the bytes are cached as before. A `--sky-hd` dome whose bytes changed turns the procedural sky off for that frame. The exit summary prints `[tex-race] N textures changed ... in M replay checks (... MiB checked, ... ms), ... draws skipped, ... textures drawn with their current bytes`. Changes outside the bytes the key samples (first and last 16 bytes, 64 spaced dwords) stay invisible, as at record time |
| `--fx-scale N` | effect pools, 1-4 with decimals (default 1 = the title untouched): multiplies the particle tasks (blood, debris, smoke, glass shards, sparks: 1400 + 100), glass (100) and sound tasks (256), and the free particle tasks at or below which ground blood fades or dies (512), so blood stays through fights; and the particle sprites drawn per frame per pool (part_page1 640 / 400 / 200, part_fire 256), clamped to the 2048 quads of the shared index buffer (each pool's 3 vertex buffers are sized from its count and come from the graphics device's 2 MB "Dynamic vertex buffers" heap, which grows by their extra bytes: 1.2 MB at 4; when that heap's site differs, no sprite pool is raised). Implies `--big-heap`, so the larger pools (about 1 MB at 4) do not come out of the streamed world's memory. Every site is byte-checked; one with other bytes is skipped with a `[fx] skip ...` line. Not raised: the 60 characters, the 255 particle systems, the audio channels. Launcher: Display > Effects and particles (Normal 1 / More 2 / Maximum 4). With `--stats`, a `[fx]` line every 60 presented frames: each pool's used/capacity with its window and run peak (`particles`, `particles2`, `glass`, `sound`, `script` objects), per sprite pool of 64 or more (`spritesN`, N its size) the most sprites asked for in one flush and the sprites dropped, all pools' dropped sprites, and the MB of the 128 MB contiguous window handed out |
| `--big-heap` | the title's main heap (all its pools, then the streamed world) grows from 45.4 MB below 64 MB to 77.5 MB inside the runtime's 128 MB contiguous window, which leaves 31.9 MB of the window for the D3D data the runtime allocates later. Implied by `--fx-scale` above 1 |
| `--no-rate-fixes` | keep the HUD minigames' per-frame steps the title authored for 30 fps (see Rate fixes under Design; on by default). `--rate-fixes` turns them back on |
| `--draw-distance N` | far plane (= fog end) up to N times the title's, 1-4 (default 1 = as shipped, nothing written). The title grows the far plane toward the sector streamer's target, capped at 1.5 x the level's Lua value (mostly 90) and at 300; both caps become x N (operands at 0x32a514 / 0x32a651 pointed at runtime floats). The fog moves out with it. Cutscene cameras keep the Lua value. The far plane still only reaches as far as sectors have streamed in |
| `--prop-detail N` | small props (bins, crates, debris) drawn up to N times as far, 1-8 (default 1 = as shipped). The title skips a prop of radius r at distance d when r / d^2 < 0.0004 and fades it in up to 0.0005; both thresholds become / N^2 and the fade scale x N^2 (`.rdata` 0x4212d4, 0x4212d8, 0x42142c, used only by the prop draw). Per-prop authored max distances are unchanged |
| `--fog-breathing` | keep the title's far-plane pull-in after slow frames. Default (steady fog): the branch at 0x32a5e7 that, whenever the title's fps meter reads 29.5 or less, pulls the far plane in by up to 20 units per hitch (regrowth 10.5 units/s) is skipped, because on this port host hitches feed that meter and the fog visibly "breathes". All three patches check the shipped bytes at every site and write nothing of a patch whose bytes differ; the start-up line `[drawdist] applied: steady fog (0032a5e7), draw distance 2.00x (0032a514, 0032a651), ...; skipped: ...` lists them |
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

#### Performance lines (always on)

Two log lines need no option (`src/perf_log.c`, pure logic tested by `build/perf_log_test`;
fed by `perf_frame` in `src/hle_d3d.c` on the host thread):

```
[hitch] frame F: T ms (game G, hle H, render R, gpu P, tex-upload U, pipeline-compile C ms, pace S, wait W, other O ms) level L
[level-summary] level L: N frames, avg A fps, 1% low B fps, worst W ms, cpu C ms/frame, gpu P ms/frame, vram peak V MiB, hitches H, tex misses M
```

- `[hitch]`: a presented frame more than 25 ms after the previous one. `F` is the frame number
  (as in `--dump-every` names), `T` the interval between the two presents. `game`: the guest's
  frame from the end of the 60 Hz pacing sleep to its Swap (the frame it presented; draw
  recording included). `hle`: of it, the Direct3D draw recording (measured only with `--stats`,
  else `?`). `render`: the host replay and renderer_end / renderer_present. `gpu`: the GPU
  timestamps of the latest completed frame (the timestamp pool of `--fps-overlay-items gpu`,
  now created in every run: two timestamps per frame, nothing drawn; `?` without timestamps or
  for frames that skip the renderer), so with frames in flight a GPU-heavy frame shows up one or
  two frames late. `tex-upload`: `renderer_texture_create` (image and staging copy) plus the
  frame's one submit and wait for its batched texture copies (the CPU decode of guest textures
  before it is not counted). `pipeline-compile`:
  pipelines compiled on demand in that frame. `pace`: the guest's 60 Hz pacing sleep before the
  frame (waitable timer or sleep, then the spin to the deadline); at light load it is most of a
  period, and more than a period means the pacer woke late. `wait`: of `render`, the time
  blocked on the device (the frame slot's fence, swapchain image acquire and present; `?` for
  frames that skip the renderer). `other`: `T - game - render - pace`, the time no field
  accounts for (the host loop picking the frame up, the hand-off, scheduling; never below 0, `?`
  when `game` or `render` is `?`). Lines of older builds end at `pipeline-compile`. `level`: the
  level name the title last loaded (`level51`, ...), `?` before the first load. Frames with a movie quad (Bink movies play at
  their own 24-30 fps) print no line. Frames without draws (the CPU path, black screens), the
  frame that sees a level load and frames while it is pending (`[[0x5b9f78]+0x148] == 3`) end
  with ` (loading)`. At most one line per second; the hitches in
  between become one `[hitch] N more suppressed` line before the next `[hitch]` line (or before
  the summary); at most 200 `[hitch]` lines per session, then none.
- `[level-summary]`: when the title loads a level (the previous level's frames, printed at the
  next presented frame) and at exit (`[d3d] exit`). Loading and movie frames are left out of
  every number.
  `avg`: frames over their summed intervals. `1% low`: the frame rate at the 99th percentile
  frame time, from a fixed histogram of 0.25 ms bins up to 256 ms (frames longer than that count
  as 256 ms; `worst` is exact). `cpu`: process CPU time (every thread, `plat_process_cpu_ms`, as
  `--fps-overlay-items cpu`) per frame. `gpu`: the mean of the frames' `gpu` values. `vram peak`:
  the driver's device-local usage of this process (VK_EXT_memory_budget, the numbers
  `src/vram_budget.c` works from), sampled once a second; without the extension the renderer's
  own device-local total (`renderer_vram`). `hitches`: the counted frames over 25 ms (also
  the suppressed ones). `tex misses`: draws drawn with the guest texture because the pack's
  replacement was not loaded yet (`?` without `--texture-pack`).

#### Load profile (`--load-profile`)

One line per level entered (`src/load_prof.c`, pure logic tested by `build/load_prof_test`), for finding where level load time goes:

```
[load] level87: 1182 ms to the first level frame (39 frames, longest 594 ms) | guest frames 310 ms | file reads 58 (13.9 MB, 46 ms; audio 0 (0.0 MB, 0 ms), movies 0 (0.0 MB, 0 ms)) | texture creation 212 ms (pack 0 ms) | pipeline builds 17 ms | pack loader reads 0 (0.0 MB, 0 ms) | process CPU 800 ms
```

- The window opens at the title's level load request (the MenuLoadLevel site that also names the
  level for `[level-summary]`) and closes at the first presented frame with at least 100 draws
  and no movie quad (loading screens draw 1-2 quads, the menus about 40, a level a few hundred).
  A load that shows no such frame within 180 s prints `no level frame within 180 s`. A level
  that starts with a movie (mission 1, `l99_in`) counts the movie's time until it ends or is
  skipped.
- `guest frames`: the guest's frame time without the 60 Hz pacing sleep (its game logic, the
  parsing and any decompression of what it read, and its waits for the reads below). `file
  reads`: the guest's `NtReadFile` calls (`src/kfile.c`): count, bytes and wall time; `audio` are
  reads of `.xwb`, `.xsb`, `.xgs`, `.wav`, `.wma` files or files in an `audio` folder, `movies`
  of `.bik` files, the rest are the archives (`*.wad*`, `XBoxWad.idx`). The HLE wave-bank
  streaming in `src/xact.c` is not counted. `texture creation`: `renderer_texture_create` on the
  render thread (the `[hitch]` line's `tex-upload`), of it `pack` the texture pack replacements.
  `pipeline builds`: pipelines compiled on demand. `pack loader reads`: the texture pack loader
  threads' reads and decodes (count, decoded MB, time). `process CPU`: every thread.
- Times of different threads overlap, so the fields do not add up to the total; the remainder
  of the total is mostly frames the title paces at 60 Hz while its loading screen is up.
- At the first frame, `[load] packs at start: mesh pack M ms, texture pack index T ms` (with
  `--mesh-pack` or `--texture-pack`), and `[load] sound pack at start: S ms` with `--sound-pack`:
  these are read once at start, not per level.
- Level loads are seen at MenuLoadLevel (0x27fd9e): with `--warp` / `--scene` through the warp's
  stub (after its redirect), otherwise the same stub only reports the name (`[level] level loads
  tracked ...`; nothing redirected, no save stub; signature-checked like the warp, and on a
  mismatch `[level] level loads not tracked` and every level reads `?`).
- Cost: a few clock reads, one process-CPU read and three histogram/sum updates per presented
  frame on the host thread, and the driver's memory numbers once a second; no lock, no
  allocation, no output unless a frame is a hitch.

#### Log line prefixes: robustness diagnostics

Always on (`src/diaglog.c`, test `make build/diaglog_test`); they are there so a crash or a
glitch can be diagnosed from the log alone:

| Prefix | Line | When |
| --- | --- | --- |
| `[breadcrumb]` | `[breadcrumb] -12.345s frame 1234: <event>` | dumped on every crash and fatal path, oldest first: the last 100 notable events (seconds before the crash, frame). Events: `level load level51 (mission 14)`, `movie start l1_in.bik` / `movie end ...`, `thread t9 create (...)` / `thread t9 exit (status 0)`, `texture pack load 1: N textures`, `mesh pack load: N meshes`, `mesh pack replacement <hash>`, `texture pack cache cap ...`, `swapchain create\|recreate WxH`, `large alloc N KiB at <addr> (arena U MiB used)` (guest and contiguous allocations of 4 MiB or more, and failed ones), `guest arena exhausted: ...`, `save read\|write <file>` (files on the save partition `U:`), `Vulkan device lost in <call>` |
| `[crash-context]` | `[crash-context] reason: <why> (last N breadcrumbs, oldest first)`, then after the breadcrumbs `[crash-context] level L, mission M, frame F, uptime S s, guest arena used U MiB of T MiB, threads N` | on the same paths, before the register dump: `=== FAULT` (signal / exception), `[fatal]` (unimplemented kernel import and other kernel fatal errors), `abort()`, Windows `=== CRT invalid parameter` (first report) and `=== unhandled exception`, a failed guest thread creation, and a Vulkan `VK_ERROR_DEVICE_LOST` (once; the game goes on). L and M are `-` before the first level load; M is the story mission, 0 for the Coney hub (`level95`); threads = guest threads running. Written with `write()` from a fixed buffer, no allocation |
| `[hle-missing]` | `[hle-missing] <name> (first call from 0xADDR, frame F)` | once per name: the first call of a kernel export that is a stub (accepted, does nothing: `kernel_register_stub`, e.g. `KeSaveFloatingPointState`, `MmFreeContiguousMemory`) or not implemented (just before its `[fatal]`), of a D3D8 / XACT / DirectSound entry point the HLE accepts without emulating (`ok`), and of a path that is not supported (`IDirectSound_CreateSoundBuffer: unsupported format (silent)`) |
| | `[hle-missing] summary: <name>xN, ...` | at exit (frame limit or window closed), most calls first; `none` when nothing was missing |
| `[vk-error]` | `[vk-error] <call>: <VkResult name> (frame F)` | every non-success `VkResult` of a renderer call (`VKC` in `vk_renderer.cpp`), at most 5 lines per call and result every 10 s (a later line adds `(N more not logged)`); `VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` (the swapchain is recreated) once per call, `(further ones only counted)` |
| | `[vk-error] summary: <call> <name> xN, ...` | at exit, when any was reported |
| `[audio]` | `[audio] underrun N (frame F)` | the SDL audio queue was empty when the mixer delivered its next chunk (the device ran dry); N counts them; at most 3 lines every 10 s |
| | `[audio] underruns: N total` | at exit |

`XBRUN_DEBUG_CRASH=FRAME` (or `FRAME:abort`), a hidden test hook: crashes on purpose (a write
fault in host code on the presenting guest thread, or `abort()`) when frame FRAME is presented,
to check the dump.

### Audio

Sound plays through SDL2 (i386 build; `sudo apt install libsdl2-2.0-0:i386 libpulse0:i386
libasound2t64:i386` — `libasound2:i386` on older releases). SDL picks PulseAudio/PipeWire
when a server is reachable and ALSA otherwise; force one with `SDL_AUDIODRIVER=pulseaudio`
(or `pipewire`, `alsa`). WSL2 with WSLg: WSLg exports `PULSE_SERVER=unix:/mnt/wslg/PulseServer`,
so `./xbrun ... --symbols default.syms` plays through Windows; if no device opens, the run
continues silently (`[audio] falling back to the null backend`). Headless checks:
`--audio null --audio-dump build/run.wav`, then `python3 tools/wav_stats.py build/run.wav`.

3D sounds (voices, crickets, dogs, traffic and other positioned emitters) fade with distance
along the sound bank's rolloff curves, as on the Xbox, and use a constant-power pan (0.707 per
ear when centered). The Xbox's HRTF filters are not emulated (its I3DL2 reverb is rebuilt on the
host, see `--reverb`). `--dialogue-volume DB` raises or lowers the spoken lines,
`--music-volume DB` and `--sfx-volume DB` the music and the other sounds.

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
under `extracted/` (gitignored) and never commit, upload or share them. A step-by-step guide for
players is [`docs/TEXTURE_PACKS.md`](../docs/TEXTURE_PACKS.md).

- `--dump-textures DIR` writes every unique texture a draw samples once, as `DIR/<hash>.png`
  (level 0, RGBA8) plus `DIR/manifest.csv` (`hash,format,w,h,levels,linear,has_alpha,draws,
  first_frame`). Runs into the same directory merge (known textures are not rewritten, draw
  counts add up), so dump several scenes one after the other.
- `--texture-pack DIR` replaces every texture whose hash has a `DIR/<hash>.png` (any size; the
  mip chain is generated on load, uploaded as RGBA8; LIN_* textures work too because their
  coordinates are normalized by the original size). Files are read the first time the texture is
  drawn; with a pack the renderer's texture cache budget is 1 GiB instead of 512 MiB.
- A pack may also hold `DIR/<hash>.dds` files (BC1, BC3 or BC7, UNORM, with the complete mip
  chain down to 1x1; DX10 header or legacy DXT1/DXT5 FourCC), made from a PNG pack by
  `tools/texture_compress.py`. When the device samples BC formats the `.dds` is preferred and
  uploaded as stored (no PNG decode, no CPU mips, 4-8x fewer device bytes than RGBA8); otherwise,
  or when a `.dds` is malformed (logged once with the reason: sRGB, partial chain, wrong size ...),
  the hash's `.png` is used if the pack has one. The log adds `[texpack] N replacements have a
  .dds` and, on exit, how many replaced textures were BC blocks. With layered packs a hash keeps
  the first pack's files; a later pack's `.png` only backs a `.dds`-only hash.
- `--texture-pack` can be given up to 8 times to layer packs: the first pack that has a hash
  wins, hashes only in a later pack still load from it (e.g. `--texture-pack <button-icon pack>
  --texture-pack texture_pack/<pack>` puts your own button-icon atlas over an upscaled pack).
  The log has one `[texpack] N replacement textures in DIR` line per pack and, with several,
  a `[texpack] K packs, first match wins: ...` line.
- `--mesh-pack DIR` (experimental, off by default) replaces skinned character meshes that have a
  `DIR/<hash>.wmesh` (src/meshpack.h: content hash of the draw's indices and streams 0/1). The
  replacement keeps the vertex format and the 32-slot bone palette, so the title's vertex program
  skins it with the live animation. `tools/mesh_pack` writes smoothed (Loop-subdivided) meshes.
  The log has one `[meshpack] character mesh <hash>: ...` line per mesh seen (with or without a
  replacement), which is how hashes are matched to files.

### Sound packs

`tools/audio_extract/audio_extract.py` writes every game sound as a WAV named by its cue name, and
`--sound-pack DIR` plays a WAV with the same path instead. Both work from your own disc only.

```sh
make -C runtime                                   # builds runtime/build/xact_extract (decoder)
python3 tools/wad_extract.py <image.iso> extracted/default.xbe extracted/wad   # name candidates (once)
python3 tools/audio_extract/audio_extract.py -j4  # extracted/audio/: 23,523 WAVs, 7.3 GiB, ~37 h
python3 tools/audio_extract/audio_extract.py --filter '*/speeches/l01/*'   # only some sounds
```

Output: `extracted/audio/vags/<path>.wav` (16-bit PCM at the original rate; ~22.8k of 23.5k sounds
are named), `_unnamed/<bank>_<index>.wav` for the rest, `catalog.csv` (`name, bank, index,
duration, channels, rate, category, file`; category is voice, dialogue, sfx, music or ambient)
and `names.csv` (the recovered names, reused by later runs; `--rescan-names` rebuilds it).
Names are CRC-32 matches of strings from `default.xbe`, `lua_src/` and the `.scn` files against
`warriors.glr` chunk 0x29, checked against the 15-character name tails of its debug copy.

To make a pack, copy the WAVs you want to change into a folder with the same relative path, edit
or replace them, and pass the folder:

```text
mypack/vags/character/<voice set>/<line>.wav     (the "vags/" folder may be left out)
mypack/vags/xboxmusic/<track>.wav
TheWarriors.exe <disc>\default.xbe <disc> --sound-pack mypack
```

- Keying: the runtime computes CRC-32 of each file's path without `.wav` (lower case, `/`,
  `vags/` prefixed when missing) and looks it up in the game's own name table, read at start-up
  from `<disc>/XBoxWad.idx` -> `warriors.glr`, giving the cue; when the title creates its sound
  bank, the cue's wave (wave bank, index) is replaced. Every place that wave plays uses the file,
  with the cue's volume, 3D position and looping (a looping cue loops the whole file).
  `IXACTSoundBank_GetCueProperties` reports the file's length, so the title times the cue by it.
- Formats: RIFF WAVE PCM 8/16/24/32-bit or 32-bit float (also WAVE_FORMAT_EXTENSIBLE), mono or
  stereo, any sample rate (the mixer resamples). FLAC/OGG/MP3 are not read: convert them to WAV
  first (e.g. `ffmpeg -i in.flac out.wav`).
- Limits: every file is decoded to 16-bit PCM in memory at start-up, 256 MiB in all (about 25
  minutes of 44.1 kHz stereo); files past the budget are skipped with a message. Files whose
  name is not a cue name are reported and ignored. A random-variation cue would key one variation
  only (this title's cues have one wave each).
- Off by default; without `--sound-pack` nothing is read and the lookup returns before any other
  work.


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

BC pack (smaller, faster to load): `tools/texture_compress.py` turns any PNG pack into
`<hash>.dds` files the runtime uploads as is (see `--texture-pack` above). It needs `bc7enc` from
[bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) (MIT or public domain; the optional
`bc7e.ispc` encoder is Apache-2.0), built separately, e.g.
`cmake -S bc7enc_rdo -B bc7enc_rdo/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
-DCMAKE_CXX_FLAGS="-include cstdint" && cmake --build bc7enc_rdo/build` (add `-DSUPPORT_BC7E=TRUE`
with [ispc](https://github.com/ispc/ispc) on PATH and `-DCMAKE_EXE_LINKER_FLAGS=-no-pie` for the
better BC7 encoder):

```sh
python3 tools/texture_compress.py --pack extracted/texture_pack/<pack> --out extracted/texture_pack/<pack>-bc \
    --encoder path/to/bc7enc -j 4
```

Opaque textures become BC1 (`--opaque bc7` for BC7), textures with any alpha BC7, UI/font
textures (category `ui`/`font` in the pack's `selection.csv`, the ui-like heuristic, `--skip-list`)
are copied as PNG, so the output is a complete pack (`--with-png` copies every PNG as a fallback
for devices without BC support). Mips are made offline in display space like the runtime's own,
with premultiplied alpha and Lanczos-3 (`--mip-filter box` = the runtime's 2x2 box);
the alpha-test coverage of cutouts is kept on every level of every BC7 texture by default
(`--alpha-coverage cutout` limits it to category `cutout`, `--alpha-coverage none` turns it off).
Up-to-date outputs are skipped (`--force` redoes them).

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
both). Without a 32-bit Vulkan driver, or when Vulkan fails to start for another reason, the game
carries on with the CPU path: the Bink movies still show and the sound plays, but the menus and the
game are black. The log then says `[renderer] Vulkan unavailable (<reason>); using the CPU path
only` and `[renderer] ERROR: 3D frames will be black: <reason>`, and on Windows a message box (in
Spanish and English) asks the player to update the graphics card driver before the game goes on
(not with `--video null` or `--dump-frame`, nor with the `XBRUN_NO_DIALOGS` environment variable
set; never for an explicit `--renderer none`). A readback frame the renderer fails to draw shows
the CPU back buffer: the first failure is logged once (`[renderer] ERROR: readback frame N failed
to render`) and the exit summary counts them. Nothing else is needed: the C runtime is the Universal CRT that Windows 10/11 include,
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
| AMD FidelityFX Super Resolution 1 (`ffx_fsr1.h`, `ffx_a.h` v1.20210629; EASU and RCAS ported to GLSL) | `src/renderer/shaders/fsr_easu.frag`, `fsr_rcas.frag` (compiled into `xbrun-core.dll` as SPIR-V), constant setup in `src/fsr.c` | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. | the notice at the top of both shader files ([FidelityFX-FSR `license.txt`](https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/master/license.txt)) |
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
  `DISPATCHER_HEADER` objects in guest memory, guarded by one global mutex. A blocked
  thread sleeps on its own condition variable and is woken only when an object it waits
  for changes (one shared, broadcast condition variable let the Bink movie player's
  mutex poll, about 520,000 releases a second, keep every blocked guest thread spinning:
  3.1 cores during an intro movie, now 1.0). Blocked threads also re-check every 100 ms;
  a wait that only that re-check satisfies is logged once. `make -C runtime
  build/kobj_test` builds the game-independent test.
- **Memory** (`src/mem.c`): guest allocations come from `0x10000000..0x70000000`;
  contiguous memory comes from a 128 MB window at `0x80000000`, with
  physical address = VA - `0x80000000`. Pool allocations use the host heap.
  Guest allocations (NtAllocateVirtualMemory, MmAllocateSystemMemory, thread KPCR/KTHREAD and
  stacks) take the next address of a bump pointer; once it reaches the arena end they reuse the
  lowest gap left by freed guest regions (log `[mem] guest arena end reached: allocations now
  reuse freed space`). On Windows the host's own allocations go bottom-up into the same range,
  so at start-up the free parts of `0x10000000..0x30000000` become reservations of the runtime
  (`[mem] guest arena 10000000-30000000: N MB kept free of host allocations`): before this, host
  heaps and thread stacks (texture loader staging, pipeline pre-warm) landed ahead of the bump
  pointer, which skipped them for good, and starting mission 14 crashed when the title's
  thread creation found the arena exhausted. A truly exhausted arena logs `guest arena
  exhausted (N bytes): M MB live in K guest regions` (on Windows plus a `[mem]` address-space line), and
  thread creation then fails with STATUS_NO_MEMORY instead of writing through NULL.
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
    panned from their sound source and the listener. The distance gain is the 3D record's
    rolloff curve, as XACT hands it to DirectSound (0x3d83f3 -> `SetRolloffCurve`; byte +0x1d
    point count, bytes +0x1e the points / 255) and the full-HRTF calculator applies it
    (0x3af4c8): n linear gains at min + k (max - min) / n, k = 1..n, 1.0 at the min distance,
    linear in between, the last point at and beyond max. Every 3D record of this title has the
    same 10 points (206 163 124 91 63 40 22 10 2 0), so a sound fades smoothly from its min to
    its max distance and is silent beyond it: a voice (min 1, max 40) is -4.5 dB at 10 m, where
    the inverse distance law gave -20 dB (`--no-rolloff-curves`). `SetCategoryParam` pauses
    (param 1) or resumes (0) the cues of a category (0xff = all; 0x3cf4ff -> 0x3d8780); the
    title pauses everything around loads and its pause screen. Parameter controls: the sound bank maps
    named per-cue values (this title: "V" volume on every sound, "F" fade on 2D sounds, "P"
    pitch on 3D sounds) to volume or pitch through 4-point piecewise-linear curves (table at
    header +0x14); the title sets them in the `Prepare`/`PlayEx` description and with
    `SetParameterControl` (e.g. an ambient bed at V 0.18 = -15.9 dB, faded in by F 0 -> 1
    over 5 s). Volume curves add to the entry volume (clamped to 0 dB), for 2D and 3D cues,
    with XACT ramps advanced once per `XACTEngineDoWork`.
    Category volumes (`IXACTEngine_SetCategoryVolume`, clamped to -10000..0 mB): the sound
    entry's byte +0xa names its category (0 effects, 1 music in this title); the cue level adds
    its category volume and, unless the category is 0xff, the global category 0xff (XBE
    0x3d77b7), clamped to -10000..0, and changes reach prepared and playing cues at once. The
    title sets -6400 on a category when its options slider is at its minimum, so effects or
    music then fall silent. Every XACT wave voice then plays 6 dB lower (the console's voice
    headroom, `--no-xbox-headroom`). Not applied: pitch curves (the title never sets "P"),
    global controls ("MasterVolume"), cones. The I3DL2 listener drives the host reverb (`src/reverb.c`,
    `--reverb`); 3D cues send to it through their source's mix bin 10.
  `build/hle_audio_test` checks the stubs and the XACT/DirectSound behavior with synthetic banks
  (including the parameter controls, mixed level by level); `build/audio_test` covers the curve
  evaluation and the control table lookup.
- **Audio output** (`src/audio.c`): a mixer thread clocked by the host monotonic clock mixes
  the voices every 5 ms (48 kHz stereo, windowed-sinc resampling in `src/resample.c` or linear
  with `--resampler linear`, float accumulation, s16 output)
  and hands each chunk to the backend (`SDL_QueueAudio`, queue capped at 150 ms) and the
  `--audio-dump` file, so the dump is the same with or without a device. SDL2 is opened with
  `dlopen` like the video backend (`LoadLibrary` of `SDL2.dll` in a Windows build).
  `build/audio_test` (and `tests/test_audio.py`, which compares the ADPCM decoder with a Python
  port of vgmstream's) covers ADPCM, synthetic banks, streaming, the mixer and the WAV dump.
  `tools/wav_stats.py FILE.wav` prints per-second RMS/peak levels of a dump.
  `build/resample_test [sinc|linear]` checks the resampler on synthetic tones (images and
  aliasing at most -85 dB, passband within 0.1 dB to 0.45 fs_in: `linear` fails these by design),
  voice start/end, looping, stereo and rate changes through the mixer, and prints the CPU cost.
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
- **Gamma ramp** (`--gamma-ramp`, default on; `src/gamma_ramp.c`, `set_gamma_ramp` in
  `src/hle_d3d.c`, `record_gamma` in `src/renderer/vk_renderer.cpp`, `shaders/gamma.frag`): the
  title calls `D3DDevice_SetGammaRamp(2 = D3DSGR_IMMEDIATE, ramp)` about once per frame, from
  0xbbbb3 in levels and 0x249d55 in the movie player, and the in-game Brightness screen
  (`DlgLight` in `level100main.lua`, default 40 of 0..80) ends there too. The ramps are not the
  identity: `0xba800` builds them from a per-level curve (table 0x459288, 40-byte entries; level81:
  0..256 -> 0..285, slope about 1.11) plus an offset the Brightness setting raises, so dark tones
  are crushed and highlights lifted (menus: mid-grey 128 -> 118; a level81 headless run: green
  quantiles 14/20/28/41/81 -> 3/11/20/35/81). The console applies them in the video output, so
  before this the picture lacked the console's contrast and the Brightness slider did nothing. The ramp is the last pass on the frame: after bloom, `--filmic` and `--sharpen`
  (the console applies it to everything in the framebuffer, and the port's post effects are part of
  that picture; applying it earlier would also let `--filmic` re-grade the title's brightness), and
  before the swapchain scaling and `--fps-overlay` (port UI the console never had). The table is a
  1 KB uniform buffer (entry i = red | green << 8 | blue << 16) written with `vkCmdUpdateBuffer`
  when it changes; the pass reads the B8G8R8A8 frame with `texelFetch`, so channel i / 255 maps to
  table entry i exactly. An identity ramp (or `--no-gamma-ramp`) records no pass and the frame is
  bit-identical. Without a Vulkan renderer (`--renderer none`) the ramp is not applied.
  `build/gamma_ramp_test` checks the options, the D3DGAMMARAMP layout, identity detection and the
  table against `gamma_ramp_apply` (the shader's CPU reference).
- **HD movies** (`--movie-hd`, default on; `src/movie_hd.c`, `movie_hd_redirect` in `src/kfile.c`,
  `draw_movie_gpu` in `src/hle_d3d.c`): the title picks the movie file by its video flags
  (0x24a243: bit 0x8 `%s_hd`, 0x4 `%s_w`, 0x2 `%s_pal`), and the HD bit needs an HDTV AV pack in
  720p, which would also change the title's device and HUD setup. Instead `NtCreateFile` opens
  `<name>_hd.bik` in place of a requested `bik\<name>[_w][_pal].bik` when that file is a 16:9 HD
  Bink movie (header width and height at bytes 20/24); the guest Bink code sizes its planes from
  the header (1280x720 luma, 640x360 chroma). The title centres the quad at the movie's size
  ((-320,-120)-(960,600), made for a 1280x720 back buffer); it is fitted to the 640x480 frame at
  16:9 on the display. With a renderer the quad is recorded as a regular draw: the title's Bink
  combiner program runs on the GPU at output resolution with bilinear sampling of all three
  planes. Three details of that draw: texture coordinates move from the title's texel-centre
  convention (texel i at i) to the renderer's (i + 0.5) and in by half a texel, so the wrapping
  sampler never blends the opposite edge; stage 2 takes coordinate set 1 (the title's
  TexCoordIndex, which is not replayed); and the program's constants come from its definition
  (`SetPixelShaderProgram`: the PSC0/PSC1 mappings only name the D3D constants a later
  `SetPixelShaderConstant` would replace). The planes are not cached textures: each frame they are
  copied into three persistent 8-bit images (`renderer_movie_plane`, made again only when the
  movie size changes) by a copy recorded at the start of the frame's command buffer, so they never
  fill or flush the texture cache, skip the texture pack and `--dump-textures`, and cost no
  synchronous upload. The exit summary prints `[renderer] N movie planes copied, M texture cache
  drops`. `build/movie_hd_test` checks the name rule, the
  header check, the fit, the texel coordinates and the bilinear quad; `build/kfile_test` the
  redirect.
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
  reaching the next frame time early), so the pair runs on every other presented frame. When a
  present is a whole period late or more (a missed deadline, a stall; 1/16 period short of it
  counts), the frame time also advances by the whole periods it is late; when it is more than
  half a period away from real time at two presents in a row (a slow host, 30 fps movies), or
  more than half a period ahead, it moves by whole periods to the one nearest real time: game
  speed stays real time within about a period and the steps keep their phase. A single present
  less than a period late (a pacing sleep that woke late, the next present back on the grid)
  keeps +1 period, so no step pair runs twice in one frame and none in the next. With the lock,
  a Swap a whole period late or more also skips those periods of the 60 Hz schedule (the same
  grid) instead of presenting the next frames back to back to catch up; one less late presents
  at once and the next one waits less. Only this counter is
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
  | litter step | 0x8c394 | 0x410440 -> 0x410448 | wind-blown litter (FUN_0008bc00) moves velocity x 1/30 per update; the wind kick's velocity (velocity x 0.7 + wind x 1/30, FUN_0008b270, 0x8b929/0x8ba3a) is not a step and keeps 1/30 now that its interval doubles |
  | litter counters | 0x8bd70 (ground probe 20 -> 40), 0x8ae05/0x8ae0a (settle random(600, 900) -> (1200, 1800)), 0x8be93 and 0x8bf6f (lifetime 60 -> 120), 0x8adc9/0x8adcb and 0x8bf81/0x8bf83 (kick interval random(1, 30) -> (2, 60)), 0x8b79c/0x8b7a1 (tumble blend random(30, 60) -> (60, 120)) | | every litter duration counts updates: at 60 Hz the pieces were kicked, tumbled and respawned twice as often, and with the halved kick drifted at half speed |
  | litter gravity | 0x8bfd4, 0x8bfda; constant at 0x44c074 (unused bytes, checked zero) | x 0.32667 (0x420778) -> 0.16333 (0x420774); + 0.16333 (0x420774) -> 0.081667 | vz -= (0.5 - 0.5 x \|n.z\|) x 0.32667 + 0.16333 per update: twice the fall acceleration at 60 Hz |
  | litter 2 counters | 0x8cd70/0x8cd72 and 0x8d1fb/0x8d1fd (kick random(20, 60) -> (40, 120)), 0x8d095/0x8d09a (sway blend random(30, 60) -> (60, 120)), 0x8d1eb (lifetime 60 -> 120) | | the second litter pool (FUN_0008d0c0, rotation only) counts the same way |
  | airborne fail-safe | 0x16b294 (`cmp eax,60` on +0x344), 0x16b2aa (`cmp dword [eax+0x68],60` on the mover +0x170) in the human update | 60 -> 120 both | airborne for 60 updates while the mover (FUN_002503a0) failed to resolve its collision for 60 consecutive moves puts the character back at its last safe position (FUN_00142220): 2 s at 30 Hz, 1 s at 60 Hz |
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
  instead of using the time step and were not patched run at twice their rate. The litter, the
  held-button test and the airborne fail-safe are patched.
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
