/* xbrun: loads an original Xbox XBE into this 32-bit process at its native addresses,
 * replaces kernel imports with host implementations and runs it: by default with the D3D8
 * API boundary on the host (frames shown in a window), or with --no-hle-d3d until a hooked
 * D3D entry point is reached. Usage: xbrun <default.xbe> <disc_dir> [options] */
#define _GNU_SOURCE
#include "diag.h"
#include "diaglog.h"
#include "files.h"
#include "fspatch.h"
#include "hle_spawn.h"
#include "hle_fx.h"
#include "hle_aidbg.h"
#include "hle_allies.h"
#include "hle_tactics.h"
#include "hle_ratefix.h"
#include "hle_luagc.h"
#include "hle_skipintro.h"
#include "hle_memreg.h"
#include "hle_drawdist.h"
#include "hle_sim60.h"
#include "hle_huspeed.h"
#include "combat_debug.h"
#include "refresh_match.h"
#include "rateprobe.h"
#include "hle_warp.h"
#include "hle_goto.h"
#include "gamectl.h"
#include "gamectl_proto.h"
#include "hle_devlua.h"
#include "hle_mods.h"
#include "hle_modmenu.h"
#include "modsound.h"
#include "colpatch.h"
#include "modsafe.h"
#include "hle_textfix.h"
#include "hle_scriptfix.h"
#include "audio.h"
#include "steam_audio.h"
#include "acoustics.h"
#include "resample.h"
#include "hle_audio.h"
#include "sndpack.h"
#include "sharp_font.h"
#include "hle_d3d.h"
#include "kbm_glyphs.h"
#include "settings_sync.h"
#include "hle_settings.h"
#include "hle_photo.h"
#define RENDER_REC_NO_ROUTE
#include "render_rec.h"
#include "r64_input.h"
#include "r64_selftest.h"
#include "renderer/vrs.h"
#include "normal_maps.h"
#include "ktx2.h"
#include "hle_input.h"
#include "input_map.h"
#include "hle_xapi.h"
#include "kernel.h"
#include "load_prof.h"
#include "movie_hd.h"
#include "gamma_ramp.h"
#include "cinema_bars.h"
#include "nvset.h"
#include "kobj.h"
#include "mem.h"
#include "video.h"
#include "xbe.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fps_overlay.h"
#include "fsr.h"
#include "taa.h"
#include "dynres.h"
#include "bench.h"
#include "framegen.h"
#include "anim_interp.h"
#include "platform.h"
#include "tex_race.h"

extern void d3d_hook_entry(void);
extern int watchdog_seconds;
void apu_fake_start(void);

static int handle_fd(uint32_t h) {
    HObj *o = handle_get(h);
    return o && o->type == H_FILE ? o->fd : -1;
}

/* The program name in the usage text: argv[0] without its folder (xbrun on Linux,
 * TheWarriors.exe on Windows). */
static const char *prog_name = "xbrun";

static void set_prog_name(const char *argv0) {
    if (!argv0 || !*argv0) return;
    prog_name = argv0;
    for (const char *c = argv0; *c; c++)
        if (*c == '/' || *c == '\\') prog_name = c + 1;
    if (!*prog_name) prog_name = "xbrun";
}

/* Exit after the frame limit or a window close: the I3DL2 log summary, then the audio dump. */
static void audio_exit(void) {
    hle_audio_i3dl2_summary();
    audio_close();
}

/* Everything that reports at that exit (the process ends with _exit: no atexit handlers). */
static int audio_on_exit;
static void on_exit_all(void) {
    if (audio_on_exit) audio_exit();
    luagc_report_exit();   /* the last Lua state's peak (hle_luagc.h) */
}

/* Level watch (hle_warp.h) -> the breadcrumbs and the [crash-context] level (diaglog.h). */
static void level_loaded(const char *level, int mission) {
    dlog_set_level(level, mission);
    gamectl_level_loaded(level);   /* --control / --fast-boot: the level's first frame (gamectl.h) */
    if (mission >= 0) dlog_crumb("level load %s (mission %d)", level, mission);
    else dlog_crumb("level load %s", level);
}

