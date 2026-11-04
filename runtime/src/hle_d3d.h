/* High-level emulation (HLE) of the Direct3D 8 API boundary: every D3D8 library entry point the
 * title calls is replaced with a host function, so the statically linked D3D8 code never
 * programs the NV2A (MMIO at 0xFD000000). Device, surfaces, textures and buffers are guest-memory
 * objects with the Xbox D3DResource layout; Clear fills the render target, Swap posts the back
 * buffer to the host video backend (src/video.h). Title code that writes D3D globals inline
 * (D3D_g_RenderState, deferred state arrays, dirty flags) keeps writing them at their original
 * addresses; nothing consumes them yet (the title's blend and depth states arrive through
 * SetRenderState_Simple). */
#ifndef HLE_D3D_H
#define HLE_D3D_H
#include "hle_audio.h"   /* HleLookup, HLE_SIG_LEN */
#include "renderer.h"    /* RState */
#include "hud_glass.h"   /* HudGlassConfig */
#include "bloom.h"       /* BloomConfig */
#include "shadow_range.h" /* ShadowRangeConfig */
#include "rain.h"        /* RainConfig */
#include "ssao.h"        /* SsaoConfig */
#include "soft_particles.h"   /* SoftConfig */
#include <stdint.h>

/* Patches the D3D entry points (same rules as hle_audio_install: an entry is patched only when
 * its first bytes match and its symbol, if known, resolves to the same address). */
int hle_d3d_install(HleLookup lookup);
void hle_d3d_summary(void);              /* per-function call counts, frames, last clear color */
extern int hle_d3d_log_limit;            /* log lines per function after the first Swap (default 1) */
extern int hle_d3d_frame_limit;          /* exit after this many presented frames (0 = no limit) */
extern const char *hle_d3d_dump_path;    /* BMP of the last presented frame, written on exit */
extern const char *hle_d3d_video;        /* video backend preference ("sdl" or "null") */
extern const char *hle_d3d_renderer;     /* host renderer: "vulkan" (default, CPU path if it fails) or "none" */
extern int hle_d3d_stats;                /* --stats: a timing line per 60 presented frames */
/* HD output, host side only (the title keeps its 640x480 device):
 * --render-scale N, --resolution WxH, --msaa N, --aniso N, --widescreen. */
extern int hle_d3d_render_scale, hle_d3d_res_w, hle_d3d_res_h, hle_d3d_msaa, hle_d3d_aniso, hle_d3d_widescreen;
/* Graphics options: --supersample N (internal render size N x the output, box-filtered down;
 * RendererConfig.supersample), --shadow-scale N (the title's render targets at N x their scaled
 * size) and --shadow-soft (3x3 PCF on the character shadows). 0 = off / the default. */
extern int hle_d3d_supersample, hle_d3d_shadow_scale, hle_d3d_shadow_soft;
extern int hle_d3d_sharpen; /* --sharpen 0-100 (RendererConfig.sharpen) */
/* --present auto|mailbox|fifo|immediate|readback: auto = a Vulkan swapchain in the SDL window
 * (mailbox when available, else fifo); readback = the renderer's image read back and shown by
 * the video backend (always used with --video x11|null, --dump-frame or when the swapchain
 * cannot be made). --fullscreen starts in desktop fullscreen. */
extern const char *hle_d3d_present;
/* --frames-in-flight N (1-3, default 2): frames the GPU may still be executing when the swapchain
 * path moves on to the next one (RendererConfig.frames_in_flight); 1 = wait after each present. */
extern int hle_d3d_frames_in_flight;
/* --hud-hd / --hud-glass and their tunables (src/hud_glass.h; off by default): the target health
 * bars are restyled (thicker, outline, gradient fill) or drawn as low-opacity bars on a
 * frosted-glass panel (renderer_glass); the rage meter (player panel) is never changed. */
extern HudGlassConfig hle_d3d_glass;
/* --bloom, --bloom-strength, --bloom-threshold (src/bloom.h; off by default): glow of the 3D scene,
 * split from the HUD after the frame's last depth-tested back-buffer draw (renderer_bloom). */
extern BloomConfig hle_d3d_bloom;
/* --rain-hd, --rain-hd-density (src/rain.h; off by default): while the title draws its rain layers,
 * they are replaced by the renderer's procedural rain (renderer_rain) in their place. */
extern RainConfig hle_d3d_rain;
/* --ssao, --ssao-strength, --ssao-radius (src/ssao.h; off by default): ambient occlusion of the
 * opaque 3D world, applied after its last opaque draw (renderer_ssao), before effects and HUD. */
extern SsaoConfig hle_d3d_ssao;
/* --soft-particles, --soft-particles-distance (src/soft_particles.h; off by default): the title's
 * camera-facing particle quads after the opaque world fade where they meet the scene behind them
 * (renderer_soft, RENDERER_SOFT). */
extern SoftConfig hle_d3d_soft;
/* --shadow-range, --shadow-range-dist (src/shadow_range.h; off by default): character shadows fade
 * out on receivers more than the distance below the caster (RendererConfig.shadow_range). */
