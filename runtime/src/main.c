/* xbrun: loads an original Xbox XBE into this 32-bit process at its native addresses,
 * replaces kernel imports with host implementations and runs it: by default with the D3D8
 * API boundary on the host (frames shown in a window), or with --no-hle-d3d until a hooked
 * D3D entry point is reached. Usage: xbrun <default.xbe> <disc_dir> [options] */
#define _GNU_SOURCE
#include "diag.h"
#include "files.h"
#include "fspatch.h"
#include "hle_spawn.h"
#include "hle_ratefix.h"
#include "hle_sim60.h"
#include "rateprobe.h"
#include "hle_warp.h"
#include "hle_textfix.h"
#include "audio.h"
#include "hle_audio.h"
#include "hle_d3d.h"
#include "hle_input.h"
#include "input_map.h"
#include "hle_xapi.h"
#include "kernel.h"
#include "nvset.h"
#include "kobj.h"
#include "mem.h"
#include "video.h"
#include "xbe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

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

static void usage(void) {
    fprintf(stderr,
            "usage: %s <default.xbe> <disc_dir> [--symbols file] [--hook SYMBOL|0xADDR]...\n"
            "             [--skip SYMBOL:POPBYTES[:RET]]... [--no-default-skips] [--no-hle-audio] [--fake-mmio] [--watchdog SEC] [--hdd dir] [--trace] [--log-limit N]\n"
            "             [--no-hle-d3d] [--frames N] [--dump-frame FILE.bmp] [--video sdl|x11|null]\n"
            "             [--renderer vulkan|none] [--no-hle-input] [--input-script FRAME:[pN/]CONTROLS[:FRAMES],...] [--stats]\n"
            "             [--input-config FILE] [--button-icons xbox|kbm|auto] [--no-rumble] [--pad-mappings FILE] [--pad-slot N=GUID[#K]]... [--spawn-scale N] [--no-rate-fixes] [--spray-time PCT] [--no-text-fixes] [--no-frame-lock] [--sim60] [--rate-probe FILE[:MS]] [--rate-probe-range ADDR:SIZE|@PTR[+OFF]:SIZE]... [--poke ADDR:HEX[/EXPECT]]... [--warp MISSION[:CHECKPOINT]] [--audio sdl|null] [--audio-dump FILE.wav] [--audio-log N] [--sfx-3d-volume DB] [--no-xact-controls] [--patch-fs] [--win-thread-probe]\n"
            "             [--resolution WxH | --render-scale N] [--supersample N] [--sharpen N] [--bloom] [--bloom-strength F] [--bloom-threshold F] [--rain-hd] [--rain-hd-density F] [--ssao] [--ssao-strength F] [--ssao-radius F] [--msaa N] [--aniso N] [--widescreen] [--fullscreen]\n"
            "             [--language en|es|fr|de|it]\n"
            "             [--present auto|mailbox|fifo|immediate|readback] [--frames-in-flight N] [--dump-textures DIR] [--texture-pack DIR]...\n"
            "             [--no-render-targets] [--shadow-scale N] [--shadow-soft] [--shadow-range] [--shadow-range-dist F] [--hud-hd] [--hud-hd-height F] [--hud-hd-opacity F] [--hud-hd-frost N] [--hud-hd-glass] [--hud-hd-track-opacity F] [--hud-hd-fill-opacity F] [--hud-hd-vivid F] [--hud-glass] [--hud-glass-blur N] [--hud-glass-opacity F] [--hud-glass-tint AARRGGBB]\n"
            "  default: HLE D3D, frames in an SDL2 window; with --no-hle-d3d the default hook is\n"
            "  D3D8__Direct3D_CreateDevice (needs --symbols)\n"
            "  --input-config FILE: key map (default: warriors-input.cfg in the current directory, if present)\n"
            "  --patch-fs: rewrite the title's fs:[KPCR] accesses as on Windows (Linux: for testing)\n"
            "  --trace: log every kernel call, file open and HLE call (default: summaries, warnings and faults)\n"
            "  --stats: per 60 presented frames, the presented rate and where the time goes (guest frame,\n"
            "           draw recording, waits for the host, host renderer, present)\n"
            "  --resolution WxH: render the 3D scene at the largest 4:3 size inside WxH (e.g. 1920x1080 -> 1440x1080);\n"
            "  --render-scale N: at N x 640x480; --msaa N: N samples per pixel (2, 4, 8); --aniso N: anisotropic\n"
            "           filtering up to N (default: 640x480, no MSAA, no anisotropy; --dump-frame writes the scaled frame)\n"
            "  --supersample N: render at N x that size (2 or 4) and box-filter it down to it (antialiasing of\n"
            "           every edge and texture; combines with --msaa; window and --dump-frame keep the output size)\n"
            "  --sharpen N: contrast-adaptive sharpening of the finished frame, 0-100 (default 0 = off; AMD FidelityFX\n"
            "           CAS's filter: stronger on soft detail, gentle on edges that are already sharp; also in --dump-frame)\n"
            "  --bloom: lamps, neon, fire and glow sprites bleed light into the 3D scene (not into the HUD or text);\n"
            "           --bloom-strength F: how much glow is added (0.1-3, default 0.6); --bloom-threshold F: the\n"
            "           brightness where it starts (0-1, default 0.75); --no-bloom turns it off again\n"
            "  --rain-hd: while it rains, the title's flat rain layers are replaced by thin falling streaks at several\n"
            "           depths (hidden behind nearer walls and people, near ones larger and faster), with the title's\n"
            "           slant, speed and colour; --rain-hd-density F: streaks per area (0.25-4, default 1); --no-rain-hd\n"
            "           turns it off again\n"
            "  --ssao: ambient occlusion: corners, wall and floor joints and the ground under people and props darken\n"
            "           a little (the opaque world only; effects, lamp halos, rain and the HUD are not darkened);\n"
            "           --ssao-strength F: how much (0.1-2, default 0.8); --ssao-radius F: how far around a point\n"
            "           occluders count, in world units (0.1-8, default 0.8); --no-ssao turns it off again\n"
            "  --present: auto (default) presents through a Vulkan swapchain in the SDL window (mailbox, else fifo);\n"
            "           readback reads the frame back and shows it with SDL (also used with --video x11|null, --dump-frame)\n"
            "  --frames-in-flight N: swapchain path, frames the GPU may still run while the next is recorded\n"
            "           (1-3, default 2; 1 = wait for each frame after presenting it; readback always waits)\n"
            "  --fullscreen: desktop fullscreen (F11 or Alt+Enter toggles)\n"
            "  --widescreen: report a 16:9 TV to the title (anamorphic frames, presented and scaled as 16:9)\n"
            "  --language CODE: the console language the title reads (en, es, fr, de, it; default en):\n"
            "           menu and HUD text (voices are not translated)\n"
            "  --dump-textures DIR: write each unique texture once as DIR/<hash>.png (level 0) + DIR/manifest.csv\n"
            "  --button-icons xbox|kbm|auto: in-game button prompts as the Xbox glyphs (default), as key caps and\n"
            "           mouse drawings for the key map, or following the last used device (keyboard/mouse or controller);\n"
            "           kbm also names the keyboard and mouse where the tutorial texts name the sticks (auto: unless a\n"
            "           controller is connected when the first such text is read)\n"
            "  --no-rumble: no controller vibration\n"
            "  --pad-mappings FILE: SDL controller mappings (default: gamecontrollerdb.txt in the program's folder\n"
            "           and in the current directory, as the launcher's controller set-up writes it)\n"
            "  --pad-slot N=GUID[#K]: the controller with that SDL GUID is player N (1-4) (port N-1), also when\n"
            "           it is plugged in during play; #K: the Kth of identical controllers (same GUID) in the order\n"
            "           they were attached. Repeatable. The others take the ports no controller was given\n"
            "  --texture-pack DIR: replace textures that have a DIR/<hash>.png (any size; mips generated);\n"
            "                 repeatable (up to 8): the first pack that has a texture wins\n"
            "  --no-render-targets: skip the title's own render-target passes (character shadows)\n"
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
            "  --no-rate-fixes: keep the HUD minigames' per-frame steps authored for 30 fps (default: halved for 60 Hz)\n"
            "  --spray-time PCT: graffiti spray can duration in percent of the Xbox one, 100..200 (default 110;\n"
            "                    applied with the rate fixes)\n"
            "  --no-text-fixes: read the string tables as shipped (default: known mistranslations corrected,\n"
            "                   e.g. the Spanish gym's \"Sentadillas\" for Sit Ups -> \"Abdominales\")\n"
            "  --sfx-3d-volume DB: experiment, -40 to +12 (default 0): extra level on every positioned (3D)\n"
            "      sound effect; cutscene dialogue, music, movies and 2D sounds are unchanged\n"
            "  --no-xact-controls: ignore the cue volume/fade controls the title sets (A/B test; default: applied)\n"
            "  --no-frame-lock: QueryPerformanceCounter on real time (default: one 60 Hz period per presented frame,\n"
            "      whole periods towards real time when behind, so the 30 Hz world updates on every other frame)\n"
            "  --sim60: experimental, off by default: the world (characters, AI, cameras) updates on every\n"
            "      1/60 s step with half the time step, instead of every other step (same game speed)\n"
            "  --rate-probe FILE[:MS]: diagnostic: every MS (default 100) of game time, snapshot the XBE's .data\n"
            "      section (and each --rate-probe-range: ADDR:SIZE, or @PTR[+OFF]:SIZE behind a pointer) into FILE,\n"
            "      for tools/ratediff (systems that count world updates instead of time under --sim60)\n"
            "  --scene ID: play one in-engine scene alone (Rockstar scene test mode), e.g. l84_c7; implies --warp\n"
            "  --warp MISSION[:CHECKPOINT]: loading a profile starts story mission 1-23 (or a level name such as\n"
            "      level81, level95 = the Coney hub) at the checkpoint (default 1) instead of the hub; once only,\n"
            "      later loads are the game's own. Saving is disabled for the whole session.\n", prog_name);
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

int main(int argc, char **argv) {
    plat_init("xbrun");
    set_prog_name(argv[0]);
    if (argc < 3) usage();
    const char *xbe_path = argv[1], *disc = argv[2], *syms = NULL, *hdd = "hdd";
    const char *hooks[16], *skips[16];
    int nhooks = 0, nskips = 0, default_skips = 1, fake_mmio = 0, patch_fs_flag = 0, hle_audio = 1, hle_d3d = 1, hle_input = 1, trace = 0;
    const char *input_script = NULL, *input_config = NULL;
    int audio_log = -1, spawn_scale = 1, rate_fixes = 1, text_fixes = 1, sim60 = 0, spray_time = RATEFIX_SPRAY_DEFAULT;
    const char *audio_backend = "sdl", *audio_dump = NULL;
    float sfx_3d_db = 0;
    int xact_controls = 1, glass_args, bloom_args, rain_args, ssao_args, range_args, rp;
    RateProbeOptions rate_probe = {.interval_ms = RATEPROBE_DEFAULT_INTERVAL_MS};
    WarpTarget warp = {0};
    int warp_on = 0, warp_r;
    char warp_err[192];
    for (int i = 3; i < argc; i++) {
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
            else { fprintf(stderr, "--button-icons: xbox, kbm or auto\n"); return 2; }
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
        else if (!strcmp(argv[i], "--stats")) hle_d3d_stats = 1;
        else if (!strcmp(argv[i], "--no-frame-lock")) hle_xapi_frame_lock = 0;
        else if (!strcmp(argv[i], "--spawn-scale") && i + 1 < argc) {
            if ((spawn_scale = spawn_scale_parse(argv[++i])) < 0) usage();
        }
        else if (ratefix_parse_arg(argv[i], &rate_fixes)) {}
        else if (sim60_parse_arg(argv[i], &sim60)) {}
        else if ((warp_r = warp_parse_arg(argc, argv, &i, &warp, &warp_on, warp_err, sizeof warp_err))) {
            if (warp_r < 0) { fprintf(stderr, "%s\n", warp_err); return 2; }
        }
        else if (!strcmp(argv[i], "--spray-time") && i + 1 < argc) {
            if (!ratefix_parse_spray(argv[++i], &spray_time)) usage();
        }
        else if (textfix_parse_arg(argv[i], &text_fixes)) {}
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
        else if ((bloom_args = bloom_arg(&hle_d3d_bloom, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (bloom_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-3, threshold 0-1)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += bloom_args - 1;
        }
        else if ((rain_args = rain_arg(&hle_d3d_rain, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (rain_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (0.25-4)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += rain_args - 1;
        }
        else if ((ssao_args = ssao_arg(&hle_d3d_ssao, argv[i], i + 1 < argc ? argv[i + 1] : NULL)) != 0) {
            if (ssao_args < 0) {
                fprintf(stderr, "%s: invalid value '%s' (strength 0.1-2, radius 0.1-8)\n", argv[i], i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            i += ssao_args - 1;
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
        else if (!strcmp(argv[i], "--no-render-targets")) hle_d3d_render_targets = 0;
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
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audio_backend = argv[++i];
        else if (!strcmp(argv[i], "--audio-dump") && i + 1 < argc) audio_dump = argv[++i];
        else if (!strcmp(argv[i], "--sfx-3d-volume") && i + 1 < argc) {
            if (hle_audio_parse_db(argv[++i], &sfx_3d_db)) {
                fprintf(stderr, "--sfx-3d-volume: '%s' is not a level in dB from %.0f to %+.0f\n", argv[i],
                        HLE_AUDIO_SFX3D_MIN_DB, HLE_AUDIO_SFX3D_MAX_DB);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--no-xact-controls")) xact_controls = 0;
        else if ((rp = rateprobe_parse_arg(argc, argv, &i, &rate_probe)) != 0) {
            if (rp < 0) usage();
        }
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--quiet")) trace = 0;   /* the default; kept for old scripts */
        else usage();
    }
    klog_enabled = trace;   /* per-call logs only with --trace; one-time messages always */
    printf("[start] xbrun %s\n", XBRUN_VERSION);   /* the launcher's bug report reads it */
    printf("[start] language: %s (XC_LANGUAGE %u)\n", nvset_language_name(), nvset_value(NV_LANGUAGE));
    uint8_t *data;
    size_t size;
    Xbe x;
    char err[128];
    printf("[start] reading %s (disc %s)\n", xbe_path, disc);
    if (xbe_read_file(xbe_path, &data, &size)) { perror(xbe_path); return 1; }
    if (xbe_parse(&x, data, size, err, sizeof err)) { fprintf(stderr, "%s: %s\n", xbe_path, err); return 1; }
    if (syms) printf("[load] %d symbols from %s\n", symbols_load(syms), syms);
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
    kernel_init();
    patch_thunks(&x);
    if (!nhooks && syms && !hle_d3d) hooks[nhooks++] = "D3D8__Direct3D_CreateDevice";
    for (int i = 0; i < nhooks; i++) add_hook(hooks[i]);
    if (hle_audio) {   /* XACT/DirectSound API boundary on the host instead of MCPX APU MMIO */
        hle_audio_log_limit = audio_log >= 0 ? audio_log : trace ? 32 : 0;
        hle_audio_install(syms ? symbol_lookup : NULL);
        hle_audio_handle_fd = handle_fd;
        /* The mixer always runs (it advances DirectSound play cursors); "null" just has no device. */
        printf("[audio] backend: %s\n", audio_open(audio_backend, audio_dump));
        hle_audio_set_sfx_3d_volume(sfx_3d_db);
        printf("[audio] 3D sound-effect volume: %.1f dB\n", sfx_3d_db);
        hle_audio_set_xact_controls(xact_controls);
        printf("[audio] xact controls: %s\n", xact_controls ? "on" : "off");
        hle_d3d_on_exit = audio_close;
    }
    if (hle_d3d) {   /* D3D8 API boundary on the host instead of NV2A MMIO; frames on the main thread */
        hle_d3d_log_limit = trace ? 1 : 0;
        hle_d3d_install(syms ? symbol_lookup : NULL);
        kobj_idle_hook = hle_d3d_service;
    }
    if (input_script) {
        char err[128];
        if (input_script_parse(input_script, err, sizeof err)) { fprintf(stderr, "--input-script: %s\n", err); return 2; }
    }
    input_map_defaults();   /* PC key layout; a key map file changes it (invalid lines are reported, not fatal) */
    if (input_config && input_map_load_file(input_config) < 0) { perror(input_config); return 2; }
    if (!input_config) input_map_load_file("warriors-input.cfg");   /* optional */
    if (hle_d3d_button_icons != HLE_ICONS_XBOX) {
        int n = hle_d3d_glyphs_load(disc);
        if (n < 0) printf("[glyphs] no glyph table in %s/XBoxWad.idx (paks\\global.pak): Xbox button icons kept\n", disc);
        else printf("[glyphs] %d glyph rects from paks\\global.pak; button icons: %s\n", n,
                    hle_d3d_button_icons == HLE_ICONS_KBM ? "keyboard/mouse" : "auto (last used device)");
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
    /* Experimental: the world updates on every 1/60 s step with a halved time step (hle_sim60.h). */
    int sim60_active = hle_sim60_install(sim60);
    /* Diagnostic: game-time snapshots for the rate diff between both world rates (rateprobe.h). */
    if (rate_probe.path[0]) rate_probe_start(&x, &rate_probe, sim60_active);
    /* Start a chosen mission when a profile is loaded; saving off for the session (hle_warp.h). */
    hle_warp_install(warp_on, &warp);
    /* HUD widgets that step once per rendered frame, authored for 30 fps, the world-side graffiti
     * fixes when the 60 Hz world is active, and the spray-can duration comfort setting
     * (hle_ratefix.h). */
    hle_ratefix_install(rate_fixes, spray_time, sim60_active);
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
    kobj_start_entry(x.entry, x.stack_commit);
    kobj_service_loop();
}