static void usage(void) {
    fprintf(stderr,
            "usage: %s <default.xbe> <disc_dir> [--symbols file] [--hook SYMBOL|0xADDR]...\n"
            "             [--skip SYMBOL:POPBYTES[:RET]]... [--no-default-skips] [--no-hle-audio] [--fake-mmio] [--watchdog SEC] [--hdd dir] [--trace] [--log-limit N]\n"
            "             [--no-hle-d3d] [--frames N] [--dump-frame FILE.bmp] [--video sdl|x11|null]\n"
            "             [--renderer vulkan|none] [--no-hle-input] [--input-script FRAME:[pN/]CONTROLS[:FRAMES],...] [--stats] [--load-profile] [--tri-check]\n"
            "             [--record-renderer FILE] [--record-renderer-frames S:N] [--renderer-traffic S:N] [--mem-map-every N]\n"
            "             [--render-process off|loopback|helper-io|helper] [--render64-helper PATH] [--render64-frames K] [--render64-pad-poll-us N] [--renderer-cache on|off|check] [--renderer-hashes FILE] [--renderer-check-queries]\n"
            "             [--input-config FILE] [--button-icons xbox|kbm|auto|ps-black|ps-white|ps-color] [--pad-icons xbox|ps-black|ps-white|ps-color] [--no-sharp-fonts] [--no-rumble] [--pad-mappings FILE] [--pad-slot N=GUID[#K]]... [--spawn-scale N] [--fx-scale N] [--big-heap] [--no-rate-fixes] [--spray-time PCT] [--skip-intro] [--draw-distance N] [--prop-detail N] [--fog-breathing] [--no-text-fixes] [--no-script-fixes] [--no-frame-lock] [--sim60] [--smart-allies] [--enemy-tactics] [--ai-debug] [--combat-debug] [--rate-probe FILE[:MS]] [--rate-probe-range ADDR:SIZE|@PTR[+OFF]:SIZE]... [--poke ADDR:HEX[/EXPECT]]... [--warp MISSION[:CHECKPOINT]] [--goto PLACE|list] [--goto-allow-save] [--lua FILE] [--lua-exec CODE] [--lua-at FRAME] [--lua-retry] [--lua-pipe PATH] [--dev-console] [--mods DIR] [--mod ID[,ID]]... [--no-mods] [--mods-trace] [--audio sdl|null] [--audio-lead-ms N] [--audio-dump FILE.wav] [--audio-log N] [--audio-levels] [--sound-pack DIR] [--resampler linear|sinc] [--dialogue-volume DB] [--music-volume DB] [--sfx-volume DB] [--sfx-3d-volume DB] [--no-xact-controls] [--no-rolloff-curves] [--no-xbox-headroom] [--reverb|--no-reverb] [--doppler|--no-doppler] [--rear-cue|--no-rear-cue] [--audio-hrtf] [--no-steam-audio] [--no-audio-reverb-geo] [--no-audio-pathing] [--no-audio-air] [--no-audio-occlusion] [--patch-fs] [--win-thread-probe] [--control PATH] [--control-idle SEC] [--fast-boot]\n"
            "             [--resolution WxH | --render-scale N] [--supersample N] [--sharpen N] [--fsr off|ultra|quality|balanced|performance] [--fsr-sharpness F] [--taa] [--taa-sharpness F] [--taa-upscale off|native|ultra|quality|balanced|performance|RATIO] [--upscaler taa|fsr3] [--dynres TARGET_MS|auto] [--dynres-min RATIO] [--vrs off|auto|quality|performance] [--bench] [--bench-out FILE] [--anim-interp] [--fps-overlay] [--fps-overlay-corner tl|tr|bl|br] [--fps-overlay-items fps,ms,cpu,gpu] [--no-pipeline-cache] [--sync-pipelines] [--no-dynamic-state] [--bloom] [--bloom-strength F] [--bloom-threshold F] [--filmic] [--filmic-strength F] [--filmic-grain F] [--filmic-vignette F] [--filmic-lut FILE.cube] [--saturation F] [--hdr off|auto|on] [--hdr-peak NITS] [--hdr-paper-white NITS] [--auto-exposure] [--auto-exposure-strength F] [--cutscene-dof] [--cutscene-dof-strength F] [--cutscene-dof-speaker 0|1] [--rain-hd] [--rain-hd-density F] [--no-rain-hd-splash] [--no-rain-hd-wet] [--rain-hd-wet-strength F] [--no-rain-hd-cover] [--sky-hd] [--sharp-faces] [--sharp-faces-over-pack] [--neon-glow] [--neon-glow-strength F] [--neon-glow-bloom F] [--neon-glow-flicker F] [--ssao] [--ssao-strength F] [--ssao-radius F] [--ssao-mode classic|gtao] [--gtao-quality low|medium|high] [--ssgi off|low|medium|high] [--ssgi-strength F] [--lamp-lights] [--lamp-lights-strength F] [--light-walls 0|1] [--level-lights] [--level-lights-max N] [--light-shafts] [--light-shafts-strength F] [--light-shafts-cheap] [--volfog] [--volfog-quality low|medium|high] [--volfog-strength F] [--char-lighting] [--hair] [--hair-strength F] [--skin] [--skin-strength F] [--eyes] [--eyes-strength F] [--cloth] [--cloth-strength F] [--no-hat-fix] [--no-hat-subdiv] [--world-lighting] [--world-lighting-strength F] [--title-shadow-full] [--fire-light] [--fire-light-strength F] [--lamp-shadows] [--lamp-shadows-strength F] [--lamp-shadows-min-angle DEG] [--lamp-shadows-walls 0|1] [--wall-doors 0|1] [--world-shadows] [--world-shadows-strength F] [--world-shadows-size N] [--world-shadows-range M] [--contact-shadows] [--contact-shadows-strength F] [--rain-hd-puddles] [--wet-light-streaks] [--wet-light-streaks-strength F] [--wet-light-streaks-cap N] [--materials] [--materials-strength F] [--damp-streets] [--damp-streets-strength F] [--neon-reflections] [--neon-reflections-strength F] [--reflections classic|sssr] [--reflections-quality low|medium|high] [--reflections-glass F] [--reflections-res half|full] [--soft-particles] [--soft-particles-distance F] [--normal-maps] [--normal-strength F] [--specular-strength F] [--normal-baked F] [--dry-highlights] [--dry-highlights-strength F] [--parallax] [--parallax-depth F] [--msaa N] [--aniso N] [--widescreen] [--no-movie-hd] [--no-gamma-ramp] [--no-cinema-bars] [--fullscreen] [--no-refresh-match] [--frame-gen] [--frame-gen-of] [--taa-lite auto|on|off] [--fsr3-masks on|off]\n"
            "             [--language en|es|fr|de|it]\n"
            "             [--present auto|mailbox|fifo|immediate|readback] [--frames-in-flight N] [--dump-textures DIR] [--texture-pack DIR]... [--mesh-pack DIR] [--mesh-log]\n"
            "             [--no-render-targets] [--no-frame-blur] [--shadow-scale N] [--shadow-soft] [--shadow-range] [--shadow-range-dist F] [--hud-hd] [--hud-hd-height F] [--hud-hd-opacity F] [--hud-hd-frost N] [--hud-hd-glass] [--hud-hd-track-opacity F] [--hud-hd-fill-opacity F] [--hud-hd-vivid F] [--hud-glass] [--hud-glass-blur N] [--hud-glass-opacity F] [--hud-glass-tint AARRGGBB]\n"
            "  default: HLE D3D, frames in an SDL2 window; with --no-hle-d3d the default hook is\n"
            "  D3D8__Direct3D_CreateDevice (needs --symbols)\n"
            "  --input-config FILE: key map (default: warriors-input.cfg in the current directory, if present)\n"
            "  --patch-fs: rewrite the title's fs:[KPCR] accesses as on Windows (Linux: for testing)\n"
            "  --trace: log every kernel call, file open and HLE call (default: summaries, warnings and faults)\n"
            "  --tri-check: log draws with NaN/infinite or exploded post-transform vertices (spike triangles;\n"
            "           also XB_TRI_CHECK=1); a diagnostic for logs, off by default\n"
            "  --load-profile: one [load] line per level entered: time from the load request to the level's first\n"
            "           frame, split into guest frame time, file reads (archives, audio, movies), texture creation,\n"
            "           pipeline builds, texture pack loader reads and process CPU; pack index times at start\n"
            "  --stats: per 60 presented frames, the presented rate and where the time goes (guest frame,\n"
            "           draw recording, waits for the host, host renderer, present), the process CPU per frame and the\n"
            "           vertex-program pool's busy and spinning time; also an [fx] line with the effect\n"
            "           pools' use (particle tasks, glass, sound tasks, script objects), sprites dropped per sprite pool\n"
            "           and the contiguous memory handed out; every 600 frames and at each level summary a\n"
            "           [gpu-passes] line: the GPU time per pass (Vulkan timestamps; scene draws, shadows, lighting\n"
            "           passes, ambient occlusion, fog, TAA, post effects), largest first\n"
            "  --record-renderer FILE: write the renderer call stream of renderer frames --record-renderer-frames S:N\n"
            "           (default 1:60) for runtime/build64/render64_replay; the cache is\n"
            "           dropped at its first frame; --record-renderer-pixels K stores every K-th frame's pixels;\n"
            "           --renderer-traffic S:N measures that stream for frames S..S+N-1 without writing it ([render64]);\n"
            "           --mem-map-every N: the address space by owner every N renderer frames and at exit ([mem-map])\n"
            "  --render-process loopback: every renderer call goes through the render64 wire (encoder, content cache,\n"
            "           ring, decoder) into the renderer of this process (off: direct calls, the\n"
            "           default); --renderer-cache off|check: the stream's content cache off, or with a collision check;\n"
            "           --renderer-hashes FILE: every read-back frame's hash; --renderer-check-queries: count queries the\n"
            "           per-frame snapshot answers with a stale value\n"
            "  --render-process helper-io (a test mode): the 64-bit render64 helper owns the window, its events\n"
            "           and the controllers and sends the input through shared memory; rendering stays in this process,\n"
            "           offscreen; --render64-helper PATH: the helper (default\n"
            "           xbrun-render64 next to the program, then ../build64/; Windows: x64\\WarriorsRender64.exe);\n"
            "           --render64-inject N[@MS] (tests): MS after\n"
            "           its window opened the helper types N keys, then plugs a virtual controller and closes the window\n"
            "  --render-process helper: the 64-bit render64 helper renders too: every renderer call goes through\n"
            "           the wire into the helper's renderer, which presents in its window (headless, SDL's dummy driver: it\n"
            "           reads every frame back; Windows: x64\\WarriorsRender64.exe with\n"
            "           its own x64\\SDL2.dll); --render64-frames K: frames the game may run ahead of the helper (1-4,\n"
            "           default 2); with --record-renderer the file is the stream the helper rendered\n"
            "  --render64-pad-poll-us N (helper-io, helper): the helper waits for window input and reads open controllers\n"
            "           (XInput has no events) every N us (100-16000, default 1000; without a controller every 4 ms)\n"
            "  --render64-selftest [--render64-helper PATH]: no XBE or disc: checks the helper on this machine in a few\n"
            "           seconds (its channel, a synthetic scene through the wire compared with this process's renderer,\n"
            "           presents) and exits 0 with a \"[render64] selftest: PASS\" line, 1 with \"... FAIL: <check>\"\n"
            "  --resolution WxH: render the 3D scene at the largest 4:3 size inside WxH (e.g. 1920x1080 -> 1440x1080);\n"
            "  --render-scale N: at N x 640x480; --msaa N: N samples per pixel (2, 4, 8); --aniso N: anisotropic\n"
            "           filtering up to N (default: 640x480, no MSAA, no anisotropy; --dump-frame writes the scaled frame)\n"
            "  --supersample N: render at N x that size (2 or 4) and box-filter it down to it (antialiasing of\n"
            "           every edge and texture; combines with --msaa; window and --dump-frame keep the output size)\n"
            "  --sharpen N: contrast-adaptive sharpening of the finished frame, 0-100 (default 0 = off; AMD FidelityFX\n"
            "           CAS's filter: stronger on soft detail, gentle on edges that are already sharp; also in --dump-frame)\n"
            "  --fsr MODE: AMD FidelityFX Super Resolution 1: render the 3D scene below the output size (--resolution /\n"
            "           --render-scale) and upscale it (EASU) and sharpen it (RCAS) to that size; MODE off (default),\n"
            "           ultra (1.3x), quality (1.5x), balanced (1.7x), performance (2x per axis; e.g. 1440x1080 quality\n"
            "           renders 960x720). Replaces --supersample and --sharpen (ignored with a warning); the HUD is\n"
            "           upscaled with the scene; gamma ramp, --fps-overlay and --dump-frame are at the output size.\n"
            "           --fsr-sharpness F: RCAS sharpness in stops, 0 (sharpest) to 2 (default 0.2)\n"
            "  --taa: temporal anti-aliasing: the 3D scene is jittered by a sub-pixel offset every frame and blended\n"
            "           with the previous frames, reprojected by the camera's motion, at the end of the 3D scene (before\n"
            "           bloom, depth of field and the filmic look); removes jagged and shimmering edges, so --msaa can be\n"
            "           lowered or turned off. The HUD, text, menus and movies are not affected. --no-taa turns it off.\n"
            "           --taa-sharpness F: sharpening of the result (RCAS, FSR 1's contrast-adaptive filter), 0 (none)\n"
            "           to 1 (strongest; default 0.5, with --taa-upscale rising with the ratio to 0.85 at 2x per axis)\n"
            "  --taa-upscale MODE: temporal upscaling (FSR 2 style, implies --taa): the 3D scene renders below the\n"
            "           output size and the TAA history rebuilds the output size from the jittered frames; MODE off\n"
            "           (default), ultra (1.3x), quality (1.5x), balanced (1.7x), performance (2x per axis; 1920x1080\n"
            "           ultra renders 1477x831, quality 1280x720), or a ratio per axis from 1.1 to 3 (e.g. 1.4).\n"
            "           native: no upscaling, the scene at the full output size and the temporal resolve (TAA or\n"
            "           --upscaler fsr3, FSR 3.1's Native AA) anti-aliases only (jitter, history, sharpening, no\n"
            "           texture LOD bias); the costliest mode, about 1.7x ultra's scene pixels.\n"
            "           Sharper than --fsr at the same render size once the camera settles; bloom, wet ground,\n"
            "           rain, the filmic look, the HUD, text, menus and movies stay at the output size. Replaces --fsr and\n"
            "           --supersample (ignored with a warning).\n"
            "  --upscaler taa|fsr3: what rebuilds the output size under --taa-upscale: taa (default; our TAA\n"
            "           resolve) or fsr3 (a port of AMD FidelityFX FSR 3.1's upscaler, MIT; implies --taa-upscale\n"
            "           quality when no mode is given). Works with every --taa-upscale mode, --dynres and --frame-gen.\n"
            "  --fsr3-masks on|off: FSR 3.1's reactive and transparency & composition masks, from the scene's\n"
            "           particles and see-through layers (default on; off: before / after comparisons).\n"
            "  --taa-lite auto|on|off: a cheaper --taa-upscale resolve (22 texture reads per pixel instead of 48,\n"
            "           a little softer on fine moving detail) for integrated GPUs: auto (default) on an integrated\n"
            "           GPU (Intel Iris Xe, AMD APUs) only, on always, off never. Not with --msaa or --upscaler fsr3.\n"
            "  --dynres TARGET_MS|auto: dynamic resolution: with temporal upscaling (implies --taa-upscale ultra\n"
            "           when no mode is given) the render size follows the GPU time of each frame (timestamps) to\n"
            "           keep it under TARGET_MS (4-100; auto = 14 ms, 60 fps with headroom): it drops at once when\n"
            "           a frame is over, rises slowly when there is room, never above the --taa-upscale mode's size\n"
            "           and never below 1 / --dynres-min of the output (RATIO 1.1-3 per axis, default 2). With\n"
            "           --taa-upscale native it starts at the output size, auto is 15.5 ms and the floor ultra's\n"
            "           1.3 unless --dynres-min is given (it drops only when the GPU misses the frame). The history\n"
            "           stays at the output size, so a change shows no seam. Off by default.\n"
            "  --vrs off|auto|quality|performance: variable rate shading (VK_KHR_fragment_shading_rate): a\n"
            "           small compute pass builds a shading-rate image from each frame (16x16 tiles), and the\n"
            "           next frame's flat, dark, fogged or blurred parts of the world are shaded at 2x1, 1x2 or\n"
            "           2x2 (performance: up to 4x4 without temporal upscaling), which takes time off the scene\n"
            "           draws. Characters, the HUD, text, menus, neon signs and alpha-tested fences and foliage\n"
            "           are always shaded at full rate. auto: quality where the GPU supports it (not on Intel\n"
            "           GPUs); off (default) or unsupported: every frame exactly as without it.\n"
            "  --bench: benchmark: the menus are pressed through, the first level load goes to mission 7's street\n"
            "           (--warp 7 unless --warp is given; the save on disk is never written), and after 10 s of play\n"
            "           the camera pans for 60 s; then a short report (average and 1%% low fps, GPU ms per frame and\n"
            "           per pass, render size, GPU, settings without paths) goes to the log and to --bench-out FILE\n"
            "           (default bench-report.txt) and the game exits. Runs on any save, a fresh one included.\n"
            "  --frame-gen: frame generation: a frame half way between every two real frames is built from them\n"
            "           and their motion (camera reprojection from depth, skinned characters' own motion) and shown a\n"
            "           refresh before the real one, so a 120 Hz display shows 120 frames a second; the HUD, text and\n"
            "           menus are the real frame's. Shown only in a window on a display at a multiple\n"
            "           of 120 Hz (fullscreen with --refresh-match switches 144/165 Hz screens to 120 Hz) with --sim60,\n"
            "           and paused while the game runs under 60 fps (the real frame presented twice; paced by\n"
            "           present completion where the driver has VK_KHR_present_wait); menus, movies and camera cuts are not\n"
            "           interpolated. Adds about 10 ms of latency and forces FIFO presentation. With --dump-frame and\n"
            "           --dump-every N each dumped frame also writes the frame before it and the generated frame\n"
            "           between them (<path>.N-g.bmp).\n"
            "           --no-frame-gen turns it off.\n"
            "  --frame-gen-of: --frame-gen plus optical flow: where the two frames disagree along the motion\n"
            "           vectors (rain, sparks, flicker: what has none), a block match of the two frames (16x16\n"
            "           blocks, up to 12 pixels a frame) is tried and used when both frames agree along it\n"
            "  --anim-interp: smooth animation in the 30 FPS mode: draws whose motion steps slower than the display\n"
            "           (without --sim60 characters, objects and the camera change every other frame) are shown between\n"
            "           their last two states, so the scene moves on every display frame, one frame (16.7 ms) later;\n"
            "           camera cuts and animation snaps are shown as they are. GPU vertex-program draws only (the default:\n"
            "           nearly the whole 3D scene). With --sim60 everything already changes every frame and almost nothing\n"
            "           is blended. --no-anim-interp turns it off (the default)\n"
            "  --fps-overlay: a small frame counter (\"60 FPS  16.6 ms\": presented frames per second and mean\n"
            "           frame time over half a second) drawn last, on top of the finished image, in a corner;\n"
            "           --fps-overlay-corner tl|tr|bl|br (default tl); --no-fps-overlay turns it off again;\n"
            "           --fps-overlay-items LIST picks what it shows, any of fps,ms,cpu,gpu (default fps,ms; shown in that\n"
            "           order: \"60 FPS  16.6 ms  CPU 34%%  GPU 71%%\"). cpu: this process's CPU use, 100%% = every logical\n"
            "           core busy; gpu: the game's own GPU time per frame over the frame period (Vulkan timestamps,\n"
            "           \"GPU --\" without them), not the whole GPU's utilization\n"
            "  --no-pipeline-cache: compile every shader pipeline when it is first needed, as before (default: the\n"
            "           compiled pipelines are kept in pipeline-cache.bin and pipeline-keys.bin in the --hdd folder and\n"
            "           the ones used last time are built ahead on a background thread; with --stats a [pipeline]\n"
            "           line per 60 s counts the pipelines still compiled while playing)\n"
            "  --sync-pipelines: a shader pipeline first needed mid-game is compiled before the frame goes on, as\n"
            "           before (default, --async-pipelines: compiled on background threads while the frame waits at\n"
            "           most 4 ms; the draws that need it use a simpler variant or wait a few frames instead of\n"
            "           freezing the game for the compile)\n"
            "  --no-dynamic-state: one shader pipeline per cull mode and depth state, as before (default: with\n"
            "           VK_EXT_extended_dynamic_state those are set per draw, so fewer pipelines are compiled)\n"
            "  --bloom: lamps, neon, fire and glow sprites bleed light into the 3D scene (not into the HUD or text);\n"
            "           --bloom-strength F: how much glow is added (0.1-3, default 0.6); --bloom-threshold F: the\n"
            "           brightness where it starts (0-1, default 0.75); --no-bloom turns it off again\n"
            "  --neon-glow: lit and neon signs glow at night: textures with an emission mask in a texture pack\n"
            "           (<hash>.emit.png / .emit.dds, docs/TEXTURE_PACKS.md) keep their lit texels' own colour through\n"
            "           fog and darkness and feed the bloom; --neon-glow-strength F (0.1-3, default 1.25),\n"
            "           --neon-glow-bloom F (0-1, default 1), --neon-glow-flicker F (0-0.3, default 0.04: 0 = steady);\n"
            "           --no-neon-glow turns it off again\n"
            "  --normal-maps: per-pixel relief and highlights from companion maps in a texture pack: <hash>_n.png/.dds\n"
            "           (tangent-space normal map, OpenGL convention: green = up the image) and <hash>_s.png/.dds\n"
            "           (specular: R = intensity, G = gloss), tools/normal_from_albedo.py; shaded by the per-pixel\n"
            "           lights of --char-lighting and --world-lighting (lamps, --fire-light); --normal-strength F (0-4,\n"
            "           default 1), --specular-strength F (0-4, default 1), --normal-baked F (0-1, default 1: how much\n"
            "           the relief shades the game's own baked light; 0 = untouched); --no-normal-maps turns it off\n"
            "  --dry-highlights: with --normal-maps, metal and glass shine: car paint, shop windows, chrome, metal\n"
            "           shutters, fences and signs catch the lamps, neon and fire with bright, tight highlights (and a\n"
            "           soft glint at grazing angles), while brick, asphalt, wood and cloth stay dull. Reads the material\n"
            "           that tools/material_shine.py writes into the relief pack's specular maps (maps without it draw\n"
            "           as before); --dry-highlights-strength F (0.1-3, default 1); --no-dry-highlights turns it off\n"
            "  --parallax: with --normal-maps and --world-lighting, relief with depth: brick, cobbles, stone and wood\n"
            "           planks sink in and shift in 3D as the camera moves (parallax occlusion mapping), from the height\n"
            "           that tools/parallax_height.py writes into the relief pack's normal maps (alpha; maps without it\n"
            "           draw as before); fades out with distance; --parallax-depth F (0.005-0.1 of the texture's size,\n"
            "           default 0.015); --no-parallax turns it off\n"
            "  --filmic: a 1979-film look for the 3D scene (not the HUD or text): pseudo-HDR lights, a gentle filmic\n"
            "           tonemap, a warm grade with teal shadows and deep blacks, a red-orange halation around lights\n"
            "           (with --bloom, which then glows wider), a light vignette; --filmic-strength F (0-2, default 1),\n"
            "           --filmic-grain F (0-1, default 0: off), --filmic-vignette F (0-1, default 0.25), --filmic-lut\n"
            "           FILE.cube replaces the built-in grade; --no-filmic turns it off again\n"
            "  --saturation F: saturation of the final picture, 0.5-2 (default 1: unchanged; above 1 more vivid,\n"
            "           below 1 more muted), in linear light, SDR and HDR alike\n"
            "  --hdr off|auto|on: HDR10 output (default off). auto / on use an HDR10 (PQ, BT.2020) swapchain when\n"
            "           the display offers one (Windows: turn HDR on in the display settings first); else SDR, and\n"
            "           the [hdr] log line says why. The HUD, text and movies stay at paper white, the scene's\n"
            "           lights (neon, lamps, fire, bloom) rise towards the peak. --hdr-peak NITS (400-10000, default\n"
            "           1000): the display's peak brightness; --hdr-paper-white NITS (80-500, default 200): white\n"
            "           of the HUD and of the SDR-range picture\n"
            "  --auto-exposure: eye adaptation of the 3D scene (not the HUD or text): coming out of a dark alley\n"
            "           into a lit street the frame flares and settles in about a second, stepping into the dark it\n"
            "           slowly opens up; partial and narrow (-0.4 to +0.5 stop) so the night stays dark, lamps and neon\n"
            "           are not metered, brightened highlights roll off instead of clipping, a light dither hides\n"
            "           banding; with --filmic it runs in its pass, else in a small exposure-only pass;\n"
            "           --auto-exposure-strength F (0-2, default 1); --no-auto-exposure turns it off again\n"
            "  --cutscene-dof: cinematic depth of field in the in-engine cutscenes: while the letterbox bars show\n"
            "           (fading in and out with them over 0.3 s) the characters stay sharp (everything from the nearest\n"
            "           to the farthest person in the middle of the picture, and every person anywhere) while the\n"
            "           background behind them and scenery close to the camera soften, with no halo around the\n"
            "           people; without people in view it focuses the centre; the focus follows\n"
            "           smoothly and snaps on camera cuts; the bars, subtitles and HUD stay sharp; gameplay is never\n"
            "           blurred; --cutscene-dof-strength F (0.25-2, default 1: the far blur is 1%% of the frame\n"
            "           height); --no-cutscene-dof turns it off again; --cutscene-dof-speaker 0|1 (default 1): the\n"
            "           focus follows the character who speaks (found from his lip-synced jaw and lips): he is\n"
            "           sharp, the other people are a little soft (never as soft as the background), and the focus\n"
            "           is pulled over about half a second when another one answers; in silence, or with 0, the\n"
            "           characters' range above\n"
            "  --rain-hd: while it rains, the title's flat rain layers are replaced by thin falling streaks at several\n"
            "           depths (hidden behind nearer walls and people, near ones larger and faster), with the title's\n"
            "           slant, speed and colour, and drops splash and ripple on the ground; --rain-hd-density F: streaks\n"
            "           and splashes per area (0.25-4, default 1); --no-rain-hd-splash: streaks without the splashes;\n"
            "           the ground under the rain looks wet (darker, glossy, reflecting lamps and lit windows);\n"
            "           --rain-hd-wet-strength F: its reflections and darkening (0.1-4, default 1.8); --no-rain-hd-wet: dry;\n"
            "           places under a roof get no streaks, splashes or wet ground (--no-rain-hd-cover: rain everywhere);\n"
            "           --rain-hd-puddles: puddles in low spots of the open wet ground, reflecting the street, rippled by\n"
            "           the drops (off by default; --no-rain-hd-puddles turns them off again);\n"
            "           --wet-light-streaks: street lamps and burning barrels are mirrored in the wet ground as long\n"
            "           streaks, even when the lamp is above the top of the screen (the lights the lighting uses; it\n"
            "           needs none of the light options), and puddles reflect the fog colour where nothing else is seen;\n"
            "           a light behind a wall or closed door (the level's collision on the disc, with --light-walls) or\n"
            "           hidden on screen behind something nearer leaves no streak (off by default);\n"
            "           --wet-light-streaks-strength F: how bright (0.1-4, default 1);\n"
            "           --wet-light-streaks-cap N: the most lights a frame mirrors (1-32, default 16);\n"
            "           --no-wet-light-streaks turns them off again;\n"
            "           --materials: the wet ground takes each surface's own material, read from the level's collision\n"
            "           data on your disc when it loads: asphalt glossy with puddles, concrete and wood less, dirt and\n"
            "           gravel dark matte mud, grass barely wet (off by default); --materials-strength F: how strongly\n"
            "           (0.1-2, default 1); --no-materials turns it off again;\n"
            "           when the game's rain starts or stops (walking in or out) it fades in or out over half a second;\n"
            "           --no-rain-hd turns it off again\n"
            "  --damp-streets: dry night streets look damp: where the level's collision data on your disc places\n"
            "           asphalt or rooftop tar (concrete a little), the ground gets a faint sheen: street lamps and\n"
            "           fires leave short, soft glints on the ground near them (also lamps above the screen; none\n"
            "           through walls), only the brightest things (lamp halos, neon) a faint blurred reflection, pools\n"
            "           of lamp light glint,\n"
            "           with a few small damp patches along kerbs; never on dirt, grass, wood, under a roof or a\n"
            "           walkway, indoors (no sky), by day or while it rains (the rain's own wet ground wins). Only\n"
            "           the lit spots brighten; the night stays dark. Works without --rain-hd (off by default);\n"
            "           --damp-streets-strength F: how strong (0.1-2, default 1); --no-damp-streets turns it off again\n"
            "  --neon-reflections: the glowing signs of --neon-glow are mirrored in wet ground (with --rain-hd and\n"
            "           --wet-light-streaks) and damp streets (--damp-streets) as soft coloured streaks, also signs\n"
            "           just above the screen, and the reflections keep their colour (red and blue tubes too), crisp\n"
            "           in puddles; needs --neon-glow and its masks (off by default); --neon-reflections-strength F:\n"
            "           how bright (0.1-3, default 1); --no-neon-reflections turns it off again\n"
            "  --reflections classic|sssr: how the wet ground (--rain-hd), its puddles and --damp-streets reflect\n"
            "           the scene: classic (default) the short screen-space march; sssr stochastic screen-space\n"
            "           reflections ported from AMD FidelityFX SSSR (a depth hierarchy march, rough surfaces blur\n"
            "           their reflection by their material, puddles stay sharp, steadied over frames);\n"
            "           --reflections-quality low|medium|high: its march budget (default medium; high also two\n"
            "           rays per pixel); with sssr, --dry-highlights and the relief pack's materials, glass, shop\n"
            "           windows, car paint and chrome reflect the street too, dry or wet (a Fresnel term, their\n"
            "           own gloss); --reflections-glass F: how strongly (0-2, default 1; 0 off);\n"
            "           --reflections-res half|full: trace at half (default) or the full internal size\n"
            "  --sky-hd: the sky dome and cloud cap are drawn as a procedural night sky in each level's own colours:\n"
            "           a city band along the horizon that does not repeat, sharp stars, slowly drifting clouds (overcast\n"
            "           while it rains) and a moon opposite the shadows; the distant skyline gets varied lit windows and\n"
            "           haze; --no-sky-hd turns it off again\n"
            "  --sharp-faces: character skins (faces and bodies) are upscaled 2x from the game's own textures when\n"
            "           first drawn (DXT1 deblock, edge-clamped bicubic, mild sharpening; on a worker thread), so faces\n"
            "           read sharper in close-ups and cutscenes; a texture pack's version of a skin wins;\n"
            "           --no-sharp-faces turns it off again\n"
            "  --sharp-faces-over-pack: with --sharp-faces and --texture-pack, character skins are upscaled even when\n"
            "           the pack replaces them (everything else, and the same texture on the world, keeps the pack's);\n"
            "           off by default; --no-sharp-faces-over-pack turns it off again\n"
            "  --ssao: ambient occlusion: corners, wall and floor joints and the ground under people and props darken\n"
            "           a little (the opaque world only; effects, lamp halos, rain and the HUD are not darkened);\n"
            "           --ssao-strength F: how much (0.1-2, default 0.8); --ssao-radius F: how far around a point\n"
            "           occluders count, in world units (0.1-8, default 0.8); --no-ssao turns it off again\n"
            "           --ssao-mode classic|gtao: classic (default) or XeGTAO (horizon-based: tighter, darker contact lines,\n"
            "           cleaner open walls); --gtao-quality low|medium|high: its sample count (default high)\n"
            "  --ssgi off|low|medium|high: bounce lighting with --ssao --ssao-mode gtao: neon, lamps and lit walls tint\n"
            "           the walls, floor and people near them (one diffuse bounce of the frame, in screen space; default\n"
            "           off); --ssgi-strength F: how much (0.1-3, default 1)\n"
            "  --lamp-lights: lamps, fire and neon light the ground, walls and people near them (their halos become\n"
            "           point lights; the opaque world only); --lamp-lights-strength F: how much (0.1-3, default 1);\n"
            "           --no-lamp-lights turns it off again\n"
            "  --light-walls 0|1: walls and closed doors (from the level's collision on the disc) stop the light of\n"
            "           --lamp-lights, --level-lights, --fire-light, --char-lighting and --world-lighting, so no floor\n"
            "           behind a wall is lit by a lamp on its other side, and the streaks of --wet-light-streaks and\n"
            "           --damp-streets (default 1; 0: the light passes walls)\n"
            "  --level-lights: the lamp, character and world lighting and the lamp shadows use the level's own lights\n"
            "           (every lamp the level script places, also those without a halo and those just off screen; read\n"
            "           from the game each frame, with the game's colour and range) instead of the halos; with none of\n"
            "           those options it does nothing; --level-lights-max N: lights per frame (1-32, default 32);\n"
            "           --no-level-lights turns it off again\n"
            "  --light-shafts: street lamps hang a soft cone of light in the air below them, and burning barrels a\n"
            "           glow above them (with --fire-light); thicker while it rains; walls and people in front hide it\n"
            "           (works without --lamp-lights too); --light-shafts-strength F: how much (0.1-3, default 1);\n"
            "           --light-shafts-cheap: a coarser, cheaper pass for integrated GPUs; --no-light-shafts turns it off\n"
            "  --volfog: volumetric fog: the lamps', fires' and neon signs' light seen in the night air (a 3D grid\n"
            "           of the air in front of the camera): a light cone under each street lamp, a low mist over the\n"
            "           ground lit only near the lights, rain glowing in the lamps' light; thicker in the rain and on\n"
            "           damp streets, thinner indoors; walls stop it; the game's own fog is kept; replaces --light-shafts;\n"
            "           --volfog-quality low|medium|high: grid size and lights per frame (default medium);\n"
            "           --volfog-strength F: how thick the air is (0.1-3, default 1); --no-volfog turns it off\n"
            "  --char-lighting: the lamp lights light the characters per pixel, following their shape (smooth\n"
            "           normals; works without --lamp-lights too, then only the characters are lit);\n"
            "           --no-char-lighting turns it off again\n"
            "  --hair: with --char-lighting, hair that is no longer flat: each character's hair (found in its own\n"
            "           skin from the head's triangles and the hair's colours, computed on this computer) catches the\n"
            "           lamps, fire and level lights in highlights that run across the strands and break into strands,\n"
            "           gets a weak sheen of the scene's own light from above, and darker lower layers; dark at night,\n"
            "           no plastic shine. --hair-strength F (0.1-3, default 1); --no-hair turns it off\n"
            "  --skin: with --char-lighting, realistic skin: each character's skin texels (face, neck, arms, bare\n"
            "           torso; found in its own skin from the colour of the face, computed on this computer) let the\n"
            "           lamps, fire and level lights through thin parts and edges with the light behind them (ears and\n"
            "           rims turn red), soften into a warm terminator, catch a light sweat sheen (more on the face) and\n"
            "           show deeper creases; only the scene's lights add to it, so night stays dark.\n"
            "           --skin-strength F (0.1-3, default 1); --no-skin turns it off\n"
            "  --eyes: with --char-lighting, living eyes: each character's painted eyes (found in its own skin\n"
            "           from the eye lids' triangles, computed on this computer) catch a small, sharp, wet glint of\n"
            "           the lamps, fire and level lights over an eyeball, a faint catchlight as bright as the scene's\n"
            "           own light (both a small point, never whiter eyes), and slightly darker lid edges; off while a\n"
            "           character blinks. --eyes-strength F (0.1-3, default 1); --no-eyes\n"
            "  --cloth: with --char-lighting, clothing materials: each character's clothes (found in its own\n"
            "           skin by colour and place) react to the lamps, fire and level lights as leather (the vests:\n"
            "           a glossy highlight broken by the creases), denim (a low sheen with a fine weave) or cotton\n"
            "           (matte, a faint sheen at grazing angles); night stays dark.\n"
            "           --cloth-strength F (0.1-3, default 1); --no-cloth turns it off\n"
            "  --hat-fix: hats done right (on by default): the hats on the characters' heads\n"
            "           (top hats, caps, helmets) no longer show lit and shadowed squares from the game's coarse\n"
            "           character shadow, their facets are smoothed (brims and rims stay sharp) and they are lit per\n"
            "           pixel like the characters with --char-lighting (without it they keep the game's light);\n"
            "           --no-hat-fix turns it off\n"
            "  --hat-subdiv: rounder hats (on with --hat-fix): each hat mesh is subdivided once into curved\n"
            "           triangles (9 per triangle), so crowns and brims are round; --no-hat-subdiv turns it off\n"
            "  --world-lighting: the lamp lights light walls, floors, buildings and props per pixel, following their\n"
            "           surfaces, on top of the baked light (works without --lamp-lights too; with it the lamp pass\n"
            "           then lights only the characters); --world-lighting-strength F: how much (0.1-3, default 1);\n"
            "           --no-world-lighting turns it off again\n"
            "  --title-shadow-full: keep the original character shadow as it was (the old look): it also darkens\n"
            "           the light that --char-lighting and --world-lighting add, and it stays under the lamps of\n"
            "           --lamp-shadows; by default it darkens only the game's own light and fades out where a lamp\n"
            "           casts the characters' shadows (one shadow per character, from the lamp)\n"
            "  --fire-light: burning fire barrels cast a warm, gently flickering light on the walls, floor and people\n"
            "           near them (the nearest 8 fires; through --world-lighting and --char-lighting, or --lamp-lights;\n"
            "           alone it does nothing); --fire-light-strength F: how much (0.1-3, default 1); --no-fire-light\n"
            "           turns it off again\n"
            "  --lamp-shadows: characters near a lit street lamp or burning barrel cast a ground shadow pointing away\n"
            "           from it, fading with distance, on top of the original shadow (the 8 lights nearest the camera;\n"
            "           the lamps of --lamp-lights and the fires of --fire-light; works alone too);\n"
            "           --lamp-shadows-strength F: how dark (0.1-1, default 0.6); --no-lamp-shadows turns it off again;\n"
            "           --lamp-shadows-min-angle DEG: lights lower than DEG degrees above the horizon (seen from the\n"
            "           character) cast none, fading in and lengthening up to DEG + 15 (0-60, default 35; 0: no fade);\n"
            "           --lamp-shadows-walls 0|1: walls and closed doors (from the level's collision on the disc) stop\n"
            "           the shadows, so no one's shadow shows through them (default 1; 0: shadows pass walls)\n"
            "  --wall-doors 0|1: a door the game opens stops blocking the lamp shadows and the light of --light-walls\n"
            "           while it is open (read from the game's collision each frame; default 1; 0: every door counts\n"
            "           as closed)\n"
            "  --world-shadows: buildings, props, train tracks and cars cast shadows along the level's own moonlight\n"
            "           (the direction of the original character shadows), as dark as those, never on top of them;\n"
            "           weaker near street lamps and on already dark ground, off indoors (most of the view ahead\n"
            "           under a roof's shadow);\n"
            "           --world-shadows-strength F: share of the original darkness (0.1-1, default 1);\n"
            "           --world-shadows-size N: map texels a side (512, 1024, 2048 or 4096; default 2048);\n"
            "           --world-shadows-range M: metres of the view it covers (15-120, default 45); --no-world-shadows\n"
            "           turns it off again\n"
            "  --contact-shadows: the ground right under and around people and props darkens a little, so they\n"
            "           stand on it (never on the people themselves, effects or the HUD); --contact-shadows-strength F:\n"
            "           how much (0.1-2, default 0.6); --no-contact-shadows turns it off again\n"
            "  --soft-particles: smoke, fire and dust fade out where they meet the ground and walls instead of\n"
            "           ending in a hard line (view-plane billboards only; ground decals, wall tags, lamp glows and\n"
            "           the HUD are unchanged); --soft-particles-distance F: over how many world units in front of a\n"
            "           surface they fade (0.05-4, default 0.5); --no-soft-particles turns it off again\n"
            "  --present: auto (default) presents through a Vulkan swapchain in the SDL window (mailbox, else fifo);\n"
            "           readback reads the frame back and shows it with SDL (also used with --video x11|null, --dump-frame)\n"
            "  --frames-in-flight N: swapchain path, frames the GPU may still run while the next is recorded\n"
            "           (1-3, default 2; 1 = wait for each frame after presenting it; readback always waits)\n"
            "  --no-direct-geometry: vertices through a host copy uploaded at the end of each frame (default:\n"
            "           written straight into the frame's geometry buffer; same picture, less CPU)\n"
            "  --fullscreen: desktop fullscreen (F11 or Alt+Enter toggles)\n"
            "  --refresh-match, --no-refresh-match: in --fullscreen on a display whose rate is not a multiple of 60\n"
            "           (144, 165 Hz), switch it to its highest multiple of 60 at the desktop size (exclusive fullscreen,\n"
            "           restored at exit) so every frame stays up the same time (default on; --no-refresh-match keeps it)\n"
            "  --widescreen: report a 16:9 TV to the title (anamorphic frames, presented and scaled as 16:9)\n"
            "  --movie-hd, --no-movie-hd: play the disc's 1280x720 movies (bik/<name>_hd.bik) drawn by the renderer at\n"
            "           the output resolution, 16:9 (letterboxed at 4:3); default on. --no-movie-hd: the 640x480 files as before\n"
            "  --gamma-ramp, --no-gamma-ramp: apply the title's own gamma ramp (D3DDevice_SetGammaRamp: the in-game\n"
            "           Brightness setting, and the brighter levels of the console) to the finished frame, after every\n"
            "           post effect, as the Xbox does on scanout; default on. --no-gamma-ramp: frames as rendered\n"
            "  --cinema-bars, --no-cinema-bars: keep the title's cutscene letterbox bars solid black over the post\n"
            "           effects (bloom, lamp halos, --filmic) and the scene (drawn after the scene's last draw, before\n"
            "           the subtitles; the same bars at --widescreen give a 2.34:1 picture); default on.\n"
            "           --no-cinema-bars: the bars drawn where the title draws them, under the post effects\n"
            "  --language CODE: the console language the title reads (en, es, fr, de, it; default en):\n"
            "           menu and HUD text (voices are not translated)\n"
            "  --dump-textures DIR: write each unique texture once as DIR/<hash>.png (level 0) + DIR/manifest.csv\n"
            "  --button-icons xbox|kbm|auto|ps-black|ps-white|ps-color: in-game button prompts as the Xbox glyphs\n"
            "           (default), as key caps and mouse drawings for the key map, following the last used device\n"
            "           (keyboard/mouse or controller), or as PlayStation-style icons (ps-*: --pad-icons with the\n"
            "           controller icons always shown); kbm also names the keyboard and mouse where the tutorial texts\n"
            "           name the sticks (auto: unless a controller is connected when the first such text is read)\n"
            "  --pad-icons xbox|ps-black|ps-white|ps-color: the controller's icons (default xbox, the game's glyphs);\n"
            "           ps-* paints PlayStation-style icons (black or white buttons, or black with coloured symbols)\n"
            "           into the game's font atlas at run time, from your own game files: no glyph pack needed\n"
            "  --no-sharp-fonts: the HUD, menu, objective and subtitle letters as shipped. By default (--sharp-fonts)\n"
            "           they are upscaled 4x from the game's own font atlases with rebuilt edges (a worker thread, the\n"
            "           first time each is drawn), crisp at 1080p and above; button icons and other art keep their\n"
            "           look; a texture pack replacing an atlas wins\n"
            "  --no-rumble: no controller vibration\n"
            "  --pad-mappings FILE: SDL controller mappings (default: gamecontrollerdb.txt in the program's folder\n"
            "           and in the current directory, as the launcher's controller set-up writes it)\n"
            "  --pad-slot N=GUID[#K]: the controller with that SDL GUID is player N (1-4) (port N-1), also when\n"
            "           it is plugged in during play; #K: the Kth of identical controllers (same GUID) in the order\n"
            "           they were attached. Repeatable. The others take the ports no controller was given\n"
            "  --sound-pack DIR: play DIR/<cue name>.wav instead of that sound, e.g. DIR/vags/character/...wav as\n"
            "                 tools/audio_extract names them (PCM 8/16/24/32-bit or float WAV, mono/stereo, any rate)\n"            "  --texture-pack DIR: replace textures that have a DIR/<hash>.png (any size; mips generated);\n"
            "                 repeatable (up to 8): the first pack that has a texture wins\n"
            "  --texpack-sync: read texture pack files on the render thread when drawn (debugging; default:\n"
            "                 loader threads, the original texture until the replacement is uploaded)\n"
            "  --no-texpack-prefetch: the loaders read only drawn textures, not those a level load registers\n"
            "  --level-prefetch on|off: with a texture pack, read and upload a level's replacements during its\n"
            "                 load, cache them by content across levels (least recently used freed at the cap)\n"
            "                 and list each level's textures in <hdd>/cache/levels (default on)\n"
            "  --no-tex-race-guard: cache a texture whose guest bytes changed between recording and replaying\n"
            "                 the draw (the old behaviour; [tex-race] still logs it). Default: skip that draw\n"
            "                 once and cache nothing under its recorded key (a texture rewritten every\n"
            "                 frame is then drawn from its current bytes)\n"
            "  --texpack-budget MB: texture pack cache cap (default 1024); lowered to what the driver's\n"
            "                 memory budget leaves when it reports one (VK_EXT_memory_budget)\n"            "  --mesh-pack DIR: replace skinned character meshes that have a DIR/<hash>.wmesh (smoothed\n"
            "                 meshes from tools/mesh_pack; same skeleton and weights; off by default)\n"
            "  --mesh-log: print the content hash of each new draw (to find a mesh for a mod's mesh overrides,\n"
            "                 docs/MODDING.md); a mod's own overrides are logged as [meshover] lines anyway\n"            "  --no-render-targets: skip the title's own render-target passes (character shadows)\n"
            "  --no-frame-blur: turn off the game's own frame-feedback blur (rage mode trail, the haze of a mace or\n"
            "      pepper spray in the face, hot steam), which redraws the previous frame over the new one (default on)\n"
            "  --shadow-scale N: character shadow maps at N x their size (1-4, default 1 = follows the render size)\n"
            "  --shadow-soft: soft shadow edges (3x3 percentage-closer filtering instead of 2x2)\n"
            "  --shadow-range: character shadows no longer fall on floors far below the character (they fade out\n"
            "           from --shadow-range-dist F to 1.5 x F world units below it; 0.5-50, default 3); --no-shadow-range\n"
            "           turns it off again (default: the original shadows)\n"
            "  --hud-hd: restyle the target health bar (top right): thicker, dark outline, gradient fill, dark\n"
            "           translucent track; --hud-hd-height F scales its height (0.5-4, default 1); --hud-hd-opacity F its\n"
            "           opacity (0-1, default 1; the outline at sqrt(F)); --hud-hd-frost N blurs the background inside the bar's\n"
            "           own rect (radius 0-32 in 640x480 pixels, default 0 = off); each also turns it on; the rage meter is unchanged\n"
            "  --hud-hd-glass: glass target bar preset (recommended): track opacity 0.25 with a light frosted tint, fill opacity\n"
            "           0.6 made vivid (vivid 1), frost 4, thin soft frame outline; options after it override its values\n"
            "  --hud-hd-track-opacity F, --hud-hd-fill-opacity F: opacity (0-1) of the empty part and of the fill (default:\n"
            "           --hud-hd-opacity); --hud-hd-vivid F (0-1, default 0): raises the fill's saturation and brightness by\n"
            "           F x (1 / fill opacity - 1) so a translucent fill keeps its color; each turns on --hud-hd in the glass\n"
            "           style (empty part only from the fill's edge, frame outline instead of the filled outline)\n"
            "  --hud-glass: draw the target health bar as a low-opacity bar on a frosted-glass panel (blurred background,\n"
            "           light tint and rim); --hud-glass-blur N (blur radius in 640x480 pixels, 1-32, default 4),\n"
            "           --hud-glass-opacity F (bar opacity 0-1, default 0.45), --hud-glass-tint AARRGGBB (default 30ffffff);\n"
            "           each tunable also turns it on\n"
            "  --spawn-scale N: experiment, 1-8 (default 1 = no patch): multiply the scripts' spawner max-alive\n"
            "      values (clamped to 16 per spawner) and SetSpawnMax/SetCopSpawnMax (clamped to the 60-slot\n"
            "      character pool); with it or --stats, a [pool] line per 60 frames (occupied slots, spawned alive)\n"
            "  --fx-scale N: 1-4, decimals allowed (default 1 = no patch): multiply the title's effect pools, i.e.\n"
            "      particle tasks (blood, debris, smoke, sparks; 1400 + 100), glass (100) and sound tasks (256), and\n"
            "      the free particle tasks below which blood fades (512), so blood survives fights; and the particle\n"
            "      sprites drawn per frame (part_page1 640/400/200, part_fire 256; at most 2048). Implies --big-heap\n"
            "  --big-heap: main heap 45.4 -> 77.5 MB inside the 128 MB contiguous window (Xbox: below 64 MB), so the\n"
            "      larger pools leave the streamed world's budget as it is\n"
            "  --no-rate-fixes: keep the HUD minigames' per-frame steps authored for 30 fps (default: halved for 60 Hz)\n"
            "  --spray-time PCT: graffiti spray can duration in percent of the Xbox one, 100..200 (default 110;\n"
            "                    applied with the rate fixes)\n"
            "  --skip-intro: start at the title screen without the boot movies (logo, plogo, l1_in); level\n"
            "      movies and the title's attract loop still play (default off; --no-skip-intro)\n"
            "  --draw-distance N: far plane and fog end up to N times the title's (1..4, default 1 = as shipped)\n"
            "  --prop-detail N: small props drawn up to N times as far before they are culled (1..8, default 1)\n"
            "  --fog-breathing: keep the title's far-plane pull-in after slow frames (default: steady fog)\n"
            "  --no-text-fixes: read the string tables as shipped (default: known mistranslations corrected,\n"
            "                   e.g. the Spanish gym's \"Sentadillas\" for Sit Ups -> \"Abdominales\")\n"
            "  --no-script-fixes: run the level scripts as shipped (default: small Lua fixes keep missions from\n"
            "                     soft-locking, e.g. Set Up's machete guy made hostile when his cutscene does not start)\n"
            "  --dialogue-volume DB, --music-volume DB, --sfx-volume DB: -40 to +12 (default 0): extra level on\n"
            "      the game's spoken lines (crew, enemies, story lines, in-engine cutscenes), its music, or every\n"
            "      other sound; movies (and the dialogue inside them) are unchanged\n"
            "  --sfx-3d-volume DB: experiment, -40 to +12 (default 0): extra level on every positioned (3D)\n"
            "      sound effect; dialogue, music, movies and 2D sounds are unchanged\n"
            "  --audio-lead-ms N: audio queued ahead on the SDL device, 0..500 ms (default 50; 0 = the earlier\n"
            "      host-clocked queue, which underruns on most devices)\n"
            "  --no-xact-controls: ignore the cue volume/fade/pitch controls the title sets (A/B test; default: applied)\n"
            "  --no-rolloff-curves: attenuate positioned (3D) sounds by plain inverse distance instead of the\n"
            "      sound bank's rolloff curves the Xbox uses (A/B test; much quieter voices at a few meters)\n"
            "  --audio-levels: diagnostic, one [levels] line per second of mixed audio: RMS dBFS of the mix, the\n"
            "      reverb return and each sound playing (XACT cue index in hex, 100000 = movie audio)\n"
            "  --no-xbox-headroom: play sound effects, music and dialogue without the console's -6 dB voice\n"
            "      headroom (A/B test; default: applied as on the Xbox, movies at full level)\n"
            "  --reverb, --no-reverb: the title's I3DL2 room reverb on positioned (3D) sounds (default on;\n"
            "      --no-reverb: dry, as before)\n"
            "  --doppler, --no-doppler: Doppler pitch on positioned (3D) sounds from their and the listener's\n"
            "      motion (default on)\n"
            "  --rear-cue, --no-rear-cue: a low-pass on 3D sounds behind the listener (default on)\n"
            "  --audio-hrtf, --no-audio-hrtf: headphones: binaural 3D sound (time and level difference between the\n"
            "      ears, head shadow, a slight height cue) instead of the stereo pan (default off: speakers); with the\n"
            "      Steam Audio library next to the program: its measured HRTF (clear front/back and height cues)\n"
            "  --steam-audio, --no-steam-audio: use the Steam Audio library (libphonon.so / phonon.dll) next to the\n"
            "      program when an option needs it (default on; off or missing: the built-in models, as before):\n"
            "      --audio-hrtf, --audio-occlusion (the level's walls, floors and ceilings with their materials\n"
            "      instead of the wall map: concrete and brick block more than wood, glass or a fence) and\n"
            "      --audio-reverb-geo\n"
            "  --audio-reverb-geo, --no-audio-reverb-geo: location-based reverb: the room reverb's length and tone\n"
            "      follow the place (an alley, a room, an open street) from the level's geometry, traced once per\n"
            "      spot and kept (default on; needs the Steam Audio library and --reverb)\n"
            "  --audio-pathing, --no-audio-pathing: sound around corners: a 3D sound behind a wall that has a way\n"
            "      round it (a corner, a doorway, an open door) comes through that opening, from its direction and\n"
            "      louder than through the wall; closed doors block the way (default on; needs the Steam Audio\n"
            "      library and --audio-occlusion; each level's paths are worked out once in the background and\n"
            "      kept in the --hdd folder's acoustics/ subfolder, safe to delete)\n"
            "  --audio-air, --no-audio-air: far 3D sounds duller, not only quieter (air absorption; default on)\n"
            "  --audio-occlusion, --no-audio-occlusion: 3D sounds behind walls quieter and muffled, from the level's\n"
            "      walls on the disc (the map of --lamp-shadows walls; default on)\n"
            "  --resampler linear|sinc: how voices are converted to the 48 kHz mix (default sinc: 64-tap\n"
            "      windowed sinc, images about -90 dB, flat to 0.45 of the source rate; linear: the earlier\n"
            "      linear interpolation, for A/B listening)\n"
            "  --no-frame-lock: QueryPerformanceCounter on real time (default: one 60 Hz period per presented frame,\n"
            "      whole periods towards real time when behind, so the 30 Hz world updates on every other frame)\n"
            "  --sim60: experimental, off by default: the world (characters, AI, cameras) updates on every\n"
            "      1/60 s step with half the time step, instead of every other step (same game speed)\n"
            "  --no-sim60-ai-clocks: with --sim60, leave the AI's periodic checks counted in world updates\n"
            "      (target re-check, re-pathing, Wounded, Backoff, brain scans) at their 30 Hz counts (A/B)\n"
            "  --smart-allies: the crew AI (the player's gang) spreads over the enemies instead of queueing on one,\n"
            "      covers the player and held Warriors, circles less and switches to a free enemy when refused an\n"
            "      attack slot; enemies unchanged\n"
            "      (--no-smart-allies: off, the default)\n"
            "  --enemy-tactics: enemies fighting the player or the crew take turns attacking (an attacker that held\n"
            "      its slot for 3 s steps back when another waits), surround the player from its sides and back while\n"
            "      they wait, break a long block, grab more and use heavier combos against a player who stands still,\n"
            "      pick on a weakened Warrior and help a gang mate who is held; same reaction times and attacker limits\n"
            "      (--no-enemy-tactics: off, the default)\n"
            "  --ai-debug: diagnostic: [ai] lines when the crew or the characters fighting it change goal, action\n"
            "      or target, and per 10 s of game time the crew's time near enemies without engaging, damage and hits taken;\n"
            "      [tactics] lines with the enemies' attack decisions on the player and the crew, their attack ids and\n"
            "      the player's hits taken (with --enemy-tactics also its turns, flanking moves and boosts)\n"
            "  --combat-debug: development: draw the characters' hurt (green), strike (red) and weapon (orange)\n"
            "      volumes and recent contacts (yellow), and log [combat] lines: strike windows with their hit tests\n"
            "      and closest approach, startup/active/recovery frames per move, contacts and damage\n"
            "  --rate-probe FILE[:MS]: diagnostic: every MS (default 100) of game time, snapshot the XBE's .data\n"
            "      section (and each --rate-probe-range: ADDR:SIZE, or @PTR[+OFF]:SIZE behind a pointer) into FILE,\n"
            "      for tools/ratediff (systems that count world updates instead of time under --sim60)\n"
            "  --scene ID: play one in-engine scene alone (Rockstar scene test mode), e.g. l84_c7; implies --warp\n"
            "  --warp MISSION[:CHECKPOINT]: loading a profile starts story mission 1-23 (or a level name such as\n"
            "      level81, level95 = the Coney hub) at the checkpoint (default 1) instead of the hub; once only,\n"
            "      later loads are the game's own. Saving is disabled for the whole session.\n"
            "  --goto PLACE: skip the boot movies, press through the menus and go to PLACE: title, hideout[:C],\n"
            "      coney, gym, rumble (everything unlocked in memory), aotn[:L[:C]] (Armies of the Night 60-64),\n"
            "      m<N>[:C] (= --warp N[:C]); --goto list prints them with the state each leaves. The save on disk is\n"
            "      never written (--goto-allow-save allows it for the targets that do not redirect a level load)\n"
            "  --lua FILE / --lua-exec CODE: run Lua in the game's state at the engine's next C->Lua call (lua_call);\n"
            "      results and errors go to the log as [lua] lines; '=EXPR' is 'return EXPR'. --lua-at FRAME and\n"
            "      --lua-retry (run again each frame until the chunk returns non-nil) apply to the following chunks.\n"
            "      --lua-pipe PATH: every line appended to the file PATH runs (headless console);\n"
            "      --dev-console: every line typed on standard input runs\n"
            "  --control PATH: (Linux developer builds) live game control on the Unix socket PATH: lua, input, warp,\n"
            "      wait, capture, stats, opt, ping, quit, one JSON reply per command (tools/xbctl is the client);\n"
            "      --control-idle SEC (default 600) quits after SEC seconds without a command\n"
            "  --fast-boot: (Linux developer builds) run unpaced until the first frame of the --goto/--warp level\n"
            "      (the game clock still steps one 60 Hz period per frame), then at the normal pace\n"
            "  --mod ID[,ID...]: run these Lua mods (repeatable; all = every mod found) from the mods folder\n"
            "      (--mods DIR; default mods/ next to the program). Each mod is a folder with a mod.ini; its Lua runs\n"
            "      in the game's state after the scripts named in mod.ini load (docs/MODDING.md). A mod whose Lua\n"
            "      fails is logged ([mods] ID: error ...) and turned off. --no-mods runs none; --mods-trace logs\n"
            "      every script the game loads ([mods] script NAME). A mod's assets.ini is built from the disc into\n"
            "      <hdd>/mods-cache at its first load; --rebuild-mod-assets [ID] builds it again\n", prog_name);
    exit(2);
}