extern ShadowRangeConfig hle_d3d_shadow_range;
extern int hle_d3d_fullscreen;
/* --dump-textures DIR: every unique texture sampled by a draw as DIR/<hash>.png + manifest.csv;
 * --texture-pack DIR (repeatable, up to HLE_D3D_MAX_PACKS): replaces textures whose content hash
 * has a DIR/<hash>.png (src/texpack.h); with several packs the first one that has a hash wins. */
#define HLE_D3D_MAX_PACKS 8
extern const char *hle_d3d_dump_textures, *hle_d3d_texture_packs[HLE_D3D_MAX_PACKS];
extern int hle_d3d_texture_pack_count;
/* --mesh-pack DIR: replaces skinned character meshes whose content hash has a DIR/<hash>.wmesh
 * (src/meshpack.h); NULL (the default) leaves every draw as the title issues it. */
extern const char *hle_d3d_mesh_pack;
/* --button-icons xbox|kbm|auto: the in-game button prompts as the game's own (or the first
 * texture pack's) glyphs, as key caps and mouse drawings for the current key map
 * (src/kbm_glyphs.h), or following the last used device (keyboard/mouse or controller; the two
 * atlases are cached under separate keys, so a switch costs nothing after the first use). The
 * cell rects come from the game folder (hle_d3d_glyphs_load); without them icons stay Xbox. */
enum { HLE_ICONS_XBOX, HLE_ICONS_KBM, HLE_ICONS_AUTO };
extern int hle_d3d_button_icons;
int hle_d3d_glyphs_load(const char *disc_dir);   /* rect count, or -1 (icons stay Xbox) */
/* Title render targets (default on; --no-render-targets turns it off): draws and clears into the
 * title's own render targets and depth surfaces (the shadow map) are replayed into offscreen
 * renderer surfaces, which stage textures over the same memory then sample.
 * Off: those draws are skipped and such textures sample white, as before. */
extern int hle_d3d_render_targets;
extern int hle_d3d_vp_threads;
extern int hle_d3d_dump_every;   /* --vp-threads N: vertex program worker threads (-1 auto, 0 off) */
/* Renderer target size for a bw x bh back buffer: N x the back buffer with --render-scale, the
 * largest rectangle of the frame's aspect inside --resolution, else the back buffer. The aspect
 * is the back buffer's, or 16:9 with --widescreen (the title then renders anamorphic frames). */
void hle_d3d_output_size(int bw, int bh, int *ow, int *oh);
extern void (*hle_d3d_on_exit)(void);  /* called once before the process exits after the frame limit or window close */

/* Host main thread: opens the window on the first frame, presents new frames, pumps window
 * events and exits the process when the window is closed or the frame limit is reached. */
void hle_d3d_service(void);

uint32_t hle_d3d_frame_count(void);
uint32_t hle_d3d_clear_color(void);      /* color of the last Clear with a target flag */
/* Frames handed to the video backend by hle_d3d_service, and Swaps dropped unpresented. */
void hle_d3d_present_stats(uint32_t *presented, uint32_t *dropped);
/* Copies the last posted frame (XRGB8888, tightly packed) into a malloc'd buffer the caller
 * frees; returns NULL before the first Swap. */
uint32_t *hle_d3d_last_frame(int *w, int *h);

/* Current output state (blend, color mask, depth write/func, cull) recorded with each draw. */
void hle_d3d_render_state(RState *out);
/* Draws recorded for the frame being drawn; *v / *nv / *ni = the last draw's vertices and its
 * index count (tests). */
uint32_t hle_d3d_recorded(const RVertex **v, uint32_t *nv, uint32_t *ni);
/* The last recorded draw of the frame being drawn (tests): its offscreen target (guest physical
 * addresses of the color and depth surfaces, 0 = none / the back buffer), the render-target
 * surface each stage samples (0 = none), its pixel shader's texture modes and shadow compare
 * function, and its viewport size. Returns 0 when the frame has no draw. */
typedef struct {
    uint32_t target_color, target_depth;
    uint32_t stage_surface[4];
    uint32_t tex_modes, shadow_func;
    uint32_t vp_w, vp_h;
} HleD3dDrawInfo;
int hle_d3d_last_draw(HleD3dDrawInfo *d);

/* Table access for tests. */
int hle_d3d_entry_count(void);
uint32_t hle_d3d_entry_addr(int i);
int hle_d3d_entry_argbytes(int i);
const char *hle_d3d_entry_name(int i);
const uint8_t *hle_d3d_entry_sig(int i);   /* HLE_SIG_LEN bytes */

/* Guest addresses in this XBE's D3D8 library (from XbSymbolDatabase and disassembly). */
#define D3D_G_PDEVICE 0x0037d978u   /* D3D8__D3D_g_pDevice */
#define D3D_DEVICE 0x0037d980u      /* the static CDevice; +0 push-buffer put, +4 end */
#endif