static void map_image(const Xbe *x) {
    uint32_t end = x->base + x->image_size;
    for (uint32_t i = 0; i < x->section_count; i++) {
        uint32_t e = x->sections[i].vaddr + x->sections[i].vsize;
        if (e > end) end = e;
    }
    uint32_t size = (end - x->base + 0xFFF) & ~0xFFFu;
    if (!guest_alloc_at(x->base, size)) {
        fprintf(stderr, "cannot map XBE image at %08x (+%x)\n", x->base, size);
        exit(1);
    }
    memcpy(G(x->base), x->file, x->headers_size);
    for (uint32_t i = 0; i < x->section_count; i++) {
        const XbeSection *s = &x->sections[i];
        memcpy(G(s->vaddr), x->file + s->raw_addr, s->raw_size < s->vsize ? s->raw_size : s->vsize);
    }
    printf("[load] image %08x-%08x, %u sections, entry %08x (%s key), thunks at %08x\n", x->base, x->base + size,
           x->section_count, x->entry, x->key, x->thunk_table);
}

static void patch_thunks(const Xbe *x) {
    uint32_t *slot = G(x->thunk_table);
    int n = 0, impl = 0;
    for (; slot[n]; n++) {
        uint32_t ord = slot[n] & 0x7FFFFFFF;
        if (ord >= KEXPORT_COUNT) { fprintf(stderr, "bad import ordinal %u\n", ord); exit(1); }
        impl += kernel_is_implemented(ord);
    }
    printf("[load] %d kernel imports, %d implemented, %d fail-loud stubs:", n, impl, n - impl);
    for (int i = 0; i < n; i++) {
        uint32_t ord = slot[i] & 0x7FFFFFFF;
        if (!kernel_is_implemented(ord)) printf(" %s", kexports[ord].name);
        slot[i] = kernel_resolve(ord);
    }
    printf("\n");
}

/* Rewrites the title's fs:[KPCR] accesses (fspatch.h): required on Windows, optional on Linux
 * (--patch-fs) where %fs already is the KPCR. Runs after every other code patch. */
static void patch_fs(const Xbe *x, int force) {
    uint32_t disp;
    if (!plat_fs_patch_required(&disp) && !force) return;
    int total = 0;
    for (uint32_t i = 0; i < x->section_count; i++) {
        const XbeSection *s = &x->sections[i];
        if (!fspatch_section_is_code(s->name, s->flags)) continue;
        uint32_t n = s->raw_size < s->vsize ? s->raw_size : s->vsize;
        int r = fspatch_apply(G(s->vaddr), n, 1, disp);
        if (r < 0) { fprintf(stderr, "fs patching: out of stub memory\n"); exit(1); }
        total += r;
    }
    printf("[load] fs: KPCR accesses rewritten: %d (KPCR pointer at fs:[%#x])\n", total, disp);
}

/* After plat_image_mapped replaced the XBE magic at the image base, code that compares that
 * dword with the "XBEH" constant must see the new value (Cxbx-Reloaded's
 * CxbxrKrnlXbePatchXBEHSig does the same). */
static void patch_xbeh_constants(const Xbe *x) {
    int n = 0;
    uint32_t now = *(uint32_t *)G(x->base);
    for (uint32_t i = 0; i < x->section_count; i++) {
        const XbeSection *s = &x->sections[i];
        if (!fspatch_section_is_code(s->name, s->flags)) continue;
        uint8_t *p = G(s->vaddr);
        uint32_t len = s->raw_size < s->vsize ? s->raw_size : s->vsize;
        for (uint32_t o = 0; o + 4 <= len; o++)
            if (!memcmp(p + o, "XBEH", 4)) { memcpy(p + o, &now, 4); n++; }
    }
    printf("[load] XBEH constants in code updated to the header now at %08x: %d\n", x->base, n);
}

/* --button-icons auto: the tutorial texts name the keys unless a controller is connected when the
 * first stick phrase is read, or was the last device used (asked once; hle_textfix.h). */
static int controls_text_gate(void) { return !input_controller_connected() && input_last_device() != INPUT_DEVICE_PAD; }

/* --rate-probe: the XBE's .data section (with its .bss) first, then the extra ranges. */
static void rate_probe_start(const Xbe *x, RateProbeOptions *o, int sim60_active) {
    for (uint32_t i = 0; i < x->section_count; i++) {
        const XbeSection *s = &x->sections[i];
        if (strcmp(s->name, ".data") || s->vaddr & 3 || s->vsize < 4) continue;
        memmove(o->ranges + 1, o->ranges, (size_t)o->nranges * sizeof *o->ranges);
        o->ranges[0] = (RateProbeRange){s->vaddr, s->vsize & ~3u, 0, 0};
        o->nranges++;
        break;
    }
    FILE *f = fopen(o->path, "wb");
    if (!f) { perror(o->path); exit(2); }
    rateprobe_active = rateprobe_open(f, o->interval_ms, sim60_active ? RATEPROBE_FLAG_SIM60 : 0, o->ranges, o->nranges);
    if (!rateprobe_active) { fprintf(stderr, "--rate-probe: cannot write %s\n", o->path); exit(2); }
    printf("[rateprobe] %s: every %u ms of game time, %d ranges:", o->path, o->interval_ms, o->nranges);
    for (int i = 0; i < o->nranges; i++) {
        const RateProbeRange *r = &o->ranges[i];
        if (r->deref) printf(" [%08x]+%x+%x", r->addr, r->off, r->size);
        else printf(" %08x+%x", r->addr, r->size);
    }
    printf("%s\n", sim60_active ? " (sim60)" : "");
}

static int probe_threads;
static void probe(const char *phase) { if (probe_threads) plat_thread_probe(phase); }

static void add_hook(const char *spec) {
    uint32_t addr = 0;
    if (!strncmp(spec, "0x", 2)) addr = strtoul(spec, NULL, 16);
    else addr = symbol_lookup(spec);
    if (!addr) { fprintf(stderr, "hook '%s': symbol not found (pass --symbols)\n", spec); exit(2); }
    int idx = d3d_hook_add(addr, spec);
    /* Per-hook stub: push $idx; jmp d3d_hook_entry */
    uint8_t *stub = plat_alloc_exec(16);
    stub[0] = 0x68; memcpy(stub + 1, &idx, 4);
    int32_t rel = (int32_t)((uintptr_t)d3d_hook_entry - (uintptr_t)(stub + 10));
    stub[5] = 0xE9; memcpy(stub + 6, &rel, 4);
    uint8_t *p = G(addr);
    rel = (int32_t)((uintptr_t)stub - (uintptr_t)(p + 5));
    p[0] = 0xE9; memcpy(p + 1, &rel, 4);
    printf("[load] hook %s at %08x\n", spec, addr);
}

/* Replaces a guest function with "mov eax, RET; ret POP" (used to skip hardware init
 * that is out of PoC scope, e.g. USB/OHCI in XInitDevices). spec = SYMBOL:POPBYTES[:RET] */
static void add_skip(const char *spec) {
    char name[256];
    snprintf(name, sizeof name, "%s", spec);
    char *colon = strchr(name, ':');
    int pop = colon ? atoi(colon + 1) : 0;
    char *colon2 = colon ? strchr(colon + 1, ':') : NULL;
    uint32_t ret = colon2 ? strtoul(colon2 + 1, NULL, 0) : 0;
    if (colon) *colon = 0;
    uint32_t addr = !strncmp(name, "0x", 2) ? strtoul(name, NULL, 16) : symbol_lookup(name);
    if (!addr) { fprintf(stderr, "skip '%s': symbol not found\n", name); exit(2); }
    uint8_t *p = G(addr);
    p[0] = 0xB8; memcpy(p + 1, &ret, 4);
    p[5] = 0xC2; p[6] = pop & 0xFF; p[7] = pop >> 8;
    printf("[load] skip %s at %08x (returns %x, pops %d bytes)\n", name, addr, ret, pop);
}

#ifndef XBRUN_VERSION
#define XBRUN_VERSION "unknown"
#endif

/* --audio-reverb-geo: every XACTEngineDoWork, the listener's pose to the
 * acoustics and the place's latest RT60 eased (0.5 s time constant) into the reverb's decay and HF ratio,
 * kept within GEO_DECAY_MIN..GEO_DECAY_MAX times the title's decay (open streets keep some of the
 * city's tail; closed rooms do not ring forever); without a result (no scene) the title's again. */
#define GEO_DECAY_MIN 0.4f
#define GEO_DECAY_MAX 2.5f
#define GEO_EASE_S 0.5
static int geo_reverb_on;
static void geo_listener(const float pos[3], const float front[3], const float top[3]) {
    ac_listener(pos, front, top);
    if (!geo_reverb_on) return;
    static uint32_t seq;
    static float tgt_d, tgt_r, cur_d, cur_r;
    static double t_last;
    static int have;
    ReverbI3dl2 tp;
    audio_get_i3dl2(&tp);
    float rt[3], d = tp.decay_time, r = tp.decay_hf_ratio;
    const uint32_t q = ac_reverb(rt);
    if (q && q != seq) {
        seq = q;
        if (!ac_reverb_to_i3dl2(rt, tp.hf_reference, &tgt_d, &tgt_r)) {
            tgt_d = fmaxf(GEO_DECAY_MIN * tp.decay_time, fminf(GEO_DECAY_MAX * tp.decay_time, tgt_d));
            have = 1;
        }
    } else if (!q) have = 0;
    if (have) d = tgt_d, r = tgt_r;
    const double t = hle_xapi_seconds(), dt = t - t_last;
    t_last = t;
    if (!(cur_d > 0) || dt < 0 || dt > 1) cur_d = d, cur_r = r;
    else {
        const float k = (float)(1 - exp(-dt / GEO_EASE_S));
        cur_d += (d - cur_d) * k;
        cur_r += (r - cur_r) * k;
    }
    audio_set_reverb_geo(1, cur_d, cur_r);
}

/* Mod menus and music (hle_modmenu.h): a sound name's cue index from the disc's name table
 * (warriors.glr, src/sndpack.h), read on first use. */
static const char *menu_disc;
static SndName *menu_names;
static int menu_nnames = -2;
/* A mod's own sounds (modsound.h) on the host mixer's voices. */
static void *snd_voice_new(int channels, uint32_t rate, ModSoundFill fill, void *ctx) { return voice_new(channels, rate, (AudioFill)fill, ctx); }
static void snd_voice_free(void *v) { voice_free(v); }
static void snd_voice_play(void *v, int on) { voice_play(v, on); }
static int snd_voice_playing(const void *v) { return voice_playing(v); }
static void snd_voice_gains(void *v, const float g[2][2]) { voice_set_gains(v, g); }
static void mods_audio_tick(void) {
    hle_modmenu_tick();
    modsound_tick();
}

/* F8 asks for a mod's menu (ModMenuKey, hle_modmenu.h) unless the player bound F8 to a control. */
static void mod_menu_key(int key, int down) {
    if (key == INPUT_KEY_F1 + 7 && down && input_key_control(key) < 0) hle_modmenu_request("F8");
}

static int32_t menu_cue_of(const char *name) {
    if (menu_nnames == -2) {
        menu_nnames = menu_disc ? sndpack_names_load(menu_disc, &menu_names) : -1;
        if (menu_nnames < 0) printf("[menu] no sound name table in the disc folder: mod music unavailable\n");
    }
    const uint32_t crc = sndpack_crc32(name);
    for (int i = 0; i < menu_nnames; i++)
        if (menu_names[i].crc == crc) return (int32_t)menu_names[i].cue;
    return -1;
}

int main(int argc, char **argv) {
    plat_init("xbrun");
    dlog_init();   /* uptime origin of the [crash-context] line */
    ktx2_init();   /* before the texture-pack loader threads: <hash>.ktx2 pack files (src/ktx2.h) */
    texpack_set_ktx2(ktx2_transcode);
    set_prog_name(argv[0]);
    for (int i = 1; i + 1 < argc; i++)   /* --goto list needs no XBE or disc */
        if (!strcmp(argv[i], "--goto") && !strcmp(argv[i + 1], "list")) { goto_list(stdout); return 0; }
    for (int i = 1; i < argc; i++)   /* --render64-selftest [--render64-helper PATH]: neither */
        if (!strcmp(argv[i], "--render64-selftest")) {
            const char *given = NULL;
            for (int j = 1; j + 1 < argc; j++)
                if (!strcmp(argv[j], "--render64-helper")) given = argv[j + 1];
            char path[4200];
            return r64_selftest_run(r64_input_find_helper(given, path, sizeof path));
        }
    if (argc < 3) usage();
    const char *xbe_path = argv[1], *disc = argv[2], *syms = NULL, *hdd = "hdd";
    const char *sound_pack = NULL;
    const char *hooks[16], *skips[16];
    int nhooks = 0, nskips = 0, default_skips = 1, fake_mmio = 0, patch_fs_flag = 0, hle_audio = 1, hle_d3d = 1, hle_input = 1, trace = 0;
    const char *input_script = NULL, *input_config = NULL;
    double fx_scale = 1;
    int big_heap = 0;
    int smart_allies = 0;
    int enemy_tactics = 0;
    int audio_log = -1, spawn_scale = 1, rate_fixes = 1, skip_intro = 0, text_fixes = 1, script_fixes = 1, sim60 = 0, combat_debug = 0, spray_time = RATEFIX_SPRAY_DEFAULT;
    const char *audio_backend = "sdl", *audio_dump = NULL;
    float sfx_3d_db = 0, class_db[HLE_AUDIO_NCLASSES] = {0};
    int doppler = 1, rear_cue = 1, audio_hrtf = 0, audio_air = 1, audio_occl = 1, steam_audio = 1, reverb_geo = 1, audio_pathing = 1;
    int xact_controls = 1, glass_args, bloom_args, filmic_args, hdr_args, ae_args, dof_args, rain_args, ssao_args, contact_args, lamp_args, level_args, lshadow_args, wshadow_args, shaft_args, vf_args, streak_args, mat_args, damp_args, nrefl_args, sssr_args, world_args, fire_args, soft_args, range_args, rp;
    RateProbeOptions rate_probe = {.interval_ms = RATEPROBE_DEFAULT_INTERVAL_MS};
    WarpTarget warp = {0};
    GotoPlan goto_plan = {0};
    int goto_on = 0, goto_allow_save = 0, goto_r, lua_r;
    int warp_on = 0, warp_r, dd_r;
    DrawDistOptions draw_dist = DRAWDIST_DEFAULTS;
    char warp_err[192];
    for (int i = 3; i < argc; i++) {
        const int r64in = r64_input_option(argc, argv, &i);   /* --render-process helper-io, --render64-helper, --render64-inject, --render64-pad-poll-us */
        if (r64in < 0) return 2;
        if (r64in) continue;
        const int rec = render_rec_option(argc, argv, &i);   /* --record-renderer, --renderer-traffic, --mem-map-every */
        if (rec < 0) return 2;
        if (rec) continue;
        if (!strcmp(argv[i], "--symbols") && i + 1 < argc) syms = argv[++i];
        else if (!strcmp(argv[i], "--hook") && i + 1 < argc && nhooks < 16) hooks[nhooks++] = argv[++i];
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc && nskips < 16) skips[nskips++] = argv[++i];
        else if (!strcmp(argv[i], "--no-default-skips")) default_skips = 0;
        else if (!strcmp(argv[i], "--no-hle-audio")) hle_audio = 0;
        else if (!strcmp(argv[i], "--no-hle-d3d")) hle_d3d = 0;
        else if (!strcmp(argv[i], "--no-hle-input")) hle_input = 0;
        else if (!strcmp(argv[i], "--input-script") && i + 1 < argc) input_script = argv[++i];
        else if (!strcmp(argv[i], "--input-config") && i + 1 < argc) input_config = argv[++i];
        else if (!strcmp(argv[i], "--button-icons") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "xbox")) hle_d3d_button_icons = HLE_ICONS_XBOX;
            else if (!strcmp(v, "kbm")) hle_d3d_button_icons = HLE_ICONS_KBM;
            else if (!strcmp(v, "auto")) hle_d3d_button_icons = HLE_ICONS_AUTO;
            else if (kbm_ps_style(v) > 0) hle_d3d_button_icons = HLE_ICONS_XBOX, hle_d3d_pad_icons = kbm_ps_style(v);
            else { fprintf(stderr, "--button-icons: xbox, kbm, auto, ps-black, ps-white or ps-color\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--pad-icons") && i + 1 < argc) {
            if ((hle_d3d_pad_icons = kbm_ps_style(argv[++i])) < 0) {
                fprintf(stderr, "--pad-icons: xbox, ps-black, ps-white or ps-color\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--no-rumble")) input_rumble_enabled = 0;
        else if (!strcmp(argv[i], "--pad-mappings") && i + 1 < argc) video_pad_mappings = argv[++i];
        else if (!strcmp(argv[i], "--pad-slot") && i + 1 < argc) {
            char err[160];
            if (input_pad_slot_parse(argv[++i], err, sizeof err)) { fprintf(stderr, "--pad-slot: %s\n", err); return 2; }
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) hle_d3d_frame_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-frame") && i + 1 < argc) hle_d3d_dump_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) hle_d3d_dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-from") && i + 1 < argc) hle_d3d_dump_from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stats")) hle_d3d_stats = 1;
        else if (!strcmp(argv[i], "--ai-debug")) hle_aidbg_active = 1;
        else if (!strcmp(argv[i], "--load-profile")) load_prof_on = 1;
        else if (!strcmp(argv[i], "--tri-check")) hle_d3d_tri_check = 1;
        else if (!strcmp(argv[i], "--no-frame-lock")) hle_xapi_frame_lock = 0;
        else if (!strcmp(argv[i], "--fx-scale") && i + 1 < argc) {
            if (fx_scale_parse(argv[++i], &fx_scale)) { fprintf(stderr, "--fx-scale: a number from 1 to 4\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--big-heap")) big_heap = 1;
        else if (!strcmp(argv[i], "--spawn-scale") && i + 1 < argc) {
            if ((spawn_scale = spawn_scale_parse(argv[++i])) < 0) usage();
        }
        else if (ratefix_parse_arg(argv[i], &rate_fixes)) {}
        else if (skipintro_parse_arg(argv[i], &skip_intro)) {}
        else if (sim60_parse_arg(argv[i], &sim60)) {}
        else if (allies_parse_arg(argv[i], &smart_allies)) {}
        else if (tactics_parse_arg(argv[i], &enemy_tactics)) {}         else if (combat_debug_parse_arg(argv[i], &combat_debug)) {}
        else if ((warp_r = warp_parse_arg(argc, argv, &i, &warp, &warp_on, warp_err, sizeof warp_err))) {
            if (warp_r < 0) { fprintf(stderr, "%s\n", warp_err); return 2; }
        }
        else if ((goto_r = goto_parse_arg(argc, argv, &i, &goto_plan, &goto_on, &goto_allow_save, warp_err, sizeof warp_err))) {
            if (goto_r < 0) { fprintf(stderr, "%s\n", warp_err); return 2; }
        }
        else if ((lua_r = gamectl_parse_arg(argc, argv, &i, warp_err, sizeof warp_err))) {
            if (lua_r < 0) { fprintf(stderr, "%s\n", warp_err); return 2; }
        }
        else if ((lua_r = devlua_parse_arg(argc, argv, &i, warp_err, sizeof warp_err))) {
            if (lua_r < 0) { fprintf(stderr, "%s\n", warp_err); return 2; }
        }
        else if ((lua_r = mods_parse_arg(argc, argv, &i, warp_err, sizeof warp_err))) {
            if (lua_r < 0) { fprintf(stderr, "%s\n", warp_err); return 2; }
        }
        else if (!strcmp(argv[i], "--spray-time") && i + 1 < argc) {
            if (!ratefix_parse_spray(argv[++i], &spray_time)) usage();
        }
        else if ((dd_r = drawdist_parse_arg(argc, argv, &i, &draw_dist))) {
            if (dd_r < 0) { fprintf(stderr, "--draw-distance: 1 to 4, --prop-detail: 1 to 8\n"); return 2; }
        }
        else if (textfix_parse_arg(argv[i], &text_fixes)) {}
        else if (scriptfix_parse_arg(argv[i], &script_fixes)) {}
        else if (!strcmp(argv[i], "--video") && i + 1 < argc) hle_d3d_video = argv[++i];
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) hle_d3d_renderer = argv[++i];
        else if (!strcmp(argv[i], "--resolution") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &hle_d3d_res_w, &hle_d3d_res_h) != 2 || hle_d3d_res_w < 1 || hle_d3d_res_h < 1 ||
                hle_d3d_res_w > 16384 || hle_d3d_res_h > 16384)
                usage();
        } else if (!strcmp(argv[i], "--render-scale") && i + 1 < argc) {
            hle_d3d_render_scale = atoi(argv[++i]);
            if (hle_d3d_render_scale < 1 || hle_d3d_render_scale > 8) usage();
        } else if (!strcmp(argv[i], "--msaa") && i + 1 < argc) hle_d3d_msaa = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aniso") && i + 1 < argc) hle_d3d_aniso = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--present") && i + 1 < argc) hle_d3d_present = argv[++i];
        else if (!strcmp(argv[i], "--frames-in-flight") && i + 1 < argc) {
            hle_d3d_frames_in_flight = atoi(argv[++i]);
            if (hle_d3d_frames_in_flight < 1 || hle_d3d_frames_in_flight > 3) usage();
        }        else if (!strcmp(argv[i], "--fullscreen")) hle_d3d_fullscreen = 1;
        else if (refresh_match_parse_arg(argv[i], &video_refresh_match)) {}
        else if ((bloom_args = bloom_arg(&hle_d3d_bloom, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (bloom_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-3, threshold 0-1)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += bloom_args - 1;
        }
        else if ((filmic_args = filmic_arg(&hle_d3d_filmic, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (filmic_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0-2, grain 0-1, vignette 0-1, LUT a file name)\n", argv[i],
                        i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += filmic_args - 1;
        }
        else if ((hdr_args = hdr_arg(&hle_d3d_hdr, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (hdr_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (--hdr off|auto|on, --hdr-peak 400-10000, --hdr-paper-white 80-500, --saturation 0.5-2)\n",
                        argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += hdr_args - 1;
        }
        else if ((dof_args = dof_arg(&hle_d3d_dof, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (dof_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.25-2, speaker 0 or 1)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += dof_args - 1;
        }
        else if ((ae_args = auto_exposure_arg(&hle_d3d_auto_exposure, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (ae_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0-2)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += ae_args - 1;
        }
        else if ((rain_args = rain_arg(&hle_d3d_rain, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (rain_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (density 0.25-4, wet strength 0.1-4)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += rain_args - 1;
        }
        else if ((ssao_args = ssao_arg(&hle_d3d_ssao, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (ssao_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-2, radius 0.1-8, ssgi strength 0.1-3)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += ssao_args - 1;
        }
        else if ((contact_args = contact_arg(&hle_d3d_contact, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (contact_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-2)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += contact_args - 1;
        }
        else if (sky_arg(&hle_d3d_sky, argv[i], NULL)) {}
        else if (sharp_font_arg(&hle_d3d_sharp_fonts, argv[i])) {}
        else if (sharp_faces_arg(&hle_d3d_sharp_faces, argv[i], NULL)) {}
        else if ((rp = neon_glow_arg(&hle_d3d_neon_glow, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (rp < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-3, bloom 0-1, flicker 0-0.3)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += rp - 1;
        }
        else if ((rp = normal_maps_arg(&hle_d3d_normal_maps, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (rp < 0) {
                fprintf(stderr, "%s: invalid value '%s' (normal and specular strength 0-4, baked 0-1, dry highlights strength 0.1-3)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += rp - 1;
        }
        else if ((rp = hair_arg(&hle_d3d_hair, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (rp < 0) {
                fprintf(stderr, "%s: invalid value '%s' (hair strength 0.1-3)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += rp - 1;
        }
        else if ((rp = skin_arg(&hle_d3d_skin, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (rp < 0) {
                fprintf(stderr, "%s: invalid value '%s' (skin strength 0.1-3)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += rp - 1;
        }
        else if ((rp = eyes_arg(&hle_d3d_eyes, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0 ||
                 (rp = cloth_arg(&hle_d3d_cloth, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (rp < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-3)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += rp - 1;
        }
        else if (char_light_arg(&hle_d3d_char_light, argv[i])) {
        }
        else if (hat_arg(&hle_d3d_hats, argv[i])) {}
        else if (gpu_skin_arg(&hle_d3d_gpu_skin, argv[i])) {}
        else if ((world_args = world_light_arg(&hle_d3d_world_light, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (world_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-3)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += world_args - 1;
        }
        else if ((fire_args = fire_light_arg(&hle_d3d_fire, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (fire_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-3)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += fire_args - 1;
        }
        else if (!strcmp(argv[i], "--direct-geometry")) hle_d3d_direct_geometry = 1;
        else if (!strcmp(argv[i], "--no-direct-geometry")) hle_d3d_direct_geometry = 0;
        else if ((wshadow_args = world_shadow_arg(&hle_d3d_world_shadow, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (wshadow_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-1, size 512/1024/2048/4096, range 15-120)\n", argv[i],
                        i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += wshadow_args - 1;
        }
        else if ((lshadow_args = lamp_shadow_arg(&hle_d3d_lamp_shadow, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (lshadow_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-1, min angle 0-60, walls and doors 0 or 1)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += lshadow_args - 1;
        }
        else if ((level_args = level_lights_arg(&hle_d3d_level, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (level_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (lights 1-32)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += level_args - 1;
        }
        else if ((lamp_args = lamp_lights_arg(&hle_d3d_lamp, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (lamp_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-3, walls 0 or 1)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += lamp_args - 1;
        }
        else if ((shaft_args = light_shafts_arg(&hle_d3d_shafts, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (shaft_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-3)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += shaft_args - 1;
        }
        else if ((vf_args = volfog_arg(&hle_d3d_volfog, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (vf_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (quality low, medium or high; strength 0.1-3)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += vf_args - 1;
        }
        else if ((streak_args = wet_streaks_arg(&hle_d3d_wet_streaks, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (streak_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-4, cap 1-32)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += streak_args - 1;
        }
        else if ((damp_args = damp_arg(&hle_d3d_damp, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (damp_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-2)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += damp_args - 1;
        }
        else if ((nrefl_args = neon_refl_arg(&hle_d3d_neon_refl, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (nrefl_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-3)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += nrefl_args - 1;
        }
        else if ((sssr_args = sssr_arg(&hle_d3d_sssr, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (sssr_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (classic|sssr; quality low|medium|high; glass 0-2; res half|full)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += sssr_args - 1;
        }
        else if ((mat_args = materials_arg(&hle_d3d_materials, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (mat_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-2)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += mat_args - 1;
        }
        else if ((soft_args = soft_arg(&hle_d3d_soft, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (soft_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (distance 0.05-4)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += soft_args - 1;
        }
        else if ((range_args = shadow_range_arg(&hle_d3d_shadow_range, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (range_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (0.5-50)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += range_args - 1;
        }
        else if ((glass_args = hud_glass_arg(&hle_d3d_glass, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (glass_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (blur 1-32, opacity 0-1, tint AARRGGBB, hd height 0.5-4, hd opacity 0-1, hd frost 0-32, hd track/fill opacity and vivid 0-1)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += glass_args - 1;
        }
        else if (!strcmp(argv[i], "--dump-textures") && i + 1 < argc) hle_d3d_dump_textures = argv[++i];
        else if (!strcmp(argv[i], "--texture-pack") && i + 1 < argc) {
            if (hle_d3d_texture_pack_count == HLE_D3D_MAX_PACKS) {
                fprintf(stderr, "at most %d --texture-pack directories\n", HLE_D3D_MAX_PACKS);
                usage();
            }
            hle_d3d_texture_packs[hle_d3d_texture_pack_count++] = argv[++i];
        }
        else if (!strcmp(argv[i], "--texpack-sync")) hle_d3d_texpack_sync = 1;
        else if (!strcmp(argv[i], "--no-texpack-prefetch")) hle_d3d_texpack_prefetch = 0;
        else if (!strcmp(argv[i], "--level-prefetch") && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "on") && strcmp(v, "off")) usage();
            hle_d3d_level_prefetch = !strcmp(v, "on");
        }
        else if (!strcmp(argv[i], "--no-tex-race-guard")) tex_race_guard = 0;
        else if (!strcmp(argv[i], "--texpack-budget") && i + 1 < argc) {
            int mb = atoi(argv[++i]);
            if (mb < 32 || mb > 4096) {
                fprintf(stderr, "--texpack-budget takes 32 to 4096 (MiB)\n");
                usage();
            }
            hle_d3d_texpack_budget_mb = (unsigned)mb;
        }
        else if (!strcmp(argv[i], "--mesh-pack") && i + 1 < argc) hle_d3d_mesh_pack = argv[++i];
        else if (!strcmp(argv[i], "--mesh-log")) hle_d3d_meshover_log = 1;
        else if (!strcmp(argv[i], "--no-render-targets")) hle_d3d_render_targets = 0;
        else if (!strcmp(argv[i], "--no-frame-blur")) hle_d3d_frame_blur = 0;
        else if (!strcmp(argv[i], "--title-shadow-full")) hle_d3d_title_shadow_full = 1;
        else if (!strcmp(argv[i], "--vp-threads") && i + 1 < argc) hle_d3d_vp_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shadow-scale") && i + 1 < argc) {
            hle_d3d_shadow_scale = atoi(argv[++i]);
            if (hle_d3d_shadow_scale < 1 || hle_d3d_shadow_scale > 4) usage();
        } else if (!strcmp(argv[i], "--shadow-soft")) hle_d3d_shadow_soft = 1;
        else if (!strcmp(argv[i], "--supersample") && i + 1 < argc) {
            hle_d3d_supersample = atoi(argv[++i]);
            if (hle_d3d_supersample != 1 && hle_d3d_supersample != 2 && hle_d3d_supersample != 4) usage();
        }
        else if (!strcmp(argv[i], "--sharpen") && i + 1 < argc) {
            char *end;
            long n = strtol(argv[++i], &end, 10);
            if (*end || n < 0 || n > 100) usage();
            hle_d3d_sharpen = (int)n;
        }
        else if (!strcmp(argv[i], "--fsr") && i + 1 < argc) {
            if (fsr_parse_mode(argv[++i], &fsr_options.mode)) usage();
        }
        else if (!strcmp(argv[i], "--fsr-sharpness") && i + 1 < argc) {
            if (fsr_parse_sharpness(argv[++i], &fsr_options.sharpness)) usage();
        }
        else if (!strcmp(argv[i], "--taa")) taa_options.on = 1;
        else if (framegen_parse_arg(argv[i], &framegen_options)) ;
        else if (!strcmp(argv[i], "--no-taa")) taa_options.on = 0;
        else if (!strcmp(argv[i], "--taa-sharpness") && i + 1 < argc) {
            if (taa_parse_sharpness(argv[++i], &taa_options.sharpness)) usage();
            taa_options.sharpness_set = 1;
        }
        else if (!strcmp(argv[i], "--taa-upscale") && i + 1 < argc) {
            if (taa_parse_upscale(argv[++i], &taa_options.upscale, &taa_options.upscale_ratio)) usage();
        }
        else if (!strcmp(argv[i], "--taa-lite") && i + 1 < argc) {
            if (taa_parse_lite(argv[++i], &taa_options.lite)) usage();
        }
        else if (!strcmp(argv[i], "--upscaler") && i + 1 < argc) {
            if (taa_parse_upscaler(argv[++i], &taa_options.upscaler)) usage();
        }
        else if (!strcmp(argv[i], "--fsr3-masks") && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "on") && strcmp(v, "off")) usage();
            taa_options.fsr3_masks_off = !strcmp(v, "off");
        }
        else if (!strcmp(argv[i], "--dynres") && i + 1 < argc) {
            if (dynres_parse_target(argv[++i], &dynres_options.target_ms)) usage();
            dynres_options.on = 1;
        }
        else if (!strcmp(argv[i], "--no-dynres")) dynres_options.on = 0;
        else if (!strcmp(argv[i], "--vrs") && i + 1 < argc) {
            if (renderer_vrs_parse(argv[++i], &hle_d3d_vrs)) usage();
        }
        else if (!strcmp(argv[i], "--bench")) bench_options.on = 1;
        else if (!strcmp(argv[i], "--bench-out") && i + 1 < argc) bench_options.out = argv[++i];
        else if (!strcmp(argv[i], "--dynres-min") && i + 1 < argc) {
            if (dynres_parse_min(argv[++i], &dynres_options.min_ratio)) usage();
            dynres_options.min_set = 1;
        }
        else if (!strcmp(argv[i], "--anim-interp")) anim_interp_options.on = 1;
        else if (!strcmp(argv[i], "--no-anim-interp")) anim_interp_options.on = 0;
        else if (!strcmp(argv[i], "--fps-overlay")) hle_d3d_fps_overlay = 1;
        else if (!strcmp(argv[i], "--photo-prepare")) hle_photo_prepare = 1;
        else if (!strcmp(argv[i], "--no-photo-prepare")) hle_photo_prepare = 0;
        else if (!strcmp(argv[i], "--photo-dir") && i + 1 < argc) hle_photo_dir = argv[++i];
        else if (!strcmp(argv[i], "--no-fps-overlay")) hle_d3d_fps_overlay = 0;
        else if (!strcmp(argv[i], "--no-pipeline-cache")) hle_d3d_pipeline_cache = 0;
        else if (!strcmp(argv[i], "--pipeline-cache")) hle_d3d_pipeline_cache = 1;
        else if (!strcmp(argv[i], "--sync-pipelines")) hle_d3d_pipeline_async = 0;
        else if (!strcmp(argv[i], "--async-pipelines")) hle_d3d_pipeline_async = 1;
        else if (!strcmp(argv[i], "--no-dynamic-state")) hle_d3d_dynamic_state = 0;
        else if (!strcmp(argv[i], "--fps-overlay-items") && i + 1 < argc) {
            int items = fps_overlay_items(argv[++i]);
            if (items < 0) usage();
            hle_d3d_fps_overlay_items = items;
        }
        else if (!strcmp(argv[i], "--fps-overlay-corner") && i + 1 < argc) {
            int c = fps_overlay_corner(argv[++i]);
            if (c < 0) usage();
            hle_d3d_fps_overlay_corner = c;
        }
        else if (!strcmp(argv[i], "--movie-hd")) movie_hd_on = 1;
        else if (!strcmp(argv[i], "--no-movie-hd")) movie_hd_on = 0;
        else if (gamma_ramp_arg(&gamma_ramp_enabled, argv[i])) {}
        else if (cinema_bars_arg(&cinema_bars_on, argv[i])) {}
        else if (!strcmp(argv[i], "--widescreen")) {
            hle_d3d_widescreen = 1;
            nvset_widescreen(1);
        }
        else if (!strcmp(argv[i], "--language") && i + 1 < argc) {
            if (!nvset_language(argv[++i])) {
                fprintf(stderr, "--language: unknown '%s' (valid: %s)\n", argv[i], NV_LANGUAGE_CODES);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--fake-mmio")) fake_mmio = 1;
        else if (!strcmp(argv[i], "--patch-fs")) patch_fs_flag = 1;
        else if (!strcmp(argv[i], "--win-thread-probe")) probe_threads = 1;
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) watchdog_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hdd") && i + 1 < argc) hdd = argv[++i];
        else if (!strcmp(argv[i], "--log-limit") && i + 1 < argc) klog_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--audio-log") && i + 1 < argc) audio_log = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--audio-levels")) audio_set_levels(1);
        else if (!strcmp(argv[i], "--no-rolloff-curves")) hle_audio_set_rolloff_curves(0);
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audio_backend = argv[++i];
        else if (!strcmp(argv[i], "--audio-dump") && i + 1 < argc) audio_dump = argv[++i];
        else if (!strcmp(argv[i], "--audio-lead-ms") && i + 1 < argc) {
            char *end;
            long ms = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end || ms < 0 || ms > AUDIO_LEAD_MAX_MS) {
                fprintf(stderr, "--audio-lead-ms: '%s' is not a whole number of ms from 0 to %d\n", argv[i], AUDIO_LEAD_MAX_MS);
                return 2;
            }
            audio_set_lead_ms((int)ms);
        }
        else if (!strcmp(argv[i], "--sound-pack") && i + 1 < argc) sound_pack = argv[++i];
        else if (!strcmp(argv[i], "--resampler") && i + 1 < argc) {
            ResampleKind k;
            if (resample_parse(argv[++i], &k)) { fprintf(stderr, "--resampler: '%s' is not linear or sinc\n", argv[i]); return 2; }
            audio_set_resampler(k);
        }
        else if ((!strcmp(argv[i], "--dialogue-volume") || !strcmp(argv[i], "--music-volume") || !strcmp(argv[i], "--sfx-volume")) &&
                 i + 1 < argc) {
            int cls = argv[i][2] == 'd' ? HLE_AUDIO_CLASS_DIALOGUE : argv[i][2] == 'm' ? HLE_AUDIO_CLASS_MUSIC : HLE_AUDIO_CLASS_SFX;
            if (hle_audio_parse_db(argv[i + 1], &class_db[cls])) {
                fprintf(stderr, "%s: '%s' is not a level in dB from %.0f to %+.0f\n", argv[i], argv[i + 1], HLE_AUDIO_SFX3D_MIN_DB,
                        HLE_AUDIO_SFX3D_MAX_DB);
                return 2;
            }
            i++;
        }
        else if (!strcmp(argv[i], "--sfx-3d-volume") && i + 1 < argc) {
            if (hle_audio_parse_db(argv[++i], &sfx_3d_db)) {
                fprintf(stderr, "--sfx-3d-volume: '%s' is not a level in dB from %.0f to %+.0f\n", argv[i],
                        HLE_AUDIO_SFX3D_MIN_DB, HLE_AUDIO_SFX3D_MAX_DB);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--no-xact-controls")) xact_controls = 0;
        else if (!strcmp(argv[i], "--no-xbox-headroom")) audio_set_xbox_headroom(0);
        else if (!strcmp(argv[i], "--reverb")) audio_set_reverb(1);
        else if (!strcmp(argv[i], "--no-reverb")) audio_set_reverb(0);
        else if (!strcmp(argv[i], "--doppler")) doppler = 1;
        else if (!strcmp(argv[i], "--no-doppler")) doppler = 0;
        else if (!strcmp(argv[i], "--rear-cue")) rear_cue = 1;
        else if (!strcmp(argv[i], "--no-rear-cue")) rear_cue = 0;
        else if (!strcmp(argv[i], "--audio-hrtf")) audio_hrtf = 1;
        else if (!strcmp(argv[i], "--no-audio-hrtf")) audio_hrtf = 0;
        else if (!strcmp(argv[i], "--audio-air")) audio_air = 1;
        else if (!strcmp(argv[i], "--no-audio-air")) audio_air = 0;
        else if (!strcmp(argv[i], "--audio-occlusion")) audio_occl = 1;
        else if (!strcmp(argv[i], "--no-audio-occlusion")) audio_occl = 0;
        else if (!strcmp(argv[i], "--steam-audio")) steam_audio = 1;
        else if (!strcmp(argv[i], "--no-steam-audio")) steam_audio = 0;
        else if (!strcmp(argv[i], "--audio-reverb-geo")) reverb_geo = 1;
        else if (!strcmp(argv[i], "--no-audio-reverb-geo")) reverb_geo = 0;
        else if (!strcmp(argv[i], "--audio-pathing")) audio_pathing = 1;
        else if (!strcmp(argv[i], "--no-audio-pathing")) audio_pathing = 0;
        else if ((rp = rateprobe_parse_arg(argc, argv, &i, &rate_probe)) != 0) {
            if (rp < 0) usage();
        }
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--quiet")) trace = 0;   /* the default; kept for old scripts */
        else usage();
    }
    klog_enabled = trace;   /* per-call logs only with --trace; one-time messages always */
    printf("[start] xbrun %s\n", XBRUN_VERSION);   /* the launcher's bug report reads it */
    settings_sync_quit = hle_d3d_request_quit;   /* in-game settings: the launcher's environment (settings_sync.h) */
    settings_sync_init();
    printf("[start] language: %s (XC_LANGUAGE %u)\n", nvset_language_name(), nvset_value(NV_LANGUAGE));
    uint8_t *data;
    size_t size;
    Xbe x;
    char err[128];
    printf("[start] reading %s (disc %s)\n", xbe_path, disc);
    if (xbe_read_file(xbe_path, &data, &size)) { perror(xbe_path); return 1; }
    if (xbe_parse(&x, data, size, err, sizeof err)) { fprintf(stderr, "%s: %s\n", xbe_path, err); return 1; }
    if (syms) printf("[load] %d symbols from %s\n", symbols_load(syms), syms);
    {
        const DlogSources sources = {hle_d3d_frame_count, mem_arena_used, mem_arena_total, kobj_live_threads};
        dlog_set_sources(&sources);
    }
    diag_install();
    plat_kpcr_init();
    printf("[start] mapping memory and the XBE image\n");
    mem_init();
    probe("before mapping the XBE");
    map_image(&x);
    probe("XBE mapped (XBE header at the image base)");
    if (plat_image_mapped(x.base, x.base + x.image_size)) patch_xbeh_constants(&x);
    probe("process image header restored");
    printf("[start] kernel imports and HLE patches\n");
    if (getenv("XB_TRI_CHECK")) hle_d3d_tri_check = 1;
    if (hle_d3d_tri_check) printf("[tri-check] on: non-finite and exploded post-transform vertices are logged\n");
    kernel_init();
    patch_thunks(&x);
    if (!nhooks && syms && !hle_d3d) hooks[nhooks++] = "D3D8__Direct3D_CreateDevice";
    for (int i = 0; i < nhooks; i++) add_hook(hooks[i]);
    if (hle_audio) {   /* XACT/DirectSound API boundary on the host instead of MCPX APU MMIO */
        hle_audio_log_limit = audio_log >= 0 ? audio_log : trace ? 32 : 0;
        hle_audio_install(syms ? symbol_lookup : NULL);
        hle_audio_handle_fd = handle_fd;
        /* The mixer always runs (it advances DirectSound play cursors); "null" just has no device. */
        audio_set_stats(hle_d3d_stats);
        printf("[audio] backend: %s, resampler %s\n", audio_open(audio_backend, audio_dump), audio_resampler_name());
        hle_audio_set_sfx_3d_volume(sfx_3d_db);
        for (int c = 0; c < HLE_AUDIO_NCLASSES; c++) hle_audio_set_class_volume(c, class_db[c]);
        printf("[audio] volumes: dialogue %+.1f dB, music %+.1f dB, effects %+.1f dB\n", class_db[HLE_AUDIO_CLASS_DIALOGUE],
               class_db[HLE_AUDIO_CLASS_MUSIC], class_db[HLE_AUDIO_CLASS_SFX]);
        printf("[audio] 3D sound-effect volume: %.1f dB\n", sfx_3d_db);
        hle_audio_set_xact_controls(xact_controls);
        printf("[audio] xact controls: %s\n", xact_controls ? "on" : "off");
        printf("[audio] Xbox headroom: %s\n", audio_xbox_headroom() ? "on" : "off");
        printf("[audio] I3DL2 reverb: %s\n", audio_reverb_enabled() ? "on" : "off");
        hle_audio_set_doppler(doppler);
        hle_audio_set_rear_cue(rear_cue);
        hle_audio_clock = hle_xapi_seconds;   /* position deltas on the title's clock */
        printf("[audio] 3D Doppler: %s, rear cue: %s\n", doppler ? "on" : "off", rear_cue ? "on" : "off");
        /* Steam Audio: the library only when an option uses it (a missing one changes nothing) */
        if (!audio_reverb_enabled()) reverb_geo = 0;   /* nothing to drive */
        if (steam_audio && (audio_hrtf || audio_occl || reverb_geo)) sa_open(NULL);
        else if (!steam_audio) printf("[steam-audio] off (--no-steam-audio): using the built-in 3D sound models\n");
        audio_set_steam_hrtf(audio_hrtf && sa_ok());
        if (!audio_occl) audio_pathing = 0;   /* the paths add to the occlusion's result */
        char ac_cache[1024];
        snprintf(ac_cache, sizeof ac_cache, "%s/acoustics", hdd);   /* beside the saves, never seen by the title */
        ac_set_cache_dir(ac_cache);
        if (sa_ok() && (audio_occl || reverb_geo) && !ac_start(1, reverb_geo, audio_pathing)) {
            ac_set_stats(hle_d3d_stats);
            hle_d3d_audio_level = ac_level;
            hle_d3d_audio_doors = ac_doors;
            if (audio_occl) hle_audio_set_geo_occluder(ac_query);
            geo_reverb_on = reverb_geo;
            hle_audio_listener_hook = geo_listener;
            printf("[acoustics] level geometry from the disc: occlusion and transmission %s, sound around corners %s, location-based reverb %s\n",
                   audio_occl ? "on" : "off", audio_pathing ? "on" : "off", reverb_geo ? "on" : "off");
        } else {
            if (reverb_geo) printf("[acoustics] location-based reverb needs the Steam Audio library: the title's reverb\n");
            if (audio_pathing) printf("[acoustics] sound around corners needs the Steam Audio library: occlusion from the wall map only\n");
        }
        hle_audio_set_hrtf(audio_hrtf);
        hle_audio_set_air(audio_air);
        hle_audio_set_occlusion(audio_occl);
        hle_d3d_audio_occlusion = audio_occl;   /* the level's wall map is built for it */
        hle_audio_set_occluder(audio_occl ? hle_d3d_audio_walls : NULL);
        printf("[audio] 3D binaural (headphones): %s, air absorption: %s, wall occlusion: %s\n", audio_hrtf ? audio_steam_hrtf() ? "on (Steam Audio HRTF)" : "on" : "off",
               audio_air ? "on" : "off", audio_occl ? "on" : "off");
        audio_on_exit = 1;
    }
    hle_d3d_on_exit = on_exit_all;
    if (hle_d3d) {   /* D3D8 API boundary on the host instead of NV2A MMIO; frames on the main thread */
        hle_d3d_log_limit = trace ? 1 : 0;
        hle_d3d_install(syms ? symbol_lookup : NULL);
        kobj_idle_hook = hle_d3d_service;
        hle_d3d_frame_queued = kobj_service_wake;   /* the host takes each frame as soon as it is queued */
    }
    if (bench_options.on) {   /* --bench: the fixed sequence (bench.h) */
        bench_settings(argc, argv, bench_options.settings, sizeof bench_options.settings);
        if (!warp_on) {
            char err[192];
            if (!warp_parse(BENCH_WARP, &warp, err, sizeof err)) warp_on = 1;
        }
        if (!input_script) input_script = BENCH_INPUT;
        skip_intro = 1;   /* BENCH_INPUT's frame numbers are counted from the title screen without the boot movies */
    }
    if (goto_on) {   /* --goto: the menu input, the warp and a Lua step (hle_goto.h) */
        if (goto_allow_save && goto_plan.warp_on) {
            fprintf(stderr, "--goto-allow-save: --goto %s redirects a level load, which always runs with saving off\n", goto_plan.place);
            return 2;
        }
        if (warp_on && goto_plan.warp_on) {
            fprintf(stderr, "--goto %s: choose either --goto or --warp/--scene\n", goto_plan.place);
            return 2;
        }
        skip_intro = 1;   /* the input script's frame numbers are counted from the title screen without the boot movies */
        if (!input_script) input_script = goto_plan.input;
        if (goto_plan.warp_on) warp = goto_plan.warp, warp_on = 1;
        if (goto_plan.lua) {
            char name[32];
            snprintf(name, sizeof name, "goto:%s", goto_plan.place);
            devlua_add(goto_plan.lua, strlen(goto_plan.lua), name, 0, 1);
        }
        printf("[goto] %s%s%s: leaves %s\n", goto_plan.place, goto_plan.warp_on ? " -> " : "",
               goto_plan.warp_on ? goto_plan.warp.level->level : "", goto_plan.leaves);
    }
    if (input_script) {
        char err[128];
        if (input_script_parse(input_script, err, sizeof err)) { fprintf(stderr, "--input-script: %s\n", err); return 2; }
    }
    input_map_defaults();   /* PC key layout; a key map file changes it (invalid lines are reported, not fatal) */
    if (input_config && input_map_load_file(input_config) < 0) { perror(input_config); return 2; }
    if (!input_config) input_map_load_file("warriors-input.cfg");   /* optional */
    hle_d3d_materials_disc(disc);
    colpatch_set_disc(disc);   /* mod collision patches recognise their level by its file */   /* --materials: each level's map is read from the disc at its load */
    /* the PlayStation icons and the controller remap repaint the icons too */
    if (hle_d3d_button_icons != HLE_ICONS_XBOX || hle_d3d_pad_icons || input_pad_icons_moved()) {
        int n = hle_d3d_glyphs_load(disc);
        if (n < 0) printf("[glyphs] no glyph table in %s/XBoxWad.idx (paks\\global.pak): Xbox button icons kept\n", disc);
        else printf("[glyphs] %d glyph rects from paks\\global.pak; button icons: %s%s%s%s\n", n,
                    hle_d3d_button_icons == HLE_ICONS_KBM ? "keyboard/mouse" : hle_d3d_button_icons == HLE_ICONS_AUTO ? "auto (last used device)" : "controller",
                    hle_d3d_pad_icons && hle_d3d_button_icons != HLE_ICONS_KBM ? ", controller icons " : "",
                    hle_d3d_pad_icons && hle_d3d_button_icons != HLE_ICONS_KBM ? kbm_ps_style_name(hle_d3d_pad_icons) : "",
                    input_pad_icons_moved() ? ", controller icons follow the button remap" : "");
    }
    if (sound_pack) {   /* DIR/<cue name>.wav replaces that cue's wave (src/sndpack.h) */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int n = sndpack_open(sound_pack, disc);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (load_prof_on) printf("[load] sound pack at start: %.0f ms\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
        if (n == -1) { fprintf(stderr, "--sound-pack: cannot read the folder %s\n", sound_pack); return 2; }
        if (n == -2) printf("[audio] sound pack %s: no sound name table in %s/XBoxWad.idx (warriors.glr), pack not used\n", sound_pack, disc);
        else printf("[audio] sound pack %s: %d sounds replaced\n", sound_pack, n);
    }
    if (!input_rumble_enabled) printf("[input] controller vibration off\n");
    if (hle_input) {   /* XAPI input boundary on the host: one gamepad in port 0, no USB/OHCI code */
        hle_input_log_limit = trace ? 32 : 0;
        hle_input_install(syms ? symbol_lookup : NULL);
    }
    /* XAPI QueryPerformanceCounter reads rdtsc but the title assumes the Xbox's 733 MHz rate
     * (QueryPerformanceFrequency is a constant): count at that rate from the host clock. */
    hle_xapi_install(syms ? symbol_lookup : NULL);
    /* Experiment: more concurrent spawns (hle_spawn.h); scale 1 leaves the title untouched. */
    hle_spawn_install(spawn_scale);
    hle_spawn_report = hle_d3d_stats || spawn_scale > 1;
    /* Effect pools (hle_fx.h): --fx-scale raises them (on a bigger main heap); the --stats [fx] line
     * shows their use and counts the sprites each particle system drops. Scale 1 leaves them. */
    hle_fx_install(fx_scale, big_heap || fx_scale > 1, hle_d3d_stats);
    hle_fx_report = hle_d3d_stats;
    /* Opt-in crew AI changes (hle_allies.h); off leaves every byte of the title. */
    if (hle_allies_install(smart_allies)) hle_aidbg_install();
    /* Opt-in enemy group tactics (hle_tactics.h); off leaves every byte of the title. */
    hle_tactics_install(enemy_tactics, hle_aidbg_active);
    /* Diagnostic: crew AI log (hle_aidbg.h); counts the Fight goal's attack gate answers. */
    if (hle_aidbg_active) hle_d3d_swap_hook = hle_aidbg_swap, hle_aidbg_install();
    /* Experimental: the world updates on every 1/60 s step with a halved time step (hle_sim60.h). */
    int sim60_active = hle_sim60_install(sim60);
    huspeed_sim60 = sim60_active;   /* ModSpeed: a released cap recovers per second, not per update */
    combat_debug_install(combat_debug);
    /* Diagnostic: game-time snapshots for the rate diff between both world rates (rateprobe.h). */
    if (rate_probe.path[0]) rate_probe_start(&x, &rate_probe, sim60_active);
    /* Start a chosen mission when a profile is loaded; saving off for the session (hle_warp.h). */
    /* Without the warp, the same site only reports level loads (the [hitch] / [level-summary] level). */
    warp_level_hook = hle_d3d_level_loaded;
    hle_d3d_level_pending = warp_level_pending;
    if (!hle_warp_install(warp_on, &warp) && !warp_on) {
        if (!goto_on || goto_allow_save || !hle_warp_guard_install()) hle_warp_track_install();
        if (goto_on && goto_allow_save) printf("[goto] --goto-allow-save: the game may write the save in --hdd\n");
    }
    /* --control / --fast-boot (gamectl.h): before the Lua hook, which then also serves the socket's chunks. */
    if (gamectl_wanted()) {
        char err[256];
        gamectl_boot_plan(goto_on ? gc_script_end(goto_plan.input) : 0, goto_on && goto_plan.lua, !goto_on || strcmp(goto_plan.place, "title"));
        if (gamectl_install(err, sizeof err)) { fprintf(stderr, "%s\n", err); return 2; }
    }
    /* --lua / --lua-exec / --lua-pipe / --dev-console and --goto's Lua steps (hle_devlua.h). */
    hle_devlua_install(hle_d3d_frame_count);
    /* The in-game settings menu (hle_settings.h): F10 / Back + Start, before a mod's menu. */
    hle_settings_install(disc, nvset_language_name());
    /* --mod: Lua mods after the game's script loads (hle_mods.h). */
    {
        char mods_dir[4200];
        snprintf(mods_dir, sizeof mods_dir, "%smods", plat_program_dir());
        mods_set_assets(disc, hdd);   /* assets.ini: built from the disc into <hdd>/mods-cache */
        mods_load(mods_dir);
        mods_safety_start(hdd);   /* save backup for save-changing mods, crash-context mods line (modsafe.h) */
        scriptfix_set_enabled(script_fixes);   /* --no-script-fixes: rides on the same lua_dobuffer hook (hle_scriptfix.h) */
        if (hle_mods_install()) {   /* menus and music for mods (hle_modmenu.h) */
            static const HleMenuAudio menu_audio = {hle_audio_music_valid, hle_audio_music_start, hle_audio_music_level, hle_audio_music_alive,
                                                    hle_audio_music_stop, hle_audio_music_mute_set, hle_audio_music_game_level};
            menu_disc = disc;
            hle_modmenu_setup(&menu_audio, menu_cue_of, disc, NULL);
            hle_audio_music_tick = mods_audio_tick;   /* menu music fades and the mods' own sounds */
            static const ModSoundAudio snd_audio = {snd_voice_new, snd_voice_free, snd_voice_play, snd_voice_playing, snd_voice_gains,
                                                    hle_audio_listener_get, audio_lock, audio_unlock};
            modsound_setup(&snd_audio);
            hle_settings_next_filter = hle_modmenu_input;   /* after the settings menu (hle_settings.h) */
            hle_settings_next_escape = hle_modmenu_escape;
            hle_settings_next_key = mod_menu_key;   /* F8: ModMenuKey (hle_modmenu.h) */
        }
    }
    {   /* photo mode (hle_photo.h): F9 / Back + RB, outermost of the pad filters (it wraps the settings menu's) */
        const char *ui = settings_sync_ui_language();
        hle_photo_install(ui && *ui ? ui : !strcmp(nvset_language_name(), "Spanish") ? "es" : "en");
    }
    warp_level_cb = level_loaded;
    if (hle_warp_watch_install()) printf("[warp] level watch: on (level loads leave breadcrumbs)\n");
    /* HUD widgets that step once per rendered frame, authored for 30 fps, the world-side graffiti
     * fixes when the 60 Hz world is active, and the spray-can duration comfort setting
     * (hle_ratefix.h). */
    hle_ratefix_install(rate_fixes, spray_time, sim60_active);
    /* The three boot movies (logo, plogo, l1_in) are not played; every other movie is (hle_skipintro.h). */
    hle_skipintro_install(skip_intro);
    /* 2048 memory-registry nodes instead of 512: with --big-heap Rumble's gang previews filled the registry (hle_memreg.h). */
    hle_memreg_install();
    /* The far plane without the low-fps pull-in, and the far-plane cap and small-prop cull
     * scaled by --draw-distance / --prop-detail (hle_drawdist.h). */
    hle_drawdist_install(&draw_dist);
    /* Known mistranslations in the string tables, fixed in the data the title reads, and with
     * keyboard/mouse icons the stick phrases of the tutorial texts as the key map's keys (hle_textfix.h). */
    {
        const int move[4] = {input_key_for_control(IN_LS_UP), input_key_for_control(IN_LS_LEFT),
                             input_key_for_control(IN_LS_DOWN), input_key_for_control(IN_LS_RIGHT)};
        int icons = hle_d3d_button_icons == HLE_ICONS_KBM ? TEXTFIX_ICONS_KBM
                    : hle_d3d_button_icons == HLE_ICONS_AUTO ? TEXTFIX_ICONS_AUTO : TEXTFIX_ICONS_XBOX;
        hle_textfix_install(text_fixes, nvset_value(NV_LANGUAGE), icons, move, input_key_for_control(IN_LTHUMB), controls_text_gate);
    }
    /* Diagnostic: --poke bytes (rateprobe.h), e.g. the cutscene sweep's level/chapter shortcut. */
    for (int i = 0; i < rate_probe.npokes; i++) {
        const RateProbePoke *k = &rate_probe.pokes[i];
        int r = rateprobe_apply_poke(k, (uint8_t *)(uintptr_t)k->addr);
        printf("[poke] %08x+%u %s\n", k->addr, k->len, r ? "NOT applied: original bytes differ" : "applied");
        if (r) return 2;
    }
    /* Without the input HLE, XInitDevices (USB/OHCI MMIO) is skipped and the title sees no pads. */
    if (default_skips && syms && nskips < 16 && !hle_input_patched("XInitDevices")) skips[nskips++] = "XAPILIB__XInitDevices:8";
    for (int i = 0; i < nskips; i++) add_skip(skips[i]);
    if (fake_mmio) {
        /* Experimental: back MCPX APU, AC97 (ACI) and USB OHCI MMIO with plain zeroed RAM so their
         * init code reads "idle" registers. NV2A (0xFD000000) is deliberately left unmapped. */
        static const uint32_t ranges[][2] = {{0xFE800000, 0x80000}, {0xFEC00000, 0x1000}, {0xFED00000, 0x10000}};
        for (int i = 0; i < 3; i++)
            printf("[load] fake MMIO RAM %08x+%x: %s\n", ranges[i][0], ranges[i][1],
                   guest_alloc_at(ranges[i][0], ranges[i][1]) ? "ok" : "FAILED");
        apu_fake_start();
    }
    probe("HLE patches installed");
    patch_fs(&x, patch_fs_flag);
    probe("fs patched, before the entry thread");
    printf("[start] files: disc %s, hdd %s; starting the title's entry thread\n", disc, hdd);
    files_init(disc, hdd);
    mods_set_storage(hdd, files_profile_id);   /* ModSave / ModLoad: <hdd>/mods-data */
    hle_d3d_pipeline_cache_dir = hdd;   /* beside the partitions: the title never sees it */
    kobj_start_entry(x.entry, x.stack_commit);
    kobj_service_loop();
}
